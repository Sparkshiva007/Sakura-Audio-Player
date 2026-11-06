/*
 * Sakura Music Player - ST7789 Display Driver & UI Renderer
 * Target: MAX32655FTHR (MSDK Standalone)
 * Display: 1.9" 170x320 SPI TFT (ST7789V - MSP1901)
 * Theme: Premium Dark Obsidian & Luminous Sakura
 */

#include "display.h"
#include "font8x12.h"
#include "kanji_sakura.h"
#include "audio_player.h"
#include "player_app.h"
#include "sakura_girl_art.h"
#include "sakura_tree_art.h"
#include "halloween_art.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "mxc_device.h"
#include "mxc_sys.h"
#include "mxc_delay.h"
#include "gpio.h"
#include "spi.h"

/* 
 * 170 x 16 Stripe Buffer (RGB565 = 2 bytes per pixel)
 * Size: 170 * 16 * 2 = 5,440 bytes
 */
static uint16_t stripe_buf[DISPLAY_WIDTH * DISPLAY_STRIPE_H];

/* 
 * Hardware Pin Definitions for ST7789 on MAX32655FTHR
 */
#define LCD_CS_PORT     MXC_GPIO0
#define LCD_CS_PIN      MXC_GPIO_PIN_20   /* P0.20 (CS, Active Low) */

#define LCD_DC_PORT     MXC_GPIO1
#define LCD_DC_PIN      MXC_GPIO_PIN_8    /* P1.8  (Data/Command: 1=Data, 0=Cmd) */

#define LCD_RST_PORT    MXC_GPIO1
#define LCD_RST_PIN     MXC_GPIO_PIN_9    /* P1.9  (Reset, Active Low) */

#define LCD_SPI         MXC_SPI1

/* Floating Sakura petals */
struct petal_t {
    int16_t x;
    int16_t y;
    uint8_t size;
    uint16_t color;
};

static const struct petal_t petals[] = {
    { 24,  80, 4, COLOR_SAKURA_PINK },
    { 48, 120, 3, COLOR_SAKURA_SOFT },
    { 140,  95, 4, COLOR_SAKURA_ROSE },
    { 120, 160, 3, COLOR_SAKURA_PINK },
    { 32, 220, 4, COLOR_SAKURA_SOFT },
    { 135, 235, 3, COLOR_SAKURA_PINK },
    { 85, 260, 3, COLOR_SAKURA_ROSE }
};
#define NUM_PETALS (sizeof(petals) / sizeof(petals[0]))

/* =========================================================================
 * Low-level ST7789 SPI Bus Communication
 * ========================================================================= */

static void display_write_cmd(uint8_t cmd)
{
    MXC_GPIO_OutClr(LCD_DC_PORT, LCD_DC_PIN); /* DC = 0 (Command) */
    MXC_GPIO_OutClr(LCD_CS_PORT, LCD_CS_PIN); /* CS = 0 */

    mxc_spi_req_t req;
    memset(&req, 0, sizeof(req));
    req.spi = LCD_SPI;
    req.ssIdx = 0;
    req.ssDeassert = 0;
    req.txData = &cmd;
    req.rxData = NULL;
    req.txLen = 1;
    req.rxLen = 0;
    MXC_SPI_MasterTransaction(&req);

    MXC_GPIO_OutSet(LCD_CS_PORT, LCD_CS_PIN); /* CS = 1 */
}

static void display_write_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;

    MXC_GPIO_OutSet(LCD_DC_PORT, LCD_DC_PIN); /* DC = 1 (Data) */
    MXC_GPIO_OutClr(LCD_CS_PORT, LCD_CS_PIN); /* CS = 0 */

    while (len > 0) {
        uint32_t chunk = (len > 1024) ? 1024 : (uint32_t)len;
        mxc_spi_req_t req;
        memset(&req, 0, sizeof(req));
        req.spi = LCD_SPI;
        req.ssIdx = 0;
        req.ssDeassert = 0;
        req.txData = (uint8_t *)data;
        req.rxData = NULL;
        req.txLen = chunk;
        req.rxLen = 0;
        MXC_SPI_MasterTransaction(&req);

        data += chunk;
        len -= chunk;
    }

    MXC_GPIO_OutSet(LCD_CS_PORT, LCD_CS_PIN); /* CS = 1 */
}

static void display_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    /* MSP1901 170x320 portrait has x_offset = 35 */
    uint16_t xa0 = x0 + 35;
    uint16_t xa1 = x1 + 35;
    uint8_t caset_data[4] = { (uint8_t)(xa0 >> 8), (uint8_t)(xa0 & 0xFF), (uint8_t)(xa1 >> 8), (uint8_t)(xa1 & 0xFF) };
    uint8_t raset_data[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF), (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };

    display_write_cmd(0x2A); /* CASET */
    display_write_data(caset_data, 4);

    display_write_cmd(0x2B); /* RASET */
    display_write_data(raset_data, 4);

    display_write_cmd(0x2C); /* RAMWR */
}

static void display_flush_stripe(int stripe_idx)
{
    int y0 = stripe_idx * DISPLAY_STRIPE_H;
    int y1 = y0 + DISPLAY_STRIPE_H - 1;

    display_set_window(0, y0, DISPLAY_WIDTH - 1, y1);
    display_write_data((const uint8_t *)stripe_buf, DISPLAY_WIDTH * DISPLAY_STRIPE_H * 2);
    AudioPlayer_Pump();
}

void display_sleep(void)
{
    display_write_cmd(0x28); /* DISPOFF */
    MXC_Delay(MXC_DELAY_MSEC(20));
    display_write_cmd(0x10); /* SLPIN */
    MXC_Delay(MXC_DELAY_MSEC(120));
    MXC_GPIO_OutSet(LCD_CS_PORT, LCD_CS_PIN);
}

void display_wakeup(void)
{
    display_write_cmd(0x11); /* SLPOUT */
    MXC_Delay(MXC_DELAY_MSEC(120));
    display_write_cmd(0x29); /* DISPON */
    MXC_Delay(MXC_DELAY_MSEC(20));
}

