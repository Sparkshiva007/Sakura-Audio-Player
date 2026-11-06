/*
 * Sakura Music Player - Direct SPI MicroSD Driver for MAX32655
 * Option B: Hardware-direct SPI block driver for FatFS
 */

#include "sd_spi.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "mxc_device.h"
#include "mxc_sys.h"
#include "mxc_delay.h"
#include "gpio.h"
#include "spi.h"

#include "ff.h"
#include "diskio.h"

/* Pin definitions on MAX32655FTHR */
#define SD_CS_PORT          MXC_GPIO0
#define SD_CS_PIN           MXC_GPIO_PIN_4    /* P0.4 */

#define SD_MOSI_PIN         MXC_GPIO_PIN_5    /* P0.5 */
#define SD_MISO_PIN         MXC_GPIO_PIN_6    /* P0.6 */
#define SD_SCK_PIN          MXC_GPIO_PIN_7    /* P0.7 */

#define SD_DET_PORT         MXC_GPIO0
#define SD_DET_PIN          MXC_GPIO_PIN_12   /* P0.12 (Active-Low) */

#define FLASH_CS_PORT       MXC_GPIO0
#define FLASH_CS_PIN        MXC_GPIO_PIN_11   /* P0.11 */
#define FLASH_WP_PIN        MXC_GPIO_PIN_8    /* P0.8 */
#define FLASH_HOLD_PIN      MXC_GPIO_PIN_9    /* P0.9 */

/* SD SPI Commands */
#define CMD0   0    /* GO_IDLE_STATE */
#define CMD8   8    /* SEND_IF_COND */
#define CMD9   9    /* SEND_CSD */
#define CMD10  10   /* SEND_CID */
#define CMD12  12   /* STOP_TRANSMISSION */
#define CMD16  16   /* SET_BLOCKLEN */
#define CMD17  17   /* READ_SINGLE_BLOCK */
#define CMD24  24   /* WRITE_BLOCK */
#define CMD55  55   /* APP_CMD */
#define CMD58  58   /* READ_OCR */
#define ACMD41 41   /* SD_SEND_OP_COND */

volatile bool g_card_is_sdhc = false;
volatile bool g_card_ready = false;
volatile uint32_t g_sector_count = 31250000;
volatile int g_sd_init_stage = 0;
volatile uint8_t g_dbg_cmd0_r1 = 0xFF;
volatile uint8_t g_dbg_cmd8_r1 = 0xFF;
volatile uint8_t g_dbg_cmd8_pattern = 0x00;
volatile uint8_t g_dbg_acmd41_r1 = 0xFF;
volatile uint8_t g_dbg_cmd58_r1 = 0xFF;
volatile uint8_t g_dbg_ocr[4] = {0};
volatile uint8_t g_dbg_cmd17_r1 = 0xFF;
volatile uint8_t g_dbg_token = 0xFF;
volatile uint32_t g_dbg_token_wait = 0;

static inline void sd_cs_select(void)
{
    MXC_GPIO_OutClr(SD_CS_PORT, SD_CS_PIN);
}

static inline void sd_cs_deselect(void)
{
    MXC_GPIO_OutSet(SD_CS_PORT, SD_CS_PIN);
}

static uint8_t sd_spi_xfer(uint8_t tx)
{
    uint8_t rx = 0xFF;
    mxc_spi_req_t req;
    memset(&req, 0, sizeof(req));
    req.spi = MXC_SPI0;
    req.ssIdx = 0;
    req.ssDeassert = 0;
    req.txData = &tx;
    req.rxData = &rx;
    req.txLen = 1;
    req.rxLen = 1;
    MXC_SPI_MasterTransaction(&req);
    return rx;
}

static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
    sd_cs_deselect();
    sd_spi_xfer(0xFF);
    sd_cs_select();

    sd_spi_xfer(0x40 | cmd);
    sd_spi_xfer((arg >> 24) & 0xFF);
    sd_spi_xfer((arg >> 16) & 0xFF);
    sd_spi_xfer((arg >> 8) & 0xFF);
    sd_spi_xfer(arg & 0xFF);
    sd_spi_xfer(crc);

    /* Wait for response byte (bit 7 must be 0) */
    for (int i = 0; i < 64; i++) {
        uint8_t r = sd_spi_xfer(0xFF);
        if ((r & 0x80) == 0) {
            return r;
        }
    }
    return 0xFF; /* Timeout */
}

