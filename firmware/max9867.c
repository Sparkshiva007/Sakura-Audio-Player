/******************************************************************************
 *
 * Copyright (C) 2022-2023 Maxim Integrated Products, Inc. (now owned by 
 * Analog Devices, Inc.),
 * Copyright (C) 2023-2024 Analog Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ******************************************************************************/

#include "max9867.h"

#include <i2c.h>
#include <gpio.h>
#include <mxc_delay.h>
#include <mxc_device.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define I2C_FREQ 100000

#define MAX9867_ADDR 0x18
#define MAX9867_REVID 0x42

/* MAX32655 Featherboard I2C1 Pins: Port 0 Pin 16 (SCL), Pin 17 (SDA) */
static const mxc_gpio_cfg_t s_fthr_i2c1 = {
    .port   = MXC_GPIO0,
    .mask   = MXC_GPIO_PIN_16 | MXC_GPIO_PIN_17,
    .func   = MXC_GPIO_FUNC_ALT1,
    .pad    = MXC_GPIO_PAD_WEAK_PULL_UP,
    .vssel  = MXC_GPIO_VSSEL_VDDIO,
    .drvstr = MXC_GPIO_DRVSTR_0
};

// Register addresses
#define MAX9867_00_STATUS 0x00
#define MAX9867_01_JACKSENSE 0x01
#define MAX9867_02_AUX_HIGH 0x02
#define MAX9867_03_AUX_LOW 0x03
#define MAX9867_04_INT_EN 0x04
#define MAX9867_05_SYS_CLK 0x05
#define MAX9867_06_CLK_HIGH 0x06
#define MAX9867_07_CLK_LOW 0x07
#define MAX9867_08_DAI_FORMAT 0x08
#define MAX9867_09_DAI_CLOCK 0x09
#define MAX9867_0A_DIG_FILTER 0x0A
#define MAX9867_0B_SIDETONE 0x0B
#define MAX9867_0C_LVL_DAC 0x0C
#define MAX9867_0D_LVL_ADC 0x0D
#define MAX9867_0E_LVL_LINE_IN_LEFT 0x0E
#define MAX9867_0F_LVL_LINE_IN_RIGHT 0x0F
#define MAX9867_10_VOL_LEFT 0x10
#define MAX9867_11_VOL_RIGHT 0x11
#define MAX9867_12_MIC_GAIN_LEFT 0x12
#define MAX9867_13_MIC_GAIN_RIGHT 0x13
#define MAX9867_14_ADC_INPUT 0x14
#define MAX9867_15_MIC 0x15
#define MAX9867_16_MODE 0x16
#define MAX9867_17_PWR_SYS 0x17
#define MAX9867_FF_REV_ID 0xFF

// MAX9867_04_INT_EN
#define MAX9867_ICLD (1 << 7)
#define MAX9867_SDODLY (1 << 2)

// MAX9867_05_SYS_CLK
#define MAX9867_PSCLK_POS 4

// MAX9867_06_CLK_HIGH
#define MAX9867_PLL (1 << 7)

// MAX9867_08_DAI_FORMAT
#define MAX9867_MAS (1 << 7)
#define MAX9867_WCI (1 << 6)
#define MAX9867_BCI (1 << 5)
#define MAX9867_DLY (1 << 4)
#define MAX9867_HIZOFF (1 << 3)
#define MAX9867_TDM (1 << 2)

// MAX9867_0D_LVL_ADC
#define MAX9867_AVL_POS 4
#define MAX9867_AVR_POS 0

// MAX9867_0E_LVL_LINE_IN_LEFT
// MAX9867_0F_LVL_LINE_IN_RIGHT
#define MAX9867_LI_MUTE (1 << 6)
#define MAX9867_LI_GAIN_POS 0

// MAX9867_10_VOL_LEFT
// MAX9867_11_VOL_RIGHT
#define MAX9867_VOL_POS 0

// MAX9867_15_MIC
#define MAX9867_DIGMICR_POS 4

// MAX9867_16_MODE
#define MAX9867_HPMODE_POS 0
#define MAX9867_STEREO_NOCAP_CLICKLESS 2
#define MAX9867_STEREO_SE_CLICKLESS 4