int display_hw_init(void)
{
    /* 1. Configure Control Pins (CS: P0.20, DC: P1.8, RST: P1.9) with VDDIOH */
    mxc_gpio_cfg_t cs_pin = { LCD_CS_PORT, LCD_CS_PIN, MXC_GPIO_FUNC_OUT, MXC_GPIO_PAD_NONE, MXC_GPIO_VSSEL_VDDIOH, MXC_GPIO_DRVSTR_0 };
    MXC_GPIO_Config(&cs_pin);
    MXC_GPIO_OutSet(LCD_CS_PORT, LCD_CS_PIN);

    mxc_gpio_cfg_t dc_pin = { LCD_DC_PORT, LCD_DC_PIN, MXC_GPIO_FUNC_OUT, MXC_GPIO_PAD_NONE, MXC_GPIO_VSSEL_VDDIOH, MXC_GPIO_DRVSTR_0 };
    MXC_GPIO_Config(&dc_pin);
    MXC_GPIO_OutClr(LCD_DC_PORT, LCD_DC_PIN);

    mxc_gpio_cfg_t rst_pin = { LCD_RST_PORT, LCD_RST_PIN, MXC_GPIO_FUNC_OUT, MXC_GPIO_PAD_NONE, MXC_GPIO_VSSEL_VDDIOH, MXC_GPIO_DRVSTR_0 };
    MXC_GPIO_Config(&rst_pin);
    MXC_GPIO_OutSet(LCD_RST_PORT, LCD_RST_PIN);

    /* 2. Configure SPI1 Pins on Port 0: SCK (P0.23 ALT1), MOSI (P0.21 ALT1) with VDDIOH */
    mxc_gpio_cfg_t spi1_pins = { LCD_CS_PORT, MXC_GPIO_PIN_21 | MXC_GPIO_PIN_23, MXC_GPIO_FUNC_ALT1, MXC_GPIO_PAD_NONE, MXC_GPIO_VSSEL_VDDIOH, MXC_GPIO_DRVSTR_0 };
    MXC_GPIO_Config(&spi1_pins);

    /* 3. Enable SPI1 Clock & Peripheral at 24MHz (ST7789 rated up to 66MHz) */
    MXC_SYS_ClockEnable(MXC_SYS_PERIPH_CLOCK_SPI1);
    MXC_SYS_Reset_Periph(MXC_SYS_RESET0_SPI1);

    mxc_spi_pins_t spi_pins_struct = {
        .clock = true,
        .ss0 = false,
        .ss1 = false,
        .ss2 = false,
        .miso = false,
        .mosi = true,
        .sdio2 = false,
        .sdio3 = false,
        .vddioh = true
    };
    MXC_SPI_Init(LCD_SPI, 1, 0, 0, 0, 24000000, spi_pins_struct);
    MXC_SPI_SetMode(LCD_SPI, SPI_MODE_3);
    MXC_SPI_SetDataSize(LCD_SPI, 8);
    MXC_SPI_SetWidth(LCD_SPI, SPI_WIDTH_STANDARD);

    /* Re-assert Port 0 SPI1 pins (ALT1, VDDIOH) & enforce VDDIOH (3.3V) on CS, MOSI, SCK */
    MXC_GPIO_Config(&spi1_pins);
    MXC_GPIO_SetVSSEL(LCD_CS_PORT, MXC_GPIO_VSSEL_VDDIOH, LCD_CS_PIN | MXC_GPIO_PIN_21 | MXC_GPIO_PIN_23);
    MXC_GPIO_SetVSSEL(LCD_DC_PORT, MXC_GPIO_VSSEL_VDDIOH, LCD_DC_PIN | LCD_RST_PIN);

    /* 4. Hardware Reset Pulse */
    MXC_GPIO_OutSet(LCD_RST_PORT, LCD_RST_PIN);
    MXC_Delay(MXC_DELAY_MSEC(10));
    MXC_GPIO_OutClr(LCD_RST_PORT, LCD_RST_PIN);
    MXC_Delay(MXC_DELAY_MSEC(50));
    MXC_GPIO_OutSet(LCD_RST_PORT, LCD_RST_PIN);
    MXC_Delay(MXC_DELAY_MSEC(150));

    /* 5. Software Reset & Sleep Out */
    display_write_cmd(0x01); /* SWRESET */
    MXC_Delay(MXC_DELAY_MSEC(120));

    display_write_cmd(0x11); /* SLPOUT */
    MXC_Delay(MXC_DELAY_MSEC(120));

    /* 6. Panel Configuration */
    display_write_cmd(0x3A); /* COLMOD */
    uint8_t colmod = 0x55;   /* 16-bit RGB565 */
    display_write_data(&colmod, 1);

    display_write_cmd(0x36); /* MADCTL */
    uint8_t madctl = 0x00;   /* Portrait mode */
    display_write_data(&madctl, 1);

    display_write_cmd(0xB2); /* PORCTRL */
    uint8_t porctrl[] = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
    display_write_data(porctrl, sizeof(porctrl));

    display_write_cmd(0xB7); /* GCTRL */
    uint8_t gctrl = 0x35;
    display_write_data(&gctrl, 1);

    display_write_cmd(0xBB); /* VCOMS */
    uint8_t vcoms = 0x1A;
    display_write_data(&vcoms, 1);

    display_write_cmd(0xC0); /* LCMCTRL */
    uint8_t lcmctrl = 0x2C;
    display_write_data(&lcmctrl, 1);

    display_write_cmd(0xC2); /* VDVVRHEN */
    uint8_t vdvvrhen = 0x01;
    display_write_data(&vdvvrhen, 1);

    display_write_cmd(0xC3); /* VRHS */
    uint8_t vrhs = 0x0B;
    display_write_data(&vrhs, 1);

    display_write_cmd(0xC4); /* VDVS */
    uint8_t vdvs = 0x20;
    display_write_data(&vdvs, 1);

    display_write_cmd(0xC6); /* FRCTRL2 */
    uint8_t frctrl = 0x0F;
    display_write_data(&frctrl, 1);

    display_write_cmd(0xD0); /* PWCTRL1 */
    uint8_t pwctrl[] = { 0xA4, 0xA1 };
    display_write_data(pwctrl, sizeof(pwctrl));

    display_write_cmd(0xE0); /* PVGAMCTRL */
    uint8_t pvgam[] = { 0x00, 0x03, 0x07, 0x08, 0x07, 0x15, 0x2A, 0x44, 0x42, 0x0A, 0x17, 0x18, 0x25, 0x27 };
    display_write_data(pvgam, sizeof(pvgam));

    display_write_cmd(0xE1); /* NVGAMCTRL */
    uint8_t nvgam[] = { 0x00, 0x03, 0x08, 0x07, 0x07, 0x23, 0x2A, 0x43, 0x42, 0x09, 0x18, 0x17, 0x25, 0x27 };
    display_write_data(nvgam, sizeof(nvgam));

    display_write_cmd(0x21); /* INVON: Inversion on for IPS */
    display_write_cmd(0x29); /* DISPON: Turn on display */
    MXC_Delay(MXC_DELAY_MSEC(20));

    printf("[DISPLAY] ST7789 display initialized successfully.\n");
    return 0;
}

/* =========================================================================
 * Stripe Rasterization Primitives
 * ========================================================================= */

static void stripe_fill_rect(int rx, int ry, int rw, int rh, uint16_t color, int y0)
{
    int y_start = (ry > y0) ? ry : y0;
    int y_end = (ry + rh - 1 < y0 + DISPLAY_STRIPE_H - 1) ? (ry + rh - 1) : (y0 + DISPLAY_STRIPE_H - 1);
    int x_start = (rx > 0) ? rx : 0;
    int x_end = (rx + rw - 1 < DISPLAY_WIDTH - 1) ? (rx + rw - 1) : (DISPLAY_WIDTH - 1);

    if (y_start > y_end || x_start > x_end) {
        return;
    }

    for (int y = y_start; y <= y_end; y++) {
        int row = (y - y0) * DISPLAY_WIDTH;
        for (int x = x_start; x <= x_end; x++) {
            stripe_buf[row + x] = color;
        }
    }
}

static void stripe_draw_rect_outline(int rx, int ry, int rw, int rh, uint16_t color, int y0)
{
    stripe_fill_rect(rx, ry, rw, 1, color, y0);
    stripe_fill_rect(rx, ry + rh - 1, rw, 1, color, y0);
    stripe_fill_rect(rx, ry, 1, rh, color, y0);
    stripe_fill_rect(rx + rw - 1, ry, 1, rh, color, y0);
}

static void stripe_fill_circle(int cx, int cy, int r, uint16_t color, int y0)
{
    int y_min = cy - r;
    int y_max = cy + r;
    int s_top = y0;
    int s_bot = y0 + DISPLAY_STRIPE_H - 1;
    if (y_max < s_top || y_min > s_bot) return;

    int ys = (y_min > s_top) ? y_min : s_top;
    int ye = (y_max < s_bot) ? y_max : s_bot;

    for (int y = ys; y <= ye; y++) {
        int dy = y - cy;
        int dx = 0;
        int r2_dy2 = r * r - dy * dy;
        if (r2_dy2 >= 0) {
            while ((dx + 1) * (dx + 1) <= r2_dy2) dx++;
            stripe_fill_rect(cx - dx, y, 2 * dx + 1, 1, color, y0);
        }
    }
}

static void stripe_draw_char(int cx, int cy, char c, uint16_t color, int y0)
{
    if (c < FONT8X12_FIRST_CHAR || c > FONT8X12_LAST_CHAR) {
        c = ' ';
    }
    int char_idx = c - FONT8X12_FIRST_CHAR;

    for (int r = 0; r < FONT8X12_HEIGHT; r++) {
        int sy = cy + r;
        if (sy < y0 || sy >= y0 + DISPLAY_STRIPE_H) {
            continue;
        }
        uint8_t row_bits = font8x12_data[char_idx][r];
        int stripe_row = (sy - y0) * DISPLAY_WIDTH;

        for (int col = 0; col < FONT8X12_WIDTH; col++) {
            if (row_bits & (0x80 >> col)) {
                int sx = cx + col;
                if (sx >= 0 && sx < DISPLAY_WIDTH) {
                    stripe_buf[stripe_row + sx] = color;
                }
            }
        }
    }
}