int sd_spi_hw_init(void)
{
    /* 1. Enable MicroSD Socket VDD Power via P0.15 (Active-High) */
    mxc_gpio_cfg_t sd_pwr = {
        MXC_GPIO0,
        MXC_GPIO_PIN_15,
        MXC_GPIO_FUNC_OUT,
        MXC_GPIO_PAD_NONE,
        MXC_GPIO_VSSEL_VDDIO,
        MXC_GPIO_DRVSTR_0
    };
    MXC_GPIO_Config(&sd_pwr);
    MXC_GPIO_OutSet(sd_pwr.port, sd_pwr.mask);
    MXC_Delay(MXC_DELAY_MSEC(50));

    /* 2. Isolate onboard QSPI Flash to prevent bus contention on MISO */
    mxc_gpio_cfg_t flash_pins = {
        FLASH_CS_PORT,
        FLASH_CS_PIN | FLASH_WP_PIN | FLASH_HOLD_PIN,
        MXC_GPIO_FUNC_OUT,
        MXC_GPIO_PAD_NONE,
        MXC_GPIO_VSSEL_VDDIOH,
        MXC_GPIO_DRVSTR_0
    };
    MXC_GPIO_Config(&flash_pins);
    MXC_GPIO_OutSet(flash_pins.port, flash_pins.mask);

    /* 3. Configure Card Detect Switch (P0.12) */
    mxc_gpio_cfg_t det_pin = {
        SD_DET_PORT,
        SD_DET_PIN,
        MXC_GPIO_FUNC_IN,
        MXC_GPIO_PAD_WEAK_PULL_UP,
        MXC_GPIO_VSSEL_VDDIOH,
        MXC_GPIO_DRVSTR_0
    };
    MXC_GPIO_Config(&det_pin);

    /* 4. Configure Chip Select (P0.4) as manual GPIO output */
    mxc_gpio_cfg_t cs_pin = {
        SD_CS_PORT,
        SD_CS_PIN,
        MXC_GPIO_FUNC_OUT,
        MXC_GPIO_PAD_NONE,
        MXC_GPIO_VSSEL_VDDIOH,
        MXC_GPIO_DRVSTR_0
    };
    MXC_GPIO_Config(&cs_pin);
    sd_cs_deselect();

    /* 4. Configure SPI0 Pins (MOSI P0.5, MISO P0.6, SCK P0.7) */
    mxc_gpio_cfg_t spi0_pins = {
        MXC_GPIO0,
        SD_MOSI_PIN | SD_MISO_PIN | SD_SCK_PIN,
        MXC_GPIO_FUNC_ALT1,
        MXC_GPIO_PAD_WEAK_PULL_UP,
        MXC_GPIO_VSSEL_VDDIOH,
        MXC_GPIO_DRVSTR_0
    };
    MXC_GPIO_Config(&spi0_pins);

    /* 5. Initialize SPI0 Master Peripheral at 400 kHz for SD card init */
    MXC_SYS_ClockEnable(MXC_SYS_PERIPH_CLOCK_SPI0);

    mxc_spi_pins_t spi_pins = {
        .clock  = true,
        .miso   = true,
        .mosi   = true,
        .ss0    = false,
        .ss1    = false,
        .ss2    = false,
        .sdio2  = false,
        .sdio3  = false,
        .vddioh = true
    };

    int err = MXC_SPI_Init(MXC_SPI0, 1, 0, 0, 0, 400000, spi_pins);
    if (err != E_NO_ERROR) {
        return -1;
    }

    /* Enforce VDDIOH (3.3V) level on all SD lines */
    MXC_GPIO_SetVSSEL(SD_CS_PORT, MXC_GPIO_VSSEL_VDDIOH, SD_CS_PIN | FLASH_CS_PIN | SD_DET_PIN);
    MXC_GPIO_SetVSSEL(MXC_GPIO0, MXC_GPIO_VSSEL_VDDIOH, SD_MOSI_PIN | SD_MISO_PIN | SD_SCK_PIN);
    MXC_GPIO_Config(&spi0_pins);

    MXC_SPI_SetMode(MXC_SPI0, SPI_MODE_0);
    MXC_SPI_SetDataSize(MXC_SPI0, 8);
    MXC_SPI_SetWidth(MXC_SPI0, SPI_WIDTH_STANDARD);
    MXC_SPI_SetDefaultTXData(MXC_SPI0, 0xFF);

    return 0;
}

bool sd_spi_is_card_inserted(void)
{
    return (MXC_GPIO_InGet(SD_DET_PORT, SD_DET_PIN) == 0);
}