// MAX9867_17_PWR_SYS
#define MAX9867_SHDN (1 << 7)
#define MAX9867_DALEN (1 << 3)
#define MAX9867_DAREN (1 << 2)
#define MAX9867_ADLEN (1 << 1)

#define MHZ(N) (N * 1000000)

static mxc_i2c_req_t i2c_req;

static int reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };

    i2c_req.tx_buf = buf;
    i2c_req.tx_len = sizeof(buf);
    i2c_req.rx_buf = NULL;
    i2c_req.rx_len = 0;
    i2c_req.restart = 0;

    int ret = E_BUSY;
    for (int retry = 0; retry < 10; retry++) {
        ret = MXC_I2C_MasterTransaction(&i2c_req);
        if (ret != E_BUSY) break;
        MXC_Delay(MXC_DELAY_USEC(200));
    }
    return ret;
}

static int reg_read(uint8_t reg, uint8_t *dest)
{
    uint8_t buf[1] = { reg };

    i2c_req.tx_buf = buf;
    i2c_req.tx_len = sizeof(buf);
    i2c_req.rx_buf = dest;
    i2c_req.rx_len = 1;
    i2c_req.restart = 0;

    int ret = E_BUSY;
    for (int retry = 0; retry < 10; retry++) {
        ret = MXC_I2C_MasterTransaction(&i2c_req);
        if (ret != E_BUSY) break;
        MXC_Delay(MXC_DELAY_USEC(200));
    }
    return ret;
}

int max9867_status(void)
{
    int err;
    uint8_t status;

    if ((err = reg_read(MAX9867_00_STATUS, &status)) != E_NO_ERROR)
        return err;

    return status;
}

int max9867_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 31) vol = 31;
    uint8_t reg_val = (uint8_t)((31 - vol) & 0x1F);
    uint8_t buf[3] = { MAX9867_10_VOL_LEFT, (uint8_t)(reg_val << MAX9867_VOL_POS), (uint8_t)(reg_val << MAX9867_VOL_POS) };
    i2c_req.tx_buf = buf;
    i2c_req.tx_len = 3;
    i2c_req.rx_buf = NULL;
    i2c_req.rx_len = 0;
    i2c_req.restart = 0;
    return MXC_I2C_MasterTransaction(&i2c_req);
}

volatile uint8_t g_dbg_rev_id = 0xFF;
volatile int g_dbg_rev_err = -999;
volatile int g_dbg_fail_line = 0;
volatile int g_dbg_fail_reg = 0;

#define WR_REG(r, v) do { \
    err = reg_write(r, v); \
    if (err != E_NO_ERROR) { \
        g_dbg_fail_line = __LINE__; \
        g_dbg_fail_reg = (r); \
        return err; \
    } \
} while (0)