static void stripe_draw_string(int x, int y, const char *str, uint16_t color, int y0)
{
    if (!str) return;
    int cur_x = x;
    while (*str) {
        stripe_draw_char(cur_x, y, *str, color, y0);
        cur_x += FONT8X12_WIDTH;
        str++;
    }
}

static void stripe_draw_kanji(int kx, int ky, uint16_t color, int y0)
{
    for (int r = 0; r < KANJI_SAKURA_HEIGHT; r++) {
        int sy = ky + r;
        if (sy < y0 || sy >= y0 + DISPLAY_STRIPE_H) {
            continue;
        }
        int stripe_row = (sy - y0) * DISPLAY_WIDTH;
        int row_byte_offset = r * KANJI_SAKURA_ROW_BYTES;

        for (int c = 0; c < KANJI_SAKURA_WIDTH; c++) {
            int byte_idx = row_byte_offset + (c / 8);
            int bit_idx = 7 - (c % 8);
            if (kanji_sakura_bits[byte_idx] & (1 << bit_idx)) {
                int sx = kx + c;
                if (sx >= 0 && sx < DISPLAY_WIDTH) {
                    stripe_buf[stripe_row + sx] = color;
                }
            }
        }
    }
}

static void stripe_render_bg_gradient(int y0)
{
    for (int line = 0; line < DISPLAY_STRIPE_H; line++) {
        int y = y0 + line;
        uint16_t color;

        if (y < 40) {
            color = COLOR_BG_DARK;
        } else if (y < 120) {
            uint8_t factor = (uint8_t)((y - 40) * 12 / 80);
            color = display_rgb(12 + factor, 9 + (factor / 2), 20 + factor * 2);
        } else if (y < 220) {
            color = COLOR_BG_PURPLE;
        } else {
            uint8_t factor = (uint8_t)((y - 220) * 16 / 100);
            color = display_rgb(26 - (factor / 2), 17 - (factor / 3), 38 - factor);
        }

        int row = line * DISPLAY_WIDTH;
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            stripe_buf[row + x] = color;
        }
    }

    /* Floating petals */
    for (size_t p = 0; p < NUM_PETALS; p++) {
        int px = petals[p].x;
        int py = petals[p].y;
        int sz = petals[p].size;
        uint16_t pcolor = petals[p].color;

        for (int dy = 0; dy < sz; dy++) {
            int sy = py + dy;
            if (sy < y0 || sy >= y0 + DISPLAY_STRIPE_H) continue;
            int row = (sy - y0) * DISPLAY_WIDTH;
            for (int dx = 0; dx < sz; dx++) {
                int sx = px + dx;
                if (sx >= 0 && sx < DISPLAY_WIDTH) {
                    stripe_buf[row + sx] = pcolor;
                }
            }
        }
    }
}

static void stripe_draw_fuji_landscape(int y0)
{
    int fuji_top_y = 265;
    int fuji_base_y = 305;
    int total_h = fuji_base_y - fuji_top_y;

    for (int line = 0; line < DISPLAY_STRIPE_H; line++) {
        int sy = y0 + line;
        if (sy < fuji_top_y || sy > fuji_base_y) continue;

        int dy = sy - fuji_top_y;
        int left_x = 75 - (dy * (75 - 20)) / total_h;
        int right_x = 81 + (dy * (135 - 81)) / total_h;
        int row = line * DISPLAY_WIDTH;

        for (int x = left_x; x <= right_x; x++) {
            if (x < 0 || x >= DISPLAY_WIDTH) continue;
            int snow_cut = 277 + ((x * 5) % 7) - 3;
            if (sy <= snow_cut) {
                stripe_buf[row + x] = (x > 78) ? COLOR_FUJI_ICE : COLOR_FUJI_SNOW;
            } else {
                stripe_buf[row + x] = COLOR_FUJI_SHADOW;
            }
        }
    }

    /* Torii Gate on right bank */
    stripe_fill_rect(130, 278, 24, 2, COLOR_TORII_RED, y0);
    stripe_fill_rect(132, 283, 20, 2, COLOR_TORII_RED, y0);
    stripe_fill_rect(134, 280, 3, 24, COLOR_TORII_RED, y0);
    stripe_fill_rect(147, 280, 3, 24, COLOR_TORII_RED, y0);
}

/* =========================================================================
 * Full Screens: Splash & Audio Player
 * ========================================================================= */

void display_render_splash(int progress_pct, const char *status_msg)
{
    if (progress_pct < 0) progress_pct = 0;
    if (progress_pct > 100) progress_pct = 100;

    for (int s = 0; s < DISPLAY_NUM_STRIPES; s++) {
        int y0 = s * DISPLAY_STRIPE_H;

        stripe_render_bg_gradient(y0);

        /* Title */
        stripe_draw_string(22, 28, "SAKURA AUDIO", COLOR_SAKURA_PINK, y0);
        stripe_draw_string(42, 44, "PLAYER MSDK", COLOR_TEXT_SILVER, y0);

        /* Kanji 桜 (centered at x = 67, y = 80) */
        stripe_draw_kanji(67, 80, COLOR_SAKURA_SOFT, y0);

        /* Status message box */
        stripe_fill_rect(12, 140, 146, 36, COLOR_CARD_BG, y0);
        stripe_draw_rect_outline(12, 140, 146, 36, COLOR_CARD_BORDER, y0);
        stripe_draw_string(18, 146, "INITIALIZING...", COLOR_CORAL, y0);
        if (status_msg) {
            stripe_draw_string(18, 160, status_msg, COLOR_WHITE, y0);
        }

        /* Progress Bar (y = 195, h = 10) */
        stripe_fill_rect(16, 195, 138, 10, COLOR_BG_DARK, y0);
        stripe_draw_rect_outline(16, 195, 138, 10, COLOR_CARD_BORDER, y0);
        int bar_w = (progress_pct * 134) / 100;
        if (bar_w > 0) {
            stripe_fill_rect(18, 197, bar_w, 6, COLOR_SAKURA_PINK, y0);
        }

        /* Landscape at bottom */
        stripe_draw_fuji_landscape(y0);

        /* Footer */
        stripe_draw_string(28, 306, "CIRCUIT DIGEST", COLOR_TEXT_MUTED, y0);

        display_flush_stripe(s);
    }
}

static void truncate_str(char *dst, size_t dst_sz, const char *src, size_t max_chars)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len <= max_chars) {
        snprintf(dst, dst_sz, "%s", src);
    } else {
        snprintf(dst, dst_sz, "%.*s..", (int)(max_chars - 2), src);
    }
}

