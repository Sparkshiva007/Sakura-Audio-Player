/*
 * Sakura Music Player - ST7789 Display Driver & UI Renderer
 * Target: MAX32655FTHR (MSDK Standalone)
 * Display: 1.9" 170x320 SPI TFT (ST7789V - MSP1901)
 * Theme: Premium Dark Obsidian & Luminous Sakura
 */

#ifndef DISPLAY_H_
#define DISPLAY_H_

#include <stdint.h>
#include <stdbool.h>

#define DISPLAY_WIDTH        170
#define DISPLAY_HEIGHT       320
#define DISPLAY_STRIPE_H     16
#define DISPLAY_NUM_STRIPES  (DISPLAY_HEIGHT / DISPLAY_STRIPE_H) /* 20 stripes */

/* 
 * RGB565 Colors (Pre-converted to Big-Endian for direct SPI transmission)
 */
#define COLOR_BLACK          0x0000
#define COLOR_WHITE          0xFFFF
#define COLOR_BG_DARK        0x4208 /* #0C0914 */
#define COLOR_BG_PURPLE      0x8418 /* #1A1126 */
#define COLOR_SAKURA_PINK    0x53FB /* #FF6B9D */
#define COLOR_SAKURA_ROSE    0x70FA /* #FF4D80 */
#define COLOR_SAKURA_SOFT    0xD5FB /* #FFAEC4 */
#define COLOR_CORAL          0x0FFD /* #FFA07A */
#define COLOR_TORII_RED      0x46E1 /* #E62832 */
#define COLOR_FUJI_SNOW      0xBFF7 /* #F0F5FF */
#define COLOR_FUJI_ICE       0xDCDE /* #DCEAF5 */
#define COLOR_FUJI_SHADOW    0x8F21 /* #221B35 */
#define COLOR_LAKE_DEEP      0x8408 /* #0E1428 */
#define COLOR_LAKE_RIPPLE    0xEC41 /* #423855 */
#define COLOR_MINT_GREEN     0x6F3F /* #38EF7D */
#define COLOR_TEXT_SILVER    0x79D6 /* #D8D4E2 */
#define COLOR_TEXT_MUTED     0x8D5A /* #5A526E */
#define COLOR_CARD_BG        0x6410 /* #150F20 */
#define COLOR_CARD_BORDER    0x4939 /* #3E274D */
#define COLOR_CARD_ACTIVE    0xA929 /* #2A1838 */
#define COLOR_TRACK_BG       0xC620 /* #241830 */
#define COLOR_DIVIDER        0x2820 /* #251B33 */

/* 
 * Halloween Special Edition RGB565 Colors (Pre-converted to Big-Endian)
 */
#define COLOR_HW_BG_DARK      0x4208 /* #0C0814 Deep Pitch Obsidian */
#define COLOR_HW_BG_PURPLE    0x2008 /* #0A0410 Gothic Midnight */
#define COLOR_HW_ORANGE       0xA3FB /* #FF7518 Vivid Neon Pumpkin Orange */
#define COLOR_HW_AMBER        0x24FD /* #FFA726 Radiant Jack-o'-Lantern Glow */
#define COLOR_HW_SOFT_ORANGE  0x70FE /* #FFCC80 Candlelight Soft Glow */
#define COLOR_HW_VIOLET       0x3699 /* #9C27B0 Spooky Witch Purple */
#define COLOR_HW_LIME         0xE077 /* #76FF03 Ghost Toxic Lime Green */
#define COLOR_HW_GHOST_WHITE  0xF7DE /* #E0E6F0 Ghost Bone Silver */
#define COLOR_HW_CARD_BG      0x0218 /* #180820 Dark Witch Card BG */
#define COLOR_HW_CARD_BORDER  0x6159 /* #5E2E0A Dark Ember Amber Border */
#define COLOR_HW_TRACK_BG     0x8628 /* #2A1035 Witch Midnight Purple Track */
#define COLOR_HW_DIVIDER      0x0138 /* #381008 Charcoal Ember Divider */

static inline uint16_t display_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	uint16_t val = (uint16_t)((((r >> 3) & 0x1F) << 11) |
	                          (((g >> 2) & 0x3F) << 5) |
	                          ((b >> 3) & 0x1F));
	return (uint16_t)((val >> 8) | ((val & 0xFF) << 8));
}

int display_hw_init(void);
void display_render_splash(int progress_pct, const char *status_msg);
void display_render_player(const char *track_name, int track_idx, int cursor_idx, int total_tracks, int volume, const char *state_str, bool show_vol_overlay);
void display_render_player_ex(const char *track_name, int track_idx, int cursor_idx, int total_tracks, int volume, const char *state_str, bool show_vol_overlay, int start_stripe, int end_stripe, bool show_power_dialog, int power_menu_mode, int power_sel);
void display_sleep(void);
void display_wakeup(void);
void display_screensaver_init(void);
void display_screensaver_step(void);
void display_render_screensaver_full(void);
void display_render_usb_mode(const char *status_msg, const char *sub_msg, int progress_pct, bool show_exit_dialog, int exit_sel);

#endif /* DISPLAY_H_ */