int sd_spi_card_init(void)
{
    g_sd_init_stage = 1;
    int err = sd_spi_hw_init();
    if (err != 0) {
        g_sd_init_stage = -1;
        return err;
    }

    if (!sd_spi_is_card_inserted()) {
        g_card_ready = false;
        g_sd_init_stage = -2;
        return -2;
    }

    g_sd_init_stage = 2;

    /* 1. Send at least 74 clock cycles with CS deasserted (16 bytes of 0xFF) */
    sd_cs_deselect();
    for (int i = 0; i < 20; i++) {
        sd_spi_xfer(0xFF);
    }
    MXC_Delay(MXC_DELAY_MSEC(10));

    /* 2. CMD0: Reset card to SPI idle state */
    uint8_t r1 = 0xFF;
    for (int try_cnt = 0; try_cnt < 100; try_cnt++) {
        r1 = sd_send_cmd(CMD0, 0, 0x95);
        sd_cs_deselect();
        sd_spi_xfer(0xFF);
        g_dbg_cmd0_r1 = r1;
        if (r1 == 0x01) {
            break;
        }
        MXC_Delay(MXC_DELAY_MSEC(2));
    }

    if (r1 != 0x01) {
        g_sd_init_stage = -4;
        return -1;
    }

    g_sd_init_stage = 4;

    /* 3. CMD8: Send Interface Condition (voltage 2.7-3.6V, pattern 0xAA) */
    r1 = sd_send_cmd(CMD8, 0x000001AA, 0x87);
    g_dbg_cmd8_r1 = r1;
    uint8_t cmd8_resp[4] = {0};
    if (r1 == 0x01) {
        for (int i = 0; i < 4; i++) {
            cmd8_resp[i] = sd_spi_xfer(0xFF);
        }
        g_dbg_cmd8_pattern = cmd8_resp[3];
    }
    sd_cs_deselect();
    sd_spi_xfer(0xFF);

    g_sd_init_stage = 5;

    /* 4. ACMD41 loop: Initialize card until ready (R1 == 0x00) */
    uint32_t acmd_arg = (cmd8_resp[3] == 0xAA) ? 0x40000000 : 0x00000000;
    r1 = 0xFF;

    for (int i = 0; i < 1000; i++) {
        /* Send CMD55 first */
        r1 = sd_send_cmd(CMD55, 0, 0x65);
        sd_cs_deselect();
        sd_spi_xfer(0xFF);

        /* Then send ACMD41 */
        r1 = sd_send_cmd(ACMD41, acmd_arg, 0x77);
        sd_cs_deselect();
        sd_spi_xfer(0xFF);
        g_dbg_acmd41_r1 = r1;

        if (r1 == 0x00) {
            break;
        }
        MXC_Delay(MXC_DELAY_MSEC(2));
    }

    if (r1 != 0x00) {
        g_sd_init_stage = -6;
        return -1;
    }

    g_sd_init_stage = 6;

    /* 5. CMD58: Read OCR to check Card Capacity Status (CCS) bit 30 */
    r1 = sd_send_cmd(CMD58, 0, 0xFD);
    g_dbg_cmd58_r1 = r1;
    uint8_t ocr[4] = {0};
    if (r1 == 0x00 || r1 == 0x01) {
        for (int i = 0; i < 4; i++) {
            ocr[i] = sd_spi_xfer(0xFF);
            g_dbg_ocr[i] = ocr[i];
        }
    }
    sd_cs_deselect();
    sd_spi_xfer(0xFF);

    if (ocr[0] & 0x40) {
        g_card_is_sdhc = true;
    } else {
        g_card_is_sdhc = false;
    }

    /* 6. Force block length to 512 bytes for all cards */
    r1 = sd_send_cmd(CMD16, 512, 0x15);
    sd_cs_deselect();
    sd_spi_xfer(0xFF);

    /* 7. Switch SPI0 to High Speed (10 MHz - rock solid for SD SPI) */
    MXC_SPI_SetFrequency(MXC_SPI0, 10000000);

    g_card_ready = true;
    g_sector_count = 31250000; /* ~16 GB default */
    g_sd_init_stage = 8;

    return 0;
}

bool sd_spi_is_ready(void)
{
    return g_card_ready;
}

uint32_t sd_spi_get_sector_count(void)
{
    return g_sector_count;
}

static const uint8_t s_spi_ff_512[512] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static void sd_spi_read_block(uint8_t *dest, int len)
{
    mxc_spi_req_t req;
    memset(&req, 0, sizeof(req));
    req.spi = MXC_SPI0;
    req.ssIdx = 0;
    req.ssDeassert = 0;
    req.txData = (uint8_t *)s_spi_ff_512;
    req.rxData = dest;
    req.txLen = len;
    req.rxLen = len;
    MXC_SPI_MasterTransaction(&req);
}