void display_render_player_ex(const char *track_name, int track_idx, int cursor_idx, int total_tracks, int volume, const char *state_str, bool show_vol_overlay, int start_stripe, int end_stripe, bool show_power_dialog, int power_menu_mode, int power_sel)
{
    char buf[32];
    char short_name[24];
    bool is_sd_mounted = AudioPlayer_IsSDMounted();
    bool is_hw = (PlayerApp_GetTheme() == THEME_HALLOWEEN);

    /* Dynamic Theme Colors: 100% faithful to original Sakura theme when inactive, vivid Halloween when active */
    uint16_t c_primary     = is_hw ? COLOR_HW_ORANGE      : COLOR_SAKURA_PINK;
    uint16_t c_secondary   = is_hw ? COLOR_HW_AMBER       : COLOR_SAKURA_ROSE;
    uint16_t c_soft        = is_hw ? COLOR_HW_SOFT_ORANGE : COLOR_SAKURA_SOFT;
    uint16_t c_alt         = is_hw ? COLOR_HW_VIOLET      : COLOR_CORAL;
    uint16_t c_highlight   = is_hw ? COLOR_HW_LIME        : COLOR_MINT_GREEN;
    uint16_t c_card_bg     = is_hw ? COLOR_HW_CARD_BG     : COLOR_CARD_BG;
    uint16_t c_card_border = is_hw ? COLOR_HW_CARD_BORDER : COLOR_CARD_BORDER;
    uint16_t c_track_bg    = is_hw ? COLOR_HW_TRACK_BG    : COLOR_TRACK_BG;
    uint16_t c_divider     = is_hw ? COLOR_HW_DIVIDER     : COLOR_DIVIDER;
    uint16_t c_text_silver = is_hw ? COLOR_HW_GHOST_WHITE : COLOR_TEXT_SILVER;

    if (start_stripe < 0) start_stripe = 0;
    if (end_stripe >= DISPLAY_NUM_STRIPES) end_stripe = DISPLAY_NUM_STRIPES - 1;
    if (start_stripe > end_stripe) { start_stripe = 0; end_stripe = DISPLAY_NUM_STRIPES - 1; }

    for (int s = start_stripe; s <= end_stripe; s++) {
        int y0 = s * DISPLAY_STRIPE_H;

        stripe_render_bg_gradient(y0);

        /* 1. Header / Status Bar (y = 4..22) */
        stripe_draw_string(10, 6, "10:24", c_text_silver, y0);
        stripe_draw_string(72, 6, is_hw ? "SPOOK" : "BLE", c_soft, y0);

        /* Battery Level Indicator on Right (x = 136..156) */
        stripe_draw_rect_outline(136, 6, 18, 9, c_card_border, y0);
        stripe_fill_rect(154, 8, 2, 5, c_card_border, y0);
        stripe_fill_rect(138, 8, 4, 5, c_highlight, y0);
        stripe_fill_rect(143, 8, 4, 5, c_highlight, y0);
        stripe_fill_rect(148, 8, 4, 5, c_highlight, y0);

        stripe_fill_rect(8, 21, 154, 1, c_divider, y0);

        /* 2. Japanese Kanji & Title Header (y = 25..64) */
        stripe_draw_kanji(12, 26, c_soft, y0);
        stripe_draw_string(52, 26, is_hw ? "HALLOWEEN ED" : "SAKURA AUDIO", c_primary, y0);

        /* State Badge */
        uint16_t badge_color = COLOR_TEXT_MUTED;
        const char *disp_badge = "STANDBY";
        if (!is_sd_mounted) {
            disp_badge = "NO SD CARD";
            badge_color = c_alt;
        } else if (state_str && strstr(state_str, "PLAY")) {
            disp_badge = "PLAYING";
            badge_color = c_highlight;
        } else if (state_str && strstr(state_str, "PAUSE")) {
            disp_badge = "PAUSED";
            badge_color = c_alt;
        } else if (state_str && strstr(state_str, "STOP")) {
            disp_badge = "STOPPED";
            badge_color = c_text_silver;
        }

        stripe_fill_rect(52, 44, 98, 18, c_card_bg, y0);
        stripe_draw_rect_outline(52, 44, 98, 18, badge_color, y0);
        int txt_len = (int)strlen(disp_badge);
        int badge_tx = 52 + (98 - txt_len * 8) / 2;
        if (badge_tx < 54) badge_tx = 54;
        stripe_draw_string(badge_tx, 48, disp_badge, badge_color, y0);

        /* 3. Now Playing / Hero Track Card (y = 66..138, h = 72) */
        stripe_fill_rect(8, 66, 154, 72, c_card_bg, y0);
        stripe_draw_rect_outline(8, 66, 154, 72, c_card_border, y0);

        if (total_tracks > 0) {
            snprintf(buf, sizeof(buf), "NOW PLAYING [%d/%d]", track_idx, total_tracks);
            stripe_draw_string(14, 72, buf, c_secondary, y0);

            truncate_str(short_name, sizeof(short_name), track_name, 16);
            stripe_draw_string(14, 88, short_name, COLOR_WHITE, y0);

            /* Audio Timeline Progress Bar (y = 104, h = 4, w = 142) */
            int pct = AudioPlayer_GetProgressPct();
            stripe_fill_rect(14, 104, 142, 4, c_track_bg, y0);
            stripe_draw_rect_outline(14, 104, 142, 4, c_card_border, y0);
            int fill_w = (pct * 142) / 100;
            if (fill_w > 0) {
                stripe_fill_rect(14, 104, fill_w, 4, c_primary, y0);
                stripe_fill_rect(14 + fill_w - 2, 103, 3, 6, is_hw ? c_secondary : COLOR_WHITE, y0);
            }

            /* Time strings (y = 114): Elapsed (Left), Pct (Center), Total (Right) */
            int el_sec = AudioPlayer_GetElapsedSec();
            int tot_sec = AudioPlayer_GetTotalSec();
            char time_buf[10];
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d", el_sec / 60, el_sec % 60);
            stripe_draw_string(14, 114, time_buf, c_text_silver, y0);

            if (tot_sec > 0) {
                snprintf(time_buf, sizeof(time_buf), "%02d:%02d", tot_sec / 60, tot_sec % 60);
            } else {
                snprintf(time_buf, sizeof(time_buf), "--:--");
            }
            stripe_draw_string(118, 114, time_buf, COLOR_TEXT_MUTED, y0);

            snprintf(time_buf, sizeof(time_buf), "%3d%%", pct);
            stripe_draw_string(68, 114, time_buf, c_soft, y0);
        } else if (!is_sd_mounted) {
            stripe_draw_string(14, 72, "MICROSD STATUS", c_alt, y0);
            stripe_draw_string(14, 90, "Insert MicroSD", COLOR_WHITE, y0);
            stripe_draw_string(14, 110, "FAT32 Card Slot", c_text_silver, y0);
        } else {
            stripe_draw_string(14, 72, "AUDIO STATUS", c_alt, y0);
            stripe_draw_string(14, 90, "No Songs Found", COLOR_WHITE, y0);
            stripe_draw_string(14, 110, "Put MP3 on card", c_text_silver, y0);
        }

        /* 4. Song Selection / Track List Card (y = 142..244, h = 102) */
        stripe_fill_rect(8, 142, 154, 102, c_card_bg, y0);
        stripe_draw_rect_outline(8, 142, 154, 102, c_card_border, y0);

        stripe_draw_string(14, 148, "PLAYLIST", c_soft, y0);
        if (total_tracks > 0) {
            snprintf(buf, sizeof(buf), "[%d/%d]", cursor_idx, total_tracks);
            stripe_draw_string(104, 148, buf, COLOR_TEXT_MUTED, y0);
        }
        stripe_fill_rect(14, 162, 142, 1, c_divider, y0);

        if (total_tracks > 0) {
            int start_idx = 0;
            if (cursor_idx > 3) {
                start_idx = cursor_idx - 3;
                if (start_idx + 4 > total_tracks) {
                    start_idx = total_tracks - 4;
                    if (start_idx < 0) start_idx = 0;
                }
            }

            for (int r = 0; r < 4; r++) {
                int t_i = start_idx + r;
                if (t_i >= total_tracks) break;

                int row_y = 168 + r * 18;
                bool is_cursor  = (t_i == (cursor_idx - 1));
                bool is_playing = (t_i == (track_idx - 1));

                const char *t_name = AudioPlayer_GetTrackName(t_i);
                char item_txt[24];
                truncate_str(short_name, sizeof(short_name), t_name, 11);

                if (is_cursor) {
                    stripe_fill_rect(12, row_y - 2, 146, 16, c_track_bg, y0);
                    stripe_draw_rect_outline(12, row_y - 2, 146, 16, c_primary, y0);

                    if (is_playing) {
                        snprintf(item_txt, sizeof(item_txt), ">* %d.%s", t_i + 1, short_name);
                        stripe_draw_string(15, row_y, item_txt, c_highlight, y0);
                    } else {
                        snprintf(item_txt, sizeof(item_txt), ">  %d.%s", t_i + 1, short_name);
                        stripe_draw_string(15, row_y, item_txt, COLOR_WHITE, y0);
                    }
                } else {
                    if (is_playing) {
                        snprintf(item_txt, sizeof(item_txt), " * %d.%s", t_i + 1, short_name);
                        stripe_draw_string(15, row_y, item_txt, c_highlight, y0);
                    } else {
                        snprintf(item_txt, sizeof(item_txt), "   %d.%s", t_i + 1, short_name);
                        stripe_draw_string(15, row_y, item_txt, c_text_silver, y0);
                    }
                }
            }
        } else if (!is_sd_mounted) {
            stripe_draw_string(20, 174, "Insert MicroSD", COLOR_WHITE, y0);
            stripe_draw_string(20, 192, "Auto-mount ready", c_text_silver, y0);
            stripe_draw_string(20, 210, "Hot-swap enabled", COLOR_TEXT_MUTED, y0);
        } else {
            stripe_draw_string(20, 174, "No Audio Files", COLOR_WHITE, y0);
            stripe_draw_string(20, 192, "Copy .mp3/.wav", c_text_silver, y0);
            stripe_draw_string(20, 210, "to root of card", COLOR_TEXT_MUTED, y0);
        }

        /* 5. Landscape Silhouette at bottom (y = 250..304) */
        stripe_draw_fuji_landscape(y0);

        /* 6. Footer (y = 306) */
        stripe_draw_string(16, 306, is_hw ? "HALLOWEEN SPECIAL ED" : "SAKURA AUDIO PLAYER", c_soft, y0);

        /* 7. Smartphone-Style Volume Overlay HUD on Right Side (y = 60..210) */
        if (show_vol_overlay) {
            int vol_pct = (volume * 100) / 31;
            if (vol_pct < 0) vol_pct = 0;
            if (vol_pct > 100) vol_pct = 100;

            stripe_fill_rect(136, 62, 26, 146, COLOR_BG_DARK, y0);
            stripe_draw_rect_outline(136, 62, 26, 146, c_primary, y0);

            stripe_draw_string(139, 68, "VOL", c_soft, y0);

            stripe_fill_rect(145, 84, 8, 90, c_track_bg, y0);
            stripe_draw_rect_outline(145, 84, 8, 90, c_card_border, y0);

            int fill_h = (vol_pct * 86) / 100;
            if (fill_h > 0) {
                stripe_fill_rect(147, 172 - fill_h, 4, fill_h, c_highlight, y0);
            }

            char v_str[8];
            snprintf(v_str, sizeof(v_str), "%d%%", vol_pct);
            int vx = 138;
            if (vol_pct < 10) vx = 144;
            else if (vol_pct < 100) vx = 140;
            stripe_draw_string(vx, 182, v_str, COLOR_WHITE, y0);
        }

        /* 8. Multi-Level Modal Dialog (y = 84..238, height = 154) */
        if (show_power_dialog) {
            stripe_fill_rect(10, 84, 150, 154, COLOR_BG_DARK, y0);
            stripe_draw_rect_outline(10, 84, 150, 154, c_primary, y0);
            stripe_fill_rect(12, 86, 146, 1, c_secondary, y0);

            if (power_menu_mode == 0) {
                /* --- MAIN SYSTEM MENU (5 items) --- */
                stripe_draw_string(36, 90, "SYSTEM MENU", c_secondary, y0);
                stripe_fill_rect(18, 103, 134, 1, c_divider, y0);

                /* Option 0: POWER OFF */
                if (power_sel == 0) {
                    stripe_fill_rect(16, 106, 138, 17, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 106, 138, 17, c_alt, y0);
                    stripe_draw_string(22, 109, ">  POWER OFF   <", c_alt, y0);
                } else {
                    stripe_fill_rect(16, 106, 138, 17, c_card_bg, y0);
                    stripe_draw_string(22, 109, "   POWER OFF    ", c_text_silver, y0);
                }

                /* Option 1: USB UPLOAD */
                if (power_sel == 1) {
                    stripe_fill_rect(16, 124, 138, 17, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 124, 138, 17, COLOR_WHITE, y0);
                    stripe_draw_string(20, 127, ">  USB UPLOAD  <", COLOR_WHITE, y0);
                } else {
                    stripe_fill_rect(16, 124, 138, 17, c_card_bg, y0);
                    stripe_draw_string(20, 127, "   USB UPLOAD   ", c_text_silver, y0);
                }

                /* Option 2: SLEEP SETTINGS */
                if (power_sel == 2) {
                    stripe_fill_rect(16, 142, 138, 17, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 142, 138, 17, c_primary, y0);
                    stripe_draw_string(20, 145, "> SLEEP SETTINGS<", c_primary, y0);
                } else {
                    stripe_fill_rect(16, 142, 138, 17, c_card_bg, y0);
                    stripe_draw_string(20, 145, "  SLEEP SETTINGS ", c_text_silver, y0);
                }

                /* Option 3: THEME SELECT */
                if (power_sel == 3) {
                    stripe_fill_rect(16, 160, 138, 17, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 160, 138, 17, c_soft, y0);
                    stripe_draw_string(20, 163, "> THEME SELECT  <", c_soft, y0);
                } else {
                    stripe_fill_rect(16, 160, 138, 17, c_card_bg, y0);
                    stripe_draw_string(20, 163, "  THEME SELECT   ", c_text_silver, y0);
                }

                /* Option 4: CANCEL */
                if (power_sel == 4) {
                    stripe_fill_rect(16, 178, 138, 17, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 178, 138, 17, c_highlight, y0);
                    stripe_draw_string(22, 181, ">    CANCEL    <", c_highlight, y0);
                } else {
                    stripe_fill_rect(16, 178, 138, 17, c_card_bg, y0);
                    stripe_draw_string(22, 181, "     CANCEL     ", c_text_silver, y0);
                }

                stripe_draw_string(18, 202, "UP/DN: Move", COLOR_TEXT_MUTED, y0);
                stripe_draw_string(18, 216, "OK: Confirm", c_soft, y0);

            } else if (power_menu_mode == 1) {
                /* --- SLEEP SETTINGS SUBMENU (5 items) --- */
                stripe_draw_string(28, 92, "SLEEP SETTINGS", c_secondary, y0);
                stripe_fill_rect(18, 104, 134, 1, c_divider, y0);

                bool is_sleep_mode = (PlayerApp_GetInactivityMode() == INACTIVITY_MODE_SLEEP);

                /* Option 0: SCREEN SLEEP */
                if (power_sel == 0) {
                    stripe_fill_rect(16, 107, 138, 16, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 107, 138, 16, c_primary, y0);
                    stripe_draw_string(18, 109, is_sleep_mode ? ">[*] SCREEN SLEEP" : ">[ ] SCREEN SLEEP", c_primary, y0);
                } else {
                    stripe_fill_rect(16, 107, 138, 16, c_card_bg, y0);
                    stripe_draw_string(18, 109, is_sleep_mode ? " [*] SCREEN SLEEP" : " [ ] SCREEN SLEEP", is_sleep_mode ? c_highlight : c_text_silver, y0);
                }

                /* Option 1: SCREEN SAVER */
                if (power_sel == 1) {
                    stripe_fill_rect(16, 124, 138, 16, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 124, 138, 16, c_alt, y0);
                    stripe_draw_string(18, 126, !is_sleep_mode ? ">[*] SCREENSAVER " : ">[ ] SCREENSAVER ", c_alt, y0);
                } else {
                    stripe_fill_rect(16, 124, 138, 16, c_card_bg, y0);
                    stripe_draw_string(18, 126, !is_sleep_mode ? " [*] SCREENSAVER " : " [ ] SCREENSAVER ", !is_sleep_mode ? c_highlight : c_text_silver, y0);
                }

                /* Option 2: SLEEP TIMER > */
                if (power_sel == 2) {
                    stripe_fill_rect(16, 141, 138, 16, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 141, 138, 16, c_soft, y0);
                    stripe_draw_string(18, 143, "> SLEEP TIMER  >", c_soft, y0);
                } else {
                    stripe_fill_rect(16, 141, 138, 16, c_card_bg, y0);
                    stripe_draw_string(18, 143, "  SLEEP TIMER  >", c_text_silver, y0);
                }

                /* Option 3: STYLE SELECT > */
                if (power_sel == 3) {
                    stripe_fill_rect(16, 158, 138, 16, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 158, 138, 16, c_secondary, y0);
                    stripe_draw_string(18, 160, "> STYLE SELECT >", c_secondary, y0);
                } else {
                    stripe_fill_rect(16, 158, 138, 16, c_card_bg, y0);
                    stripe_draw_string(18, 160, "  STYLE SELECT >", c_text_silver, y0);
                }

                /* Option 4: BACK */
                if (power_sel == 4) {
                    stripe_fill_rect(16, 175, 138, 16, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 175, 138, 16, c_highlight, y0);
                    stripe_draw_string(22, 177, ">   < BACK     <", c_highlight, y0);
                } else {
                    stripe_fill_rect(16, 175, 138, 16, c_card_bg, y0);
                    stripe_draw_string(22, 177, "    < BACK      ", c_text_silver, y0);
                }

                stripe_draw_string(18, 198, "UP/DN: Move", COLOR_TEXT_MUTED, y0);
                stripe_draw_string(18, 212, "OK: Select Setting", c_soft, y0);

            } else if (power_menu_mode == 2) {
                /* --- SLEEP TIMER SUBMENU (4 items) --- */
                stripe_draw_string(36, 92, "SLEEP TIMER", c_secondary, y0);
                stripe_fill_rect(18, 105, 134, 1, c_divider, y0);

                uint32_t t_ms = PlayerApp_GetInactivityTimeoutMs();

                /* Option 0: 5 SECONDS */
                bool is_5s = (t_ms <= 5000);
                if (power_sel == 0) {
                    stripe_fill_rect(16, 110, 138, 18, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 110, 138, 18, c_primary, y0);
                    stripe_draw_string(18, 114, is_5s ? ">[*]  5 SECONDS " : ">[ ]  5 SECONDS ", c_primary, y0);
                } else {
                    stripe_fill_rect(16, 110, 138, 18, c_card_bg, y0);
                    stripe_draw_string(18, 114, is_5s ? " [*]  5 SECONDS " : " [ ]  5 SECONDS ", is_5s ? c_highlight : c_text_silver, y0);
                }

                /* Option 1: 10 SECONDS */
                bool is_10s = (t_ms > 5000 && t_ms <= 10000);
                if (power_sel == 1) {
                    stripe_fill_rect(16, 130, 138, 18, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 130, 138, 18, c_primary, y0);
                    stripe_draw_string(18, 134, is_10s ? ">[*] 10 SECONDS " : ">[ ] 10 SECONDS ", c_primary, y0);
                } else {
                    stripe_fill_rect(16, 130, 138, 18, c_card_bg, y0);
                    stripe_draw_string(18, 134, is_10s ? " [*] 10 SECONDS " : " [ ] 10 SECONDS ", is_10s ? c_highlight : c_text_silver, y0);
                }

                /* Option 2: 30 SECONDS */
                bool is_30s = (t_ms > 10000);
                if (power_sel == 2) {
                    stripe_fill_rect(16, 150, 138, 18, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 150, 138, 18, c_primary, y0);
                    stripe_draw_string(18, 154, is_30s ? ">[*] 30 SECONDS " : ">[ ] 30 SECONDS ", c_primary, y0);
                } else {
                    stripe_fill_rect(16, 150, 138, 18, c_card_bg, y0);
                    stripe_draw_string(18, 154, is_30s ? " [*] 30 SECONDS " : " [ ] 30 SECONDS ", is_30s ? c_highlight : c_text_silver, y0);
                }

                /* Option 3: BACK */
                if (power_sel == 3) {
                    stripe_fill_rect(16, 170, 138, 18, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 170, 138, 18, c_highlight, y0);
                    stripe_draw_string(22, 174, ">   < BACK     <", c_highlight, y0);
                } else {
                    stripe_fill_rect(16, 170, 138, 18, c_card_bg, y0);
                    stripe_draw_string(22, 174, "    < BACK      ", c_text_silver, y0);
                }

                stripe_draw_string(18, 198, "UP/DN: Move", COLOR_TEXT_MUTED, y0);
                stripe_draw_string(18, 212, "OK: Save Timer", c_soft, y0);

            } else if (power_menu_mode == 3) {
                /* --- SCREENSAVER STYLE SUBMENU (3 items) --- */
                stripe_draw_string(20, 92, "SCREENSAVER STYLE", c_secondary, y0);
                stripe_fill_rect(18, 105, 134, 1, c_divider, y0);

                bool is_tree = (PlayerApp_GetScreensaverStyle() == SCREENSAVER_STYLE_TREE);

                /* Option 0: ANIME GIRL */
                if (power_sel == 0) {
                    stripe_fill_rect(16, 114, 138, 20, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 114, 138, 20, c_primary, y0);
                    stripe_draw_string(18, 118, !is_tree ? ">[*] ANIME GIRL  " : ">[ ] ANIME GIRL  ", c_primary, y0);
                } else {
                    stripe_fill_rect(16, 114, 138, 20, c_card_bg, y0);
                    stripe_draw_string(18, 118, !is_tree ? " [*] ANIME GIRL  " : " [ ] ANIME GIRL  ", !is_tree ? c_highlight : c_text_silver, y0);
                }

                /* Option 1: SAKURA TREE */
                if (power_sel == 1) {
                    stripe_fill_rect(16, 138, 138, 20, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 138, 138, 20, c_secondary, y0);
                    stripe_draw_string(18, 142, is_tree ? ">[*] SAKURA TREE " : ">[ ] SAKURA TREE ", c_secondary, y0);
                } else {
                    stripe_fill_rect(16, 138, 138, 20, c_card_bg, y0);
                    stripe_draw_string(18, 142, is_tree ? " [*] SAKURA TREE " : " [ ] SAKURA TREE ", is_tree ? c_highlight : c_text_silver, y0);
                }

                /* Option 2: BACK */
                if (power_sel == 2) {
                    stripe_fill_rect(16, 162, 138, 20, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 162, 138, 20, c_highlight, y0);
                    stripe_draw_string(22, 166, ">   < BACK     <", c_highlight, y0);
                } else {
                    stripe_fill_rect(16, 162, 138, 20, c_card_bg, y0);
                    stripe_draw_string(22, 166, "    < BACK      ", c_text_silver, y0);
                }

                stripe_draw_string(18, 198, "UP/DN: Move", COLOR_TEXT_MUTED, y0);
                stripe_draw_string(18, 212, "OK: Save Style", c_soft, y0);

            } else {
                /* --- THEME SELECT SUBMENU (3 items) --- */
                stripe_draw_string(32, 92, "THEME SELECT", c_secondary, y0);
                stripe_fill_rect(18, 105, 134, 1, c_divider, y0);

                /* Option 0: SAKURA THEME */
                if (power_sel == 0) {
                    stripe_fill_rect(16, 114, 138, 20, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 114, 138, 20, COLOR_SAKURA_PINK, y0);
                    stripe_draw_string(18, 118, !is_hw ? ">[*] SAKURA THEME" : ">[ ] SAKURA THEME", COLOR_SAKURA_PINK, y0);
                } else {
                    stripe_fill_rect(16, 114, 138, 20, c_card_bg, y0);
                    stripe_draw_string(18, 118, !is_hw ? " [*] SAKURA THEME" : " [ ] SAKURA THEME", !is_hw ? COLOR_MINT_GREEN : c_text_silver, y0);
                }

                /* Option 1: HALLOWEEN EDITION */
                if (power_sel == 1) {
                    stripe_fill_rect(16, 138, 138, 20, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 138, 138, 20, COLOR_HW_ORANGE, y0);
                    stripe_draw_string(18, 142, is_hw ? ">[*] HALLOWEEN ED" : ">[ ] HALLOWEEN ED", COLOR_HW_ORANGE, y0);
                } else {
                    stripe_fill_rect(16, 138, 138, 20, c_card_bg, y0);
                    stripe_draw_string(18, 142, is_hw ? " [*] HALLOWEEN ED" : " [ ] HALLOWEEN ED", is_hw ? COLOR_HW_LIME : c_text_silver, y0);
                }

                /* Option 2: BACK */
                if (power_sel == 2) {
                    stripe_fill_rect(16, 162, 138, 20, c_track_bg, y0);
                    stripe_draw_rect_outline(16, 162, 138, 20, c_highlight, y0);
                    stripe_draw_string(22, 166, ">   < BACK     <", c_highlight, y0);
                } else {
                    stripe_fill_rect(16, 162, 138, 20, c_card_bg, y0);
                    stripe_draw_string(22, 166, "    < BACK      ", c_text_silver, y0);
                }

                stripe_draw_string(18, 198, "UP/DN: Move", COLOR_TEXT_MUTED, y0);
                stripe_draw_string(18, 212, "OK: Apply Theme", c_soft, y0);
            }
        }

        display_flush_stripe(s);
    }
}