int max9867_init(mxc_i2c_regs_t *i2c_inst, int mclk, int lrclk)
{
    int err;
    uint8_t rev_id = 0;
    int psclk;

    MXC_I2C_Shutdown(i2c_inst);
    if ((err = MXC_I2C_Init(i2c_inst, TRUE, 0)) != E_NO_ERROR)
        return err;

    /* CRITICAL: MaximSDK MXC_I2C_Init defaults I2C1 to EvKit pins P1.28/P1.29.
     * Restore MAX32655 Featherboard pins: Port 0 Pin 16 (SCL) & Pin 17 (SDA) */
    MXC_GPIO_Config(&s_fthr_i2c1);
    MXC_I2C_SetFrequency(i2c_inst, 100000);
    MXC_I2C_SetTimeout(i2c_inst, 25000);

    i2c_req.i2c = i2c_inst;
    i2c_req.addr = MAX9867_ADDR;
    i2c_req.restart = 0;
    i2c_req.callback = NULL;

    MXC_Delay(MXC_DELAY_MSEC(10));

    for (int retry = 0; retry < 5; retry++) {
        err = reg_read(MAX9867_FF_REV_ID, &rev_id);
        g_dbg_rev_id = rev_id;
        g_dbg_rev_err = err;
        if (err == E_NO_ERROR && rev_id == MAX9867_REVID)
            break;
        MXC_Delay(MXC_DELAY_MSEC(5));
    }

    if (rev_id != MAX9867_REVID) {
        printf("[CODEC] Read REV_ID=0x%02X expected 0x%02X (err=%d)\n", rev_id, MAX9867_REVID, err);
        return (err != E_NO_ERROR) ? err : E_NOT_SUPPORTED;
    }
    printf("[CODEC] MAX9867 verified REV_ID=0x%02X\n", rev_id);

    WR_REG(MAX9867_17_PWR_SYS, 0x00);

    for (int i = MAX9867_04_INT_EN; i < MAX9867_17_PWR_SYS; i++) {
        WR_REG(i, 0x00);
    }

    int pclk;
    if ((mclk < MHZ(10)) || (mclk > MHZ(60)))
        return E_INVALID;
    else if (mclk < MHZ(20)) {
        psclk = 0x1;
        pclk = mclk;
    } else if (mclk < MHZ(40)) {
        psclk = 0x2;
        pclk = mclk / 2;
    } else {
        psclk = 0x3;
        pclk = mclk / 4;
    }

    WR_REG(MAX9867_05_SYS_CLK, psclk << MAX9867_PSCLK_POS);

    /* Calculate NI clock divider for Normal Mode (Table 5 of MAX9867 datasheet).
     * Bit 7 (MAX9867_PLL) must be set so the PLL synthesizes 44.1kHz, 48kHz, etc. from MCLK */
    uint32_t ni = (uint32_t)(((uint64_t)lrclk * 65536 * 96 + (pclk / 2)) / pclk);
    uint8_t clk_high = MAX9867_PLL | ((ni >> 8) & 0x7F);
    uint8_t clk_low  = (uint8_t)(ni & 0xFF);

    WR_REG(MAX9867_06_CLK_HIGH, clk_high);
    WR_REG(MAX9867_07_CLK_LOW, clk_low);

    WR_REG(MAX9867_09_DAI_CLOCK, 0x01);

    WR_REG(MAX9867_08_DAI_FORMAT, MAX9867_MAS | MAX9867_DLY | MAX9867_HIZOFF);

    /* Full-range Hi-Fi audio: disable voice-band filter (0x00 = bypass) */
    WR_REG(MAX9867_0A_DIG_FILTER, 0x00);

    /* Disable sidetone */
    WR_REG(MAX9867_0B_SIDETONE, 0x00);

    /* Unmute DAC Level register (0x00 = 0dB, unmuted) */
    WR_REG(MAX9867_0C_LVL_DAC, 0x00);

    /* Disable microphone & ADC inputs to prevent digital clock noise bleed */
    WR_REG(MAX9867_15_MIC, 0x00);
    WR_REG(MAX9867_14_ADC_INPUT, 0x00);
    WR_REG(MAX9867_0D_LVL_ADC, 0x00);

    WR_REG(MAX9867_0E_LVL_LINE_IN_LEFT, (0x0C << MAX9867_LI_GAIN_POS) | MAX9867_LI_MUTE);
    WR_REG(MAX9867_0F_LVL_LINE_IN_RIGHT, (0x0C << MAX9867_LI_GAIN_POS) | MAX9867_LI_MUTE);

    /* MAX32655FTHR 3.5mm jack is wired as STEREO CAPACITORLESS (HPMODE=2) with virtual ground.
     * Setting HPMODE=2 eliminates differential phase cancellation and restores full vocal presence! */
    WR_REG(MAX9867_16_MODE, MAX9867_STEREO_NOCAP_CLICKLESS << MAX9867_HPMODE_POS);

    WR_REG(MAX9867_10_VOL_LEFT, 0x04 << MAX9867_VOL_POS);
    WR_REG(MAX9867_11_VOL_RIGHT, 0x04 << MAX9867_VOL_POS);

    /* Power up DAC Left & DAC Right only (ADC disabled for clean, silent playback) */
    WR_REG(MAX9867_17_PWR_SYS, MAX9867_SHDN | MAX9867_DALEN | MAX9867_DAREN);

    return E_NO_ERROR;
}

int max9867_shutdown(void)
{
    int err;

    if ((err = reg_write(MAX9867_08_DAI_FORMAT, 0)) != E_NO_ERROR)
        return err;

    return reg_write(MAX9867_17_PWR_SYS, 0);
}