int sd_spi_read_sector(uint32_t sector, uint8_t *buf)
{
    if (!g_card_ready) {
        int err = sd_spi_card_init();
        if (err != 0) return err;
    }

    uint32_t addr = g_card_is_sdhc ? sector : (sector << 9);

    uint8_t r1 = sd_send_cmd(CMD17, addr, 0x01);
    g_dbg_cmd17_r1 = r1;
    if (r1 != 0x00) {
        sd_cs_deselect();
        sd_spi_xfer(0xFF);
        return -1;
    }

    /* Wait for Data Start Token (0xFE) */
    uint8_t token = 0xFF;
    for (int i = 0; i < 200000; i++) {
        token = sd_spi_xfer(0xFF);
        if (token == 0xFE) {
            g_dbg_token_wait = (uint32_t)i;
            break;
        }
    }
    g_dbg_token = token;

    if (token != 0xFE) {
        sd_cs_deselect();
        sd_spi_xfer(0xFF);
        return -1;
    }

    /* Read 512 bytes in one fast hardware SPI transaction (~300 microseconds) */
    sd_spi_read_block(buf, 512);

    /* Read 2-byte CRC16 */
    uint8_t crc[2];
    sd_spi_read_block(crc, 2);

    sd_cs_deselect();
    sd_spi_xfer(0xFF);

    return 0;
}

int sd_spi_read_sectors(uint32_t start_sector, uint8_t *buf, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        int err = sd_spi_read_sector(start_sector + i, buf + (i * 512));
        if (err != 0) {
            return err;
        }
    }
    return 0;
}

static void sd_spi_write_block(const uint8_t *src, uint32_t len)
{
    mxc_spi_req_t req;
    memset(&req, 0, sizeof(req));
    req.spi = MXC_SPI0;
    req.ssIdx = 0;
    req.ssDeassert = 0;
    req.txData = (uint8_t *)src;
    req.rxData = NULL;
    req.txLen = len;
    req.rxLen = 0;
    MXC_SPI_MasterTransaction(&req);
}

int sd_spi_write_sector(uint32_t sector, const uint8_t *buf)
{
    if (!g_card_ready) {
        int err = sd_spi_card_init();
        if (err != 0) return err;
    }

    uint32_t addr = g_card_is_sdhc ? sector : (sector << 9);

    /* Send CMD24 (WRITE_BLOCK) */
    uint8_t r1 = sd_send_cmd(CMD24, addr, 0x01);
    if (r1 != 0x00) {
        sd_cs_deselect();
        sd_spi_xfer(0xFF);
        return -1;
    }

    /* Send 1 dummy byte + Data Start Token (0xFE) */
    sd_spi_xfer(0xFF);
    sd_spi_xfer(0xFE);

    /* Transmit 512 bytes payload */
    sd_spi_write_block(buf, 512);

    /* Transmit 2 dummy CRC bytes */
    sd_spi_xfer(0xFF);
    sd_spi_xfer(0xFF);

    /* Read Data Response token */
    uint8_t resp = sd_spi_xfer(0xFF);
    if ((resp & 0x1F) != 0x05) {
        /* Data rejected */
        sd_cs_deselect();
        sd_spi_xfer(0xFF);
        return -2;
    }

    /* Wait while card programming (MISO held low 0x00 until ready 0xFF) */
    int timeout = 500000;
    while (timeout-- > 0) {
        if (sd_spi_xfer(0xFF) == 0xFF) break;
    }

    sd_cs_deselect();
    sd_spi_xfer(0xFF);

    if (timeout <= 0) return -3; /* Write timeout */
    return 0;
}

int sd_spi_write_sectors(uint32_t start_sector, const uint8_t *buf, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        int err = sd_spi_write_sector(start_sector + i, buf + (i * 512));
        if (err != 0) {
            return err;
        }
    }
    return 0;
}

/* =========================================================================
 * ChaN FatFS Low-Level Disk I/O Glue
 * ========================================================================= */

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    if (sd_spi_card_init() == 0) {
        return 0;
    }
    return STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    return g_card_ready ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != 0 || count == 0) return RES_PARERR;
    if (sd_spi_read_sectors(sector, buff, count) == 0) {
        return RES_OK;
    }
    return RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != 0 || count == 0) return RES_PARERR;
    if (sd_spi_write_sectors(sector, (const uint8_t *)buff, count) == 0) {
        return RES_OK;
    }
    return RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0) return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(DWORD *)buff = g_sector_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

DWORD get_fattime(void)
{
    return 0;
}