void display_render_player(const char *track_name, int track_idx, int cursor_idx, int total_tracks, int volume, const char *state_str, bool show_vol_overlay)
{
    display_render_player_ex(track_name, track_idx, cursor_idx, total_tracks, volume, state_str, show_vol_overlay, 0, DISPLAY_NUM_STRIPES - 1, false, 0, 0);
}

/* =========================================================================
 * Screensaver: Multi-Art Dynamic Engine (Sakura Girl, Sakura Tree, Halloween)
 * (Ultra High Performance Patch Engine - Only ~0.4ms per frame, 0% audio jitter)
 * ========================================================================= */

#define NUM_SCREENSAVER_PETALS 14

typedef struct {
    int16_t base_x;     /* Base X column (0..164) */
    int16_t y;          /* Current Y row (0..314) */
    int16_t curr_x;     /* Current screen X */
    int16_t curr_y;     /* Current screen Y */
    int16_t old_x;      /* Last drawn screen X */
    int16_t old_y;      /* Last drawn screen Y */
    int8_t  speed_y;    /* Falling/Rising speed (1..3 px per tick) */
    int8_t  sway_amp;   /* Horizontal sway amplitude (2..6 px) */
    int8_t  sway_phase; /* Phase offset in sine table */
    uint8_t size;       /* 0 = speck (2x2), 1 = small (3x3), 2 = blossom/ember (5x5) */
    uint16_t color;     /* Particle color */
    bool    drawn;      /* Has been drawn on screen */
} screensaver_petal_t;

static screensaver_petal_t s_petals[NUM_SCREENSAVER_PETALS];
static uint16_t s_screensaver_frame = 0;

static const uint8_t  *s_active_bg_bitmap = sakura_girl_bitmap;
static const uint16_t *s_active_bg_palette = sakura_girl_palette;
static bool s_is_halloween_screensaver = false;

static const int8_t s_sway_sin[32] = {
    0, 2, 4, 6, 7, 8, 8, 8, 7, 6, 4, 2, 0, -2, -4, -6,
    -7, -8, -8, -8, -7, -6, -4, -2, 0, 2, 4, 6, 7, 8, 8, 8
};

void display_render_screensaver_full(void)
{
    /* Render entire 170x320 active pixel art once on transition */
    for (int s = 0; s < DISPLAY_NUM_STRIPES; s++) {
        int y0 = s * DISPLAY_STRIPE_H;
        const uint8_t *src = &s_active_bg_bitmap[y0 * DISPLAY_WIDTH];
        for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_STRIPE_H; i++) {
            stripe_buf[i] = s_active_bg_palette[src[i]];
        }
        display_flush_stripe(s);
    }
}

static inline uint16_t get_petal_pixel(uint8_t size, uint16_t color, int dx, int dy, uint16_t bg)
{
    if (size == 0) {
        /* 2x2 micro speck */
        if (dx >= 2 && dx <= 3 && dy >= 2 && dy <= 3) {
            return (dx == 2 && dy == 2) ? COLOR_WHITE : color;
        }
        return bg;
    } else if (size == 1) {
        /* 3x3 diamond petal/ember */
        int d = (dx > 2 ? dx - 2 : 2 - dx) + (dy > 2 ? dy - 2 : 2 - dy);
        if (d == 0) return COLOR_WHITE;
        if (d == 1) return color;
        return bg;
    } else {
        /* 5x5 cherry blossom petal or magical flame */
        if (dy == 0 && (dx == 1 || dx == 3)) return color;
        if (dy == 1 && (dx >= 0 && dx <= 4)) return (dx == 2) ? COLOR_WHITE : color;
        if (dy == 2 && (dx >= 0 && dx <= 4)) return (dx >= 1 && dx <= 3) ? COLOR_WHITE : color;
        if (dy == 3 && (dx >= 1 && dx <= 3)) return color;
        if (dy == 4 && dx == 2) return color;
        return bg;
    }
}

void display_screensaver_init(void)
{
    s_screensaver_frame = 0;

    /* Select Art & Mode based on active theme and style */
    if (PlayerApp_GetTheme() == THEME_HALLOWEEN) {
        s_active_bg_bitmap = halloween_bitmap;
        s_active_bg_palette = halloween_palette;
        s_is_halloween_screensaver = true;
    } else if (PlayerApp_GetScreensaverStyle() == SCREENSAVER_STYLE_TREE) {
        s_active_bg_bitmap = sakura_tree_bitmap;
        s_active_bg_palette = sakura_tree_palette;
        s_is_halloween_screensaver = false;
    } else {
        s_active_bg_bitmap = sakura_girl_bitmap;
        s_active_bg_palette = sakura_girl_palette;
        s_is_halloween_screensaver = false;
    }

    static const int16_t init_x[NUM_SCREENSAVER_PETALS] = {
        15, 38, 62, 85, 110, 135, 155, 22, 48, 75, 102, 128, 148, 8
    };
    static const int16_t init_y[NUM_SCREENSAVER_PETALS] = {
        8, 45, 90, 135, 180, 225, 270, 25, 68, 115, 160, 205, 250, 290
    };
    static const uint8_t init_size[NUM_SCREENSAVER_PETALS] = {
        2, 1, 0, 2, 1, 0, 2, 1, 2, 0, 1, 2, 0, 1
    };
    static const int8_t init_speed[NUM_SCREENSAVER_PETALS] = {
        2, 1, 3, 2, 1, 2, 1, 3, 2, 1, 2, 1, 3, 2
    };

    static const uint16_t sakura_colors[NUM_SCREENSAVER_PETALS] = {
        COLOR_SAKURA_PINK, COLOR_SAKURA_SOFT, COLOR_WHITE, COLOR_SAKURA_ROSE,
        COLOR_SAKURA_PINK, COLOR_SAKURA_SOFT, COLOR_WHITE, COLOR_SAKURA_ROSE,
        COLOR_SAKURA_PINK, COLOR_SAKURA_SOFT, COLOR_WHITE, COLOR_SAKURA_ROSE,
        COLOR_SAKURA_PINK, COLOR_SAKURA_SOFT
    };

    static const uint16_t halloween_colors[NUM_SCREENSAVER_PETALS] = {
        COLOR_HW_AMBER, COLOR_HW_SOFT_ORANGE, COLOR_HW_LIME, COLOR_HW_ORANGE,
        COLOR_HW_AMBER, COLOR_HW_SOFT_ORANGE, COLOR_HW_LIME, COLOR_HW_ORANGE,
        COLOR_HW_AMBER, COLOR_HW_SOFT_ORANGE, COLOR_HW_LIME, COLOR_HW_ORANGE,
        COLOR_HW_AMBER, COLOR_HW_SOFT_ORANGE
    };

    /* 1. Draw full background art to screen */
    display_render_screensaver_full();

    /* 2. Initialize particles */
    for (int i = 0; i < NUM_SCREENSAVER_PETALS; i++) {
        s_petals[i].base_x = init_x[i];
        s_petals[i].y = init_y[i];
        s_petals[i].curr_x = init_x[i];
        s_petals[i].curr_y = init_y[i];
        s_petals[i].old_x = init_x[i];
        s_petals[i].old_y = init_y[i];
        s_petals[i].size = init_size[i];
        s_petals[i].speed_y = init_speed[i];
        s_petals[i].sway_amp = (i % 3 == 0) ? 5 : ((i % 2 == 0) ? 3 : 4);
        s_petals[i].sway_phase = (int8_t)((i * 7) & 31);
        s_petals[i].color = s_is_halloween_screensaver ? halloween_colors[i] : sakura_colors[i];
        s_petals[i].drawn = false;
    }
}

void display_screensaver_step(void)
{
    uint16_t patch[25];

    /* Phase 1: Erase previously drawn particle patches by restoring background from Flash */
    for (int i = 0; i < NUM_SCREENSAVER_PETALS; i++) {
        if (!s_petals[i].drawn) continue;
        int ox = s_petals[i].old_x;
        int oy = s_petals[i].old_y;
        if (ox < 0 || ox > DISPLAY_WIDTH - 5 || oy < 0 || oy > DISPLAY_HEIGHT - 5) continue;

        for (int dy = 0; dy < 5; dy++) {
            int by = oy + dy;
            for (int dx = 0; dx < 5; dx++) {
                int bx = ox + dx;
                patch[dy * 5 + dx] = s_active_bg_palette[s_active_bg_bitmap[by * DISPLAY_WIDTH + bx]];
            }
        }
        display_set_window((uint16_t)ox, (uint16_t)oy, (uint16_t)(ox + 4), (uint16_t)(oy + 4));
        display_write_data((const uint8_t *)patch, 50);
    }

    /* Phase 2: Advance positions and draw new particles */
    s_screensaver_frame++;

    for (int i = 0; i < NUM_SCREENSAVER_PETALS; i++) {
        if (s_is_halloween_screensaver) {
            /* Halloween embers float gently upward */
            s_petals[i].y -= s_petals[i].speed_y;
            if (s_petals[i].y <= 0) {
                s_petals[i].y = DISPLAY_HEIGHT - 6;
                s_petals[i].base_x = (int16_t)((s_petals[i].base_x + 37 + i * 13) % (DISPLAY_WIDTH - 6));
            }
        } else {
            /* Sakura petals drift gracefully downward */
            s_petals[i].y += s_petals[i].speed_y;
            if (s_petals[i].y >= DISPLAY_HEIGHT - 5) {
                s_petals[i].y = 0;
                s_petals[i].base_x = (int16_t)((s_petals[i].base_x + 47 + i * 11) % (DISPLAY_WIDTH - 6));
            }
        }

        /* Calculate horizontal harmonic sway */
        int sway = (s_sway_sin[(s_petals[i].sway_phase + s_screensaver_frame) & 31] * s_petals[i].sway_amp) / 8;
        int nx = s_petals[i].base_x + sway;
        if (nx < 0) nx = 0;
        if (nx > DISPLAY_WIDTH - 5) nx = DISPLAY_WIDTH - 5;
        int ny = s_petals[i].y;

        s_petals[i].curr_x = (int16_t)nx;
        s_petals[i].curr_y = (int16_t)ny;
        s_petals[i].old_x  = (int16_t)nx;
        s_petals[i].old_y  = (int16_t)ny;
        s_petals[i].drawn  = true;

        /* Build composite patch: background + cute sakura petal or glowing ember */
        for (int dy = 0; dy < 5; dy++) {
            int by = ny + dy;
            for (int dx = 0; dx < 5; dx++) {
                int bx = nx + dx;
                uint16_t bg = s_active_bg_palette[s_active_bg_bitmap[by * DISPLAY_WIDTH + bx]];
                patch[dy * 5 + dx] = get_petal_pixel(s_petals[i].size, s_petals[i].color, dx, dy, bg);
            }
        }
        display_set_window((uint16_t)nx, (uint16_t)ny, (uint16_t)(nx + 4), (uint16_t)(ny + 4));
        display_write_data((const uint8_t *)patch, 50);
    }
}

void display_render_usb_mode(const char *status_msg, const char *sub_msg, int progress_pct, bool show_exit_dialog, int exit_sel)
{
    for (int stripe_idx = 0; stripe_idx < DISPLAY_NUM_STRIPES; stripe_idx++) {
        int y0 = stripe_idx * DISPLAY_STRIPE_H;

        /* Pure black background */
        stripe_fill_rect(0, y0, DISPLAY_WIDTH, DISPLAY_STRIPE_H, COLOR_BLACK, y0);

        /* --- 1. Header (y=10..30) --- */
        stripe_draw_string(21, 12, "USB UPLOAD MODE", COLOR_WHITE, y0);
        stripe_fill_rect(15, 28, 140, 1, COLOR_WHITE, y0);

        /* --- 2. Full-Screen USB Trident ("Soolam") Graphic (y=68..232) --- */
        /* Base circle at (85, 222), radius 9 */
        stripe_fill_circle(85, 222, 9, COLOR_WHITE, y0);

        /* Main vertical trunk: X=83..86 (w=4), Y=92..222 */
        stripe_fill_rect(83, 92, 4, 131, COLOR_WHITE, y0);

        /* Arrow tip at top: tip (85, 68), base Y=92 from X=72 to X=97 */
        for (int y = 68; y <= 92; y++) {
            int dx = (y - 68) / 2;
            stripe_fill_rect(85 - dx, y, 2 * dx + 1, 1, COLOR_WHITE, y0);
        }

        /* Left Branch:
         * Square at (44, 102) to (60, 118)
         * Vertical stem at X=50..53, Y=118..155
         * Angled arm connecting (50, 155) to trunk at (84, 185)
         */
        stripe_fill_rect(44, 102, 17, 17, COLOR_WHITE, y0);
        stripe_fill_rect(50, 118, 4, 38, COLOR_WHITE, y0);
        for (int step = 0; step <= 30; step++) {
            int px = 50 + (step * 34) / 30;
            int py = 155 + step;
            stripe_fill_rect(px, py, 4, 2, COLOR_WHITE, y0);
        }
        stripe_fill_circle(85, 185, 3, COLOR_WHITE, y0);

        /* Right Branch:
         * Circle at (118, 122), radius 8
         * Vertical stem at X=116..119, Y=130..148
         * Angled arm connecting (117, 148) to trunk at (84, 175)
         */
        stripe_fill_circle(118, 122, 8, COLOR_WHITE, y0);
        stripe_fill_rect(116, 130, 4, 19, COLOR_WHITE, y0);
        for (int step = 0; step <= 27; step++) {
            int px = 118 - (step * 33) / 27;
            int py = 148 + step;
            stripe_fill_rect(px - 3, py, 4, 2, COLOR_WHITE, y0);
        }
        stripe_fill_circle(85, 175, 3, COLOR_WHITE, y0);

        /* --- 3. Status Lines (y=246..276) --- */
        if (status_msg && *status_msg) {
            int len1 = strlen(status_msg);
            int sx1 = (DISPLAY_WIDTH - len1 * 8) / 2;
            if (sx1 < 4) sx1 = 4;
            stripe_draw_string(sx1, 248, status_msg, COLOR_WHITE, y0);
        }

        if (sub_msg && *sub_msg) {
            int len2 = strlen(sub_msg);
            int sx2 = (DISPLAY_WIDTH - len2 * 8) / 2;
            if (sx2 < 4) sx2 = 4;
            stripe_draw_string(sx2, 264, sub_msg, COLOR_TEXT_SILVER, y0);
        }

        /* --- 4. Mini Progress Bar (y=282..290) --- */
        if (progress_pct >= 0) {
            stripe_draw_rect_outline(15, 282, 140, 9, COLOR_WHITE, y0);
            int fill_w = (progress_pct * 136) / 100;
            if (fill_w > 136) fill_w = 136;
            if (fill_w > 0) {
                stripe_fill_rect(17, 284, fill_w, 5, COLOR_WHITE, y0);
            }
        }

        /* --- 5. Footer (y=302) --- */
        stripe_draw_string(17, 302, "HOLD CENTER: EXIT", COLOR_TEXT_MUTED, y0);

        /* --- 6. Exit Confirmation Modal Dialog --- */
        if (show_exit_dialog) {
            stripe_fill_rect(12, 104, 146, 112, COLOR_BLACK, y0);
            stripe_draw_rect_outline(12, 104, 146, 112, COLOR_WHITE, y0);
            stripe_draw_rect_outline(14, 106, 142, 108, COLOR_WHITE, y0);

            stripe_draw_string(21, 114, "EXIT USB MODE?", COLOR_WHITE, y0);
            stripe_fill_rect(18, 130, 134, 1, COLOR_WHITE, y0);

            /* Option 0: OK */
            if (exit_sel == 0) {
                stripe_fill_rect(24, 138, 122, 18, COLOR_WHITE, y0);
                stripe_draw_string(32, 142, ">   OK    <", COLOR_BLACK, y0);
            } else {
                stripe_fill_rect(24, 138, 122, 18, COLOR_BLACK, y0);
                stripe_draw_string(32, 142, "    OK     ", COLOR_WHITE, y0);
            }

            /* Option 1: CANCEL */
            if (exit_sel == 1) {
                stripe_fill_rect(24, 162, 122, 18, COLOR_WHITE, y0);
                stripe_draw_string(32, 166, "> CANCEL  <", COLOR_BLACK, y0);
            } else {
                stripe_fill_rect(24, 162, 122, 18, COLOR_BLACK, y0);
                stripe_draw_string(32, 166, "  CANCEL   ", COLOR_WHITE, y0);
            }

            stripe_draw_string(18, 194, "OK: Confirm Exit", COLOR_TEXT_SILVER, y0);
        }

        display_flush_stripe(stripe_idx);
    }
}
