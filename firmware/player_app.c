#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "board.h"
#include "gpio.h"
#include "mxc_device.h"
#include "led.h"
#include "pb.h"
#include "mxc_delay.h"
#include "lp.h"

#include "player_app.h"
#include "audio_player.h"
#include "display.h"
#include "pal_rtc.h"
#include "ff.h"
#include "uart.h"
#include "i2s.h"
#include "sd_spi.h"

/* Extern button flags defined in main.c */
extern volatile bool g_sw2_pending;
extern volatile bool g_sw3_pending;
extern volatile bool g_sw4_pending;

/* 3-Way Navigation Switch Pins:
 * UP     -> P2.2 (Switch UP)
 * SELECT -> P2.1 (Switch CENTER)
 * DOWN   -> P2.0 (Switch DOWN)
 */
static const mxc_gpio_cfg_t nav_switch_pins = {
    MXC_GPIO2,
    MXC_GPIO_PIN_0 | MXC_GPIO_PIN_1 | MXC_GPIO_PIN_2,
    MXC_GPIO_FUNC_IN,
    MXC_GPIO_PAD_PULL_UP,
    MXC_GPIO_VSSEL_VDDIO,
    MXC_GPIO_DRVSTR_0
};

static const mxc_gpio_cfg_t sw4_cfg_pin = {
    MXC_GPIO3,
    MXC_GPIO_PIN_1,
    MXC_GPIO_FUNC_IN,
    MXC_GPIO_PAD_PULL_UP,
    MXC_GPIO_VSSEL_VDDIO,
    MXC_GPIO_DRVSTR_0
};

static int g_current_volume = 25;
static bool g_volume_overlay_active = false;
static uint32_t g_volume_overlay_expiry_ms = 0;
static int g_cursor_track_idx = 0;

static bool g_power_dialog_active = false;
static power_menu_t g_power_menu_state = POWER_MENU_MAIN;
static int  g_power_dialog_sel = 0; /* 0, 1, 2 */
static uint32_t g_power_dialog_expiry_ms = 0;
static bool s_wait_for_menu_release = false;

static bool g_screen_sleeping = false;
static inactivity_mode_t s_inactivity_mode = INACTIVITY_MODE_SCREENSAVER;
static uint32_t s_inactivity_timeout_ms = 10000; /* Default 10 seconds */
static player_theme_t s_theme = THEME_SAKURA;
static screensaver_style_t s_screensaver_style = SCREENSAVER_STYLE_GIRL;
static volatile bool g_screensaver_active = false;
static uint32_t s_last_screensaver_tick_ms = 0;
static uint32_t s_last_user_activity_ms = 0;
static uint32_t s_last_timeline_update_ms = 0;

static volatile uint32_t s_uptime_ms = 0;

void SysTick_Handler(void)
{
    s_uptime_ms++;
}

static uint32_t get_uptime_ms(void)
{
    return s_uptime_ms;
}

uint32_t PlayerApp_GetUptimeMs(void)
{
    return s_uptime_ms;
}

int PlayerApp_GetCursorTrackIndex(void)
{
    return g_cursor_track_idx;
}

void PlayerApp_SetCursorTrackIndex(int idx)
{
    int total = AudioPlayer_GetTotalTracks();
    if (total > 0) {
        if (idx < 0) idx = 0;
        if (idx >= total) idx = total - 1;
        g_cursor_track_idx = idx;
    }
}

bool PlayerApp_IsScreenSleeping(void)
{
    return g_screen_sleeping;
}

void PlayerApp_EnterScreenSleep(void)
{
    if (g_screen_sleeping) return;
    g_screen_sleeping = true;
    printf("[PWR] Entering Screen Sleep (display off, audio continues)...\n");
    display_sleep();
}

void PlayerApp_WakeScreen(void)
{
    if (!g_screen_sleeping) return;
    printf("[PWR] Waking Screen from Sleep...\n");
    display_wakeup();
    g_screen_sleeping = false;
    s_last_user_activity_ms = get_uptime_ms();
    PlayerApp_UpdateStatusLEDs();
    PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
}

inactivity_mode_t PlayerApp_GetInactivityMode(void)
{
    return s_inactivity_mode;
}

void PlayerApp_SetInactivityMode(inactivity_mode_t mode)
{
    s_inactivity_mode = mode;
}

uint32_t PlayerApp_GetInactivityTimeoutMs(void)
{
    return s_inactivity_timeout_ms;
}

void PlayerApp_SetInactivityTimeoutMs(uint32_t ms)
{
    s_inactivity_timeout_ms = ms;
}

player_theme_t PlayerApp_GetTheme(void)
{
    return s_theme;
}

void PlayerApp_SetTheme(player_theme_t theme)
{
    s_theme = theme;
}

screensaver_style_t PlayerApp_GetScreensaverStyle(void)
{
    return s_screensaver_style;
}

void PlayerApp_SetScreensaverStyle(screensaver_style_t style)
{
    s_screensaver_style = style;
}

bool PlayerApp_IsScreensaverActive(void)
{
    return g_screensaver_active;
}

void PlayerApp_EnterScreensaver(void)
{
    if (g_screensaver_active || g_screen_sleeping) return;
    printf("[SCREENSAVER] Entering cute pixel anime screensaver...\n");
    g_screensaver_active = true;
    s_last_screensaver_tick_ms = get_uptime_ms();
    display_screensaver_init();
}

void PlayerApp_ExitScreensaver(void)
{
    if (!g_screensaver_active) return;
    printf("[SCREENSAVER] Exiting screensaver -> restoring player UI\n");
    g_screensaver_active = false;
    s_last_user_activity_ms = get_uptime_ms();
    PlayerApp_UpdateStatusLEDs();
    PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
}

void PlayerApp_UpdateDisplayEx(ui_update_type_t type)
{
    if (g_screen_sleeping) return;

    audio_state_t st = AudioPlayer_GetState();
    int idx = AudioPlayer_GetActiveTrackIndex() + 1;
    int cursor = g_cursor_track_idx + 1;
    int total = AudioPlayer_GetTotalTracks();
    const char *fn = AudioPlayer_GetActiveTrackFilename();

    const char *st_str = "STANDBY";
    if (AudioPlayer_IsChangeRequested() || st == AUDIO_STATE_PLAYING) st_str = "PLAYING";
    else if (st == AUDIO_STATE_PAUSED)  st_str = "PAUSED";
    else if (st == AUDIO_STATE_STOPPED) st_str = "STOPPED";

    int start_stripe = 0;
    int end_stripe = DISPLAY_NUM_STRIPES - 1;

    switch (type) {
    case UI_UPDATE_CURSOR:
        /* Playlist area only (stripes 8..15) */
        start_stripe = 8;
        end_stripe = 15;
        break;
    case UI_UPDATE_VOLUME:
        /* Volume HUD overlay only (stripes 3..13) */
        start_stripe = 3;
        end_stripe = 13;
        break;
    case UI_UPDATE_POWER_DIALOG:
        /* Power / Sleep modal dialog only (stripes 5..14: y = 80..239) */
        start_stripe = 5;
        end_stripe = 14;
        break;
    case UI_UPDATE_TIMELINE:
        /* Audio Timeline Card only (stripes 4..8: y = 64..143) */
        start_stripe = 4;
        end_stripe = 8;
        break;
    case UI_UPDATE_ALL:
    default:
        start_stripe = 0;
        end_stripe = DISPLAY_NUM_STRIPES - 1;
        break;
    }

    display_render_player_ex(fn, idx, cursor, total, g_current_volume, st_str,
                             g_volume_overlay_active, start_stripe, end_stripe,
                             g_power_dialog_active, (int)g_power_menu_state, g_power_dialog_sel);
}

void PlayerApp_UpdateDisplay(void)
{
    PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
}

void PlayerApp_VolumeUp(void)
{
    if (g_current_volume < 31) {
        g_current_volume++;
        AudioPlayer_SetVolume(g_current_volume);
    }
    g_volume_overlay_active = true;
    g_volume_overlay_expiry_ms = get_uptime_ms() + 1500;
    PlayerApp_UpdateDisplayEx(UI_UPDATE_VOLUME);
}

void PlayerApp_VolumeDown(void)
{
    if (g_current_volume > 0) {
        g_current_volume--;
        AudioPlayer_SetVolume(g_current_volume);
    }
    g_volume_overlay_active = true;
    g_volume_overlay_expiry_ms = get_uptime_ms() + 1500;
    PlayerApp_UpdateDisplayEx(UI_UPDATE_VOLUME);
}

int PlayerApp_GetVolume(void)
{
    return g_current_volume;
}

/* MAX32655FTHR Board Discrete LEDs Mapping:
 * D1 -> P0.18 (RED LED)   - Index 0
 * D2 -> P0.19 (GREEN LED) - Index 1
 * D3 -> P0.26 (BLUE LED)  - Index 2
 */
static const mxc_gpio_cfg_t board_led_pins[] = {
    { MXC_GPIO0, MXC_GPIO_PIN_18, MXC_GPIO_FUNC_OUT, MXC_GPIO_PAD_NONE, MXC_GPIO_VSSEL_VDDIO, MXC_GPIO_DRVSTR_3 }, /* RED   */
    { MXC_GPIO0, MXC_GPIO_PIN_19, MXC_GPIO_FUNC_OUT, MXC_GPIO_PAD_NONE, MXC_GPIO_VSSEL_VDDIO, MXC_GPIO_DRVSTR_3 }, /* GREEN */
    { MXC_GPIO0, MXC_GPIO_PIN_26, MXC_GPIO_FUNC_OUT, MXC_GPIO_PAD_NONE, MXC_GPIO_VSSEL_VDDIO, MXC_GPIO_DRVSTR_3 }  /* BLUE  */
};

#ifdef LED_RED
#undef LED_RED
#endif
#ifdef LED_GREEN
#undef LED_GREEN
#endif
#ifdef LED_BLUE
#undef LED_BLUE
#endif

#define LED_RED   0
#define LED_GREEN 1
#define LED_BLUE  2

/* Override Cordio BLE stack PAL LED functions so BLE events don't overwrite board LEDs */
void PalLedInit(void) {}
void PalLedDeInit(void) {}
void PalLedOn(uint8_t ledId) { (void)ledId; }
void PalLedOff(uint8_t ledId) { (void)ledId; }

static playerSender_t g_sender = NULL;

void PlayerApp_SetSender(playerSender_t sender)
{
    g_sender = sender;
}

void PlayerApp_SendStatus(const char *event)
{
    if (g_sender == NULL) return;

    char msg[128];
    audio_state_t st = AudioPlayer_GetState();
    int idx = AudioPlayer_GetActiveTrackIndex() + 1;
    const char *fn = AudioPlayer_GetActiveTrackFilename();

    const char *st_str = "STOPPED";
    if (AudioPlayer_IsChangeRequested() || st == AUDIO_STATE_PLAYING) st_str = "PLAYING";
    else if (st == AUDIO_STATE_PAUSED) st_str = "PAUSED";

    /* Transmit explicit track name for mobile display */
    snprintf(msg, sizeof(msg), "TRACK_NAME:%s", fn);
    g_sender(msg);

    if (event != NULL && strlen(event) > 0) {
        snprintf(msg, sizeof(msg), "STATUS:%s:TRK:%d:%s:EVT:%s", st_str, idx, fn, event);
    } else {
        snprintf(msg, sizeof(msg), "STATUS:%s:TRK:%d:%s", st_str, idx, fn);
    }
    g_sender(msg);
}

void PlayerApp_UpdateStatusLEDs(void)
{
    audio_state_t st = AudioPlayer_GetState();
    bool is_playing = (st == AUDIO_STATE_PLAYING) || AudioPlayer_IsChangeRequested();

    /* Active-Low board LEDs: LOW (0) = ON, HIGH (1) = OFF */
    if (is_playing) {
        MXC_GPIO_OutSet(board_led_pins[LED_RED].port, board_led_pins[LED_RED].mask);
        MXC_GPIO_OutClr(board_led_pins[LED_GREEN].port, board_led_pins[LED_GREEN].mask); /* GREEN ON */
        MXC_GPIO_OutSet(board_led_pins[LED_BLUE].port, board_led_pins[LED_BLUE].mask);
    } else if (st == AUDIO_STATE_PAUSED) {
        MXC_GPIO_OutSet(board_led_pins[LED_RED].port, board_led_pins[LED_RED].mask);
        MXC_GPIO_OutSet(board_led_pins[LED_GREEN].port, board_led_pins[LED_GREEN].mask);
        MXC_GPIO_OutClr(board_led_pins[LED_BLUE].port, board_led_pins[LED_BLUE].mask);   /* BLUE ON */
    } else {
        MXC_GPIO_OutClr(board_led_pins[LED_RED].port, board_led_pins[LED_RED].mask);     /* RED ON */
        MXC_GPIO_OutSet(board_led_pins[LED_GREEN].port, board_led_pins[LED_GREEN].mask);
        MXC_GPIO_OutSet(board_led_pins[LED_BLUE].port, board_led_pins[LED_BLUE].mask);
    }
}

void PlayerApp_Init(void)
{
    for (int i = 0; i < 3; i++) {
        MXC_GPIO_Config(&board_led_pins[i]);
    }
    SysTick_Config(SystemCoreClock / 1000);
    PB_Init();
    MXC_GPIO_Config(&nav_switch_pins);
    MXC_GPIO_Config(&sw4_cfg_pin);

    s_last_user_activity_ms = get_uptime_ms();

    PlayerApp_UpdateStatusLEDs();
    PlayerApp_SendStatus("INIT");
    PlayerApp_UpdateDisplay();
}

/* SW2 pressed -> Backward Track (Previous Song) */
void PlayerApp_SW2_Pressed(void)
{
    printf("[BTN SW2] Backward Track pressed!\n");
    if (g_sender) {
        g_sender("BACKWARD");
    }
    AudioPlayer_PrevTrack();
    PlayerApp_UpdateStatusLEDs();
    PlayerApp_SendStatus("BTN_SW2_BACKWARD");
    PlayerApp_UpdateDisplay();
}

/* SW3 pressed -> Forward Track (Next Song) */
void PlayerApp_SW3_Pressed(void)
{
    printf("[BTN SW3] Forward Track pressed!\n");
    if (g_sender) {
        g_sender("FORWARD");
    }
    AudioPlayer_NextTrack();
    PlayerApp_UpdateStatusLEDs();
    PlayerApp_SendStatus("BTN_SW3_FORWARD");
    PlayerApp_UpdateDisplay();
}

/* Cursor movement UP (visually moves up the playlist) */
void PlayerApp_CursorUp(void)
{
    int total = AudioPlayer_GetTotalTracks();
    if (total <= 0) return;

    g_cursor_track_idx = (g_cursor_track_idx - 1 + total) % total;
    printf("[NAV] Cursor UP -> Track %d: %s\n", g_cursor_track_idx + 1, AudioPlayer_GetTrackName(g_cursor_track_idx));
    PlayerApp_UpdateDisplayEx(UI_UPDATE_CURSOR);
}

/* Cursor movement DOWN (visually moves down the playlist) */
void PlayerApp_CursorDown(void)
{
    int total = AudioPlayer_GetTotalTracks();
    if (total <= 0) return;

    g_cursor_track_idx = (g_cursor_track_idx + 1) % total;
    printf("[NAV] Cursor DOWN -> Track %d: %s\n", g_cursor_track_idx + 1, AudioPlayer_GetTrackName(g_cursor_track_idx));
    PlayerApp_UpdateDisplayEx(UI_UPDATE_CURSOR);
}

/* SELECT: Play selected track if different, or toggle Play/Pause if on playing track */
void PlayerApp_SelectPressed(void)
{
    int total = AudioPlayer_GetTotalTracks();
    if (total <= 0) return;

    int playing_idx = AudioPlayer_GetCurrentTrackIndex();
    audio_state_t st = AudioPlayer_GetState();

    printf("[NAV] SELECT: cursor=%d, playing=%d, state=%d\n", g_cursor_track_idx, playing_idx, st);

    if (g_cursor_track_idx != playing_idx || st == AUDIO_STATE_STOPPED) {
        printf("[NAV] Playing selected track [%d/%d]: %s\n",
               g_cursor_track_idx + 1, total, AudioPlayer_GetTrackName(g_cursor_track_idx));
        AudioPlayer_PlayTrack(g_cursor_track_idx);
        PlayerApp_UpdateStatusLEDs();
        PlayerApp_SendStatus("TRACK_SELECT");
        PlayerApp_UpdateDisplay();
    } else {
        printf("[NAV] Cursor on playing track -> Toggle Play/Pause\n");
        st = AudioPlayer_PlayPauseToggle();
        if (g_sender) {
            if (st == AUDIO_STATE_PAUSED) {
                g_sender("PAUSE");
            } else if (st == AUDIO_STATE_PLAYING) {
                g_sender("PLAY");
            }
        }
        PlayerApp_UpdateStatusLEDs();
        PlayerApp_SendStatus("PLAY_PAUSE");
        PlayerApp_UpdateDisplay();
    }
}

/* SW4 pressed -> Select / Play-Pause */
void PlayerApp_SW4_Pressed(void)
{
    printf("[BTN SW4] Select / Play-Pause pressed!\n");
    PlayerApp_SelectPressed();
}

void PlayerApp_PowerOff(void)
{
    printf("[PWR] Shutting down device into ultra-low-power Standby mode...\n");

    /* 1. Stop playback */
    AudioPlayer_Stop();
    MXC_Delay(MXC_DELAY_MSEC(50));

    /* 2. Turn off all board LEDs (Active-Low: High = OFF) */
    MXC_GPIO_OutSet(board_led_pins[LED_RED].port, board_led_pins[LED_RED].mask);
    MXC_GPIO_OutSet(board_led_pins[LED_GREEN].port, board_led_pins[LED_GREEN].mask);
    MXC_GPIO_OutSet(board_led_pins[LED_BLUE].port, board_led_pins[LED_BLUE].mask);

    /* 3. Deep sleep ST7789 display (DISPOFF 0x28, SLPIN 0x10) */
    display_sleep();

    /* 4. Power down audio peripherals (I2S, MAX9867, PMIC BUCK2, 12.288MHz osc, SD card) */
    AudioPlayer_PowerDown();

    /* 5. Wait for all buttons to be completely released before arming wakeup */
    uint32_t release_wait = 0;
    while (release_wait < 250) {
        uint32_t p2 = MXC_GPIO_InGet(MXC_GPIO2, MXC_GPIO_PIN_0 | MXC_GPIO_PIN_1 | MXC_GPIO_PIN_2);
        uint32_t p3 = MXC_GPIO_InGet(MXC_GPIO3, MXC_GPIO_PIN_1);
        bool p2_released = ((p2 & (MXC_GPIO_PIN_0 | MXC_GPIO_PIN_1 | MXC_GPIO_PIN_2)) == (MXC_GPIO_PIN_0 | MXC_GPIO_PIN_1 | MXC_GPIO_PIN_2));
        bool p3_released = ((p3 & MXC_GPIO_PIN_1) != 0);
        if (p2_released && p3_released && PB_Get(0) == 0 && PB_Get(1) == 0) {
            break;
        }
        MXC_Delay(MXC_DELAY_MSEC(20));
        release_wait++;
    }
    MXC_Delay(MXC_DELAY_MSEC(100));

    /* 6. Configure Center buttons & Navigation buttons as wakeup sources */
    mxc_gpio_cfg_t wu_p2_1 = { MXC_GPIO2, MXC_GPIO_PIN_1, MXC_GPIO_FUNC_IN, MXC_GPIO_PAD_PULL_UP, MXC_GPIO_VSSEL_VDDIO, MXC_GPIO_DRVSTR_0 }; /* Case Center */
    mxc_gpio_cfg_t wu_p3_1 = { MXC_GPIO3, MXC_GPIO_PIN_1, MXC_GPIO_FUNC_IN, MXC_GPIO_PAD_PULL_UP, MXC_GPIO_VSSEL_VDDIO, MXC_GPIO_DRVSTR_0 }; /* SW4 */
    mxc_gpio_cfg_t wu_sw2  = { MXC_GPIO0, MXC_GPIO_PIN_2, MXC_GPIO_FUNC_IN, MXC_GPIO_PAD_PULL_UP, MXC_GPIO_VSSEL_VDDIO, MXC_GPIO_DRVSTR_0 }; /* SW2 */
    mxc_gpio_cfg_t wu_sw3  = { MXC_GPIO0, MXC_GPIO_PIN_3, MXC_GPIO_FUNC_IN, MXC_GPIO_PAD_PULL_UP, MXC_GPIO_VSSEL_VDDIO, MXC_GPIO_DRVSTR_0 }; /* SW3 */

    MXC_GPIO_Config(&wu_p2_1);
    MXC_GPIO_Config(&wu_p3_1);
    MXC_GPIO_Config(&wu_sw2);
    MXC_GPIO_Config(&wu_sw3);

    MXC_GPIO_IntConfig(&wu_p2_1, MXC_GPIO_INT_FALLING);
    MXC_GPIO_EnableInt(wu_p2_1.port, wu_p2_1.mask);
    NVIC_EnableIRQ((IRQn_Type)MXC_GPIO_GET_IRQ(MXC_GPIO_GET_IDX(wu_p2_1.port)));

    MXC_GPIO_IntConfig(&wu_p3_1, MXC_GPIO_INT_FALLING);
    MXC_GPIO_EnableInt(wu_p3_1.port, wu_p3_1.mask);
    NVIC_EnableIRQ((IRQn_Type)MXC_GPIO_GET_IRQ(MXC_GPIO_GET_IDX(wu_p3_1.port)));

    MXC_GPIO_IntConfig(&wu_sw2, MXC_GPIO_INT_FALLING);
    MXC_GPIO_EnableInt(wu_sw2.port, wu_sw2.mask);
    NVIC_EnableIRQ((IRQn_Type)MXC_GPIO_GET_IRQ(MXC_GPIO_GET_IDX(wu_sw2.port)));

    MXC_GPIO_IntConfig(&wu_sw3, MXC_GPIO_INT_FALLING);
    MXC_GPIO_EnableInt(wu_sw3.port, wu_sw3.mask);
    NVIC_EnableIRQ((IRQn_Type)MXC_GPIO_GET_IRQ(MXC_GPIO_GET_IDX(wu_sw3.port)));

    MXC_LP_EnableGPIOWakeup(&wu_p2_1);
    MXC_GPIO_SetWakeEn(wu_p2_1.port, wu_p2_1.mask);

    MXC_LP_EnableGPIOWakeup(&wu_p3_1);
    MXC_GPIO_SetWakeEn(wu_p3_1.port, wu_p3_1.mask);

    MXC_LP_EnableGPIOWakeup(&wu_sw2);
    MXC_GPIO_SetWakeEn(wu_sw2.port, wu_sw2.mask);

    MXC_LP_EnableGPIOWakeup(&wu_sw3);
    MXC_GPIO_SetWakeEn(wu_sw3.port, wu_sw3.mask);

    MXC_LP_ClearWakeStatus();

    /* 7. Enter Standby Mode (< 1.5uA battery draw) with IRQs disabled for direct WFI wake */
    printf("[PWR] Entering Standby Mode now. Press Center/SW4 to power on.\n");
    MXC_Delay(MXC_DELAY_MSEC(50));

    /* Clear low-power mode bits in GCR before setting Standby */
    MXC_GCR->pm &= ~MXC_F_GCR_PM_MODE;
    __disable_irq();
    MXC_LP_EnterStandbyMode();

    /* 8. Woken up by button press! Reboot system through full Sakura splash */
    NVIC_SystemReset();

    while (1) {}
}

typedef struct {
    bool is_down;
    uint32_t press_start_ms;
    bool long_pressed;
    uint32_t last_repeat_ms;
} nav_btn_t;

static nav_btn_t s_nav_up = { 0 };
static nav_btn_t s_nav_down = { 0 };
static nav_btn_t s_nav_sel = { 0 };
static bool s_chord_pressed = false;
static uint32_t s_select_cooldown_ms = 0; /* Cooldown after SELECT action fires */

void PlayerApp_ProcessEvents(void)
{
    uint32_t now_ms = get_uptime_ms();

    /* Check for incoming console / PC serial requests first */
    static char s_serial_cmd_buf[64];
    static int s_serial_cmd_len = 0;
    static uint32_t s_last_serial_rx_ms = 0;

    /* Reset buffer if incomplete command sits idle for > 500ms */
    if (s_serial_cmd_len > 0 && (now_ms - s_last_serial_rx_ms > 500)) {
        s_serial_cmd_len = 0;
    }

    while (MXC_UART_GetRXFIFOAvailable(MXC_UART0) > 0) {
        int c = MXC_UART_ReadCharacterRaw(MXC_UART0);
        if (c < 0) continue;
        s_last_serial_rx_ms = now_ms;
        if (c == '\r') continue;
        if (c == '\n') {
            s_serial_cmd_buf[s_serial_cmd_len] = '\0';
            if (s_serial_cmd_len > 0) {
                if (g_screensaver_active) PlayerApp_ExitScreensaver();
                if (g_screen_sleeping)    PlayerApp_WakeScreen();
                s_last_user_activity_ms = now_ms;
                if (strcasecmp(s_serial_cmd_buf, "PING") == 0 || strcasecmp(s_serial_cmd_buf, "USB_UPLOAD") == 0) {
                    s_serial_cmd_len = 0;
                    PlayerApp_EnterUSBUpload();
                    return;
                } else {
                    PlayerApp_HandleCommand(s_serial_cmd_buf);
                }
            }
            s_serial_cmd_len = 0;
            break;
        } else {
            if (s_serial_cmd_len < (int)sizeof(s_serial_cmd_buf) - 1) {
                s_serial_cmd_buf[s_serial_cmd_len++] = (char)c;
            } else {
                s_serial_cmd_len = 0;
            }
        }
    }

    /* Direct physical button level readings:
     * PB_Get(0): SW2 (P0.2) active-low -> 1 when pressed
     * PB_Get(1): SW3 (P0.3) active-low -> 1 when pressed
     */
    bool sw2_down = (PB_Get(0) != 0) || g_sw2_pending;
    g_sw2_pending = false;
    bool sw3_down = (PB_Get(1) != 0) || g_sw3_pending;
    g_sw3_pending = false;

    /* Port 2 External Switch:
     * P2.0: Case DOWN (active low, pull-up)
     * P2.1: Case SELECT (active low, pull-up)
     * P2.2: Case UP (active low, pull-up)
     */
    uint32_t p2_in = MXC_GPIO_InGet(MXC_GPIO2, MXC_GPIO_PIN_0 | MXC_GPIO_PIN_1 | MXC_GPIO_PIN_2);
    bool case_down = ((p2_in & MXC_GPIO_PIN_0) == 0);
    bool case_sel  = ((p2_in & MXC_GPIO_PIN_1) == 0);
    bool case_up   = ((p2_in & MXC_GPIO_PIN_2) == 0);

    /* SW4 / PB2 / P3.1 Wakeup Pin (active low with pull-up) */
    uint32_t p3_in = MXC_GPIO_InGet(MXC_GPIO3, MXC_GPIO_PIN_1);
    bool sw4_down  = ((p3_in & MXC_GPIO_PIN_1) == 0) || g_sw4_pending;
    g_sw4_pending  = false;

    /* 0. Check Power Dialog auto-timeout (12s) */
    if (g_power_dialog_active && now_ms >= g_power_dialog_expiry_ms) {
        g_power_dialog_active = false;
        PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
    }

    /* Check Volume Overlay HUD auto-fade / expiry (1.5s) */
    if (g_volume_overlay_active && now_ms >= g_volume_overlay_expiry_ms) {
        g_volume_overlay_active = false;
        PlayerApp_UpdateDisplayEx(UI_UPDATE_VOLUME);
    }

    bool up_active   = sw3_down || case_up;
    bool down_active = sw2_down || case_down;
    bool sel_active  = case_sel || sw4_down;

    /* 0. Handle Screen Sleep Mode */
    if (g_screen_sleeping) {
        if (up_active || down_active || sel_active) {
            PlayerApp_WakeScreen();
            s_wait_for_menu_release = true; /* Consume this press so it only wakes screen */
        }
        return;
    }

    /* 0b. Handle Screensaver Mode: any button press exits screensaver immediately */
    if (g_screensaver_active) {
        if (up_active || down_active || sel_active) {
            PlayerApp_ExitScreensaver();
            s_wait_for_menu_release = true; /* Consume this press so it only wakes screen */
        } else {
            /* Advance screensaver animation step */
            uint32_t now_tick = get_uptime_ms();
            if (now_tick - s_last_screensaver_tick_ms >= 45) {
                s_last_screensaver_tick_ms = now_tick;
                display_screensaver_step();
            }
        }
        return;
    }

    /* Wait for button release before accepting new inputs */
    if (s_wait_for_menu_release) {
        if (!sel_active && !up_active && !down_active) {
            s_wait_for_menu_release = false;
            s_nav_sel.is_down = false;
            s_nav_up.is_down = false;
            s_nav_down.is_down = false;
        }
        return;
    }

    /* Track user activity timestamp */
    if (up_active || down_active || sel_active) {
        s_last_user_activity_ms = now_ms;
    }

    /* Auto Inactivity Action (Configurable Timeout: 5s, 10s, 30s) */
    if (!g_power_dialog_active && !g_screen_sleeping && !g_screensaver_active &&
        s_last_user_activity_ms != 0 && (now_ms - s_last_user_activity_ms >= s_inactivity_timeout_ms)) {
        if (s_inactivity_mode == INACTIVITY_MODE_SCREENSAVER) {
            printf("[PWR] Inactivity timeout (%lu ms) -> Starting SCREENSAVER\n", (unsigned long)s_inactivity_timeout_ms);
            PlayerApp_EnterScreensaver();
        } else {
            printf("[PWR] Inactivity timeout (%lu ms) -> Auto SCREEN SLEEP\n", (unsigned long)s_inactivity_timeout_ms);
            PlayerApp_EnterScreenSleep();
        }
        return;
    }

    /* Periodic Audio Timeline update (every 1s during playback) */
    audio_state_t cur_st = AudioPlayer_GetState();
    if (!g_power_dialog_active && (cur_st == AUDIO_STATE_PLAYING || AudioPlayer_IsChangeRequested())) {
        if (now_ms - s_last_timeline_update_ms >= 1000) {
            s_last_timeline_update_ms = now_ms;
            PlayerApp_UpdateDisplayEx(UI_UPDATE_TIMELINE);
        }
    }

    /* 0. Check Power Dialog auto-timeout (12s) */
    if (g_power_dialog_active && now_ms >= g_power_dialog_expiry_ms) {
        g_power_dialog_active = false;
        PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
    }

    /* Check Volume Overlay HUD auto-fade / expiry (1.5s) */
    if (g_volume_overlay_active && now_ms >= g_volume_overlay_expiry_ms) {
        g_volume_overlay_active = false;
        PlayerApp_UpdateDisplayEx(UI_UPDATE_VOLUME);
    }

    /* --- Handle Multi-Level Power, Sleep, Style & Theme Dialog Mode --- */
    if (g_power_dialog_active) {
        int num_options = 5;
        switch (g_power_menu_state) {
        case POWER_MENU_MAIN:          num_options = 5; break;
        case POWER_MENU_SLEEP_SUBMENU: num_options = 5; break;
        case POWER_MENU_TIMER_SUBMENU: num_options = 4; break;
        case POWER_MENU_STYLE_SUBMENU: num_options = 3; break;
        case POWER_MENU_THEME_SUBMENU: num_options = 3; break;
        default: num_options = 3; break;
        }

        /* UP button -> Select Previous Option */
        if (up_active) {
            if (!s_nav_up.is_down) {
                s_nav_up.is_down = true;
                g_power_dialog_expiry_ms = now_ms + 12000;
                g_power_dialog_sel = (g_power_dialog_sel - 1 + num_options) % num_options;
                PlayerApp_UpdateDisplayEx(UI_UPDATE_POWER_DIALOG);
            }
        } else {
            s_nav_up.is_down = false;
        }

        /* DOWN button -> Select Next Option */
        if (down_active) {
            if (!s_nav_down.is_down) {
                s_nav_down.is_down = true;
                g_power_dialog_expiry_ms = now_ms + 12000;
                g_power_dialog_sel = (g_power_dialog_sel + 1) % num_options;
                PlayerApp_UpdateDisplayEx(UI_UPDATE_POWER_DIALOG);
            }
        } else {
            s_nav_down.is_down = false;
        }

        /* SELECT button -> Confirm selected option on release */
        if (sel_active) {
            if (!s_nav_sel.is_down) {
                s_nav_sel.is_down = true;
                s_nav_sel.press_start_ms = now_ms;
            }
        } else {
            if (s_nav_sel.is_down) {
                s_nav_sel.is_down = false;
                if (now_ms - s_nav_sel.press_start_ms >= 30) {
                    if (g_power_menu_state == POWER_MENU_MAIN) {
                        if (g_power_dialog_sel == 0) {
                            /* POWER OFF */
                            PlayerApp_PowerOff();
                        } else if (g_power_dialog_sel == 1) {
                            /* USB UPLOAD */
                            printf("[PWR] Menu -> USB UPLOAD\n");
                            g_power_dialog_active = false;
                            s_wait_for_menu_release = true;
                            PlayerApp_EnterUSBUpload();
                        } else if (g_power_dialog_sel == 2) {
                            /* SLEEP SETTINGS Submenu */
                            printf("[PWR] Menu -> SLEEP SETTINGS\n");
                            g_power_menu_state = POWER_MENU_SLEEP_SUBMENU;
                            g_power_dialog_sel = 0;
                            g_power_dialog_expiry_ms = now_ms + 12000;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_POWER_DIALOG);
                        } else if (g_power_dialog_sel == 3) {
                            /* THEME SELECT Submenu */
                            printf("[PWR] Menu -> THEME SELECT\n");
                            g_power_menu_state = POWER_MENU_THEME_SUBMENU;
                            g_power_dialog_sel = (s_theme == THEME_SAKURA) ? 0 : 1;
                            g_power_dialog_expiry_ms = now_ms + 12000;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_POWER_DIALOG);
                        } else {
                            /* CANCEL */
                            printf("[PWR] Menu -> CANCEL\n");
                            g_power_dialog_active = false;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
                        }
                    } else if (g_power_menu_state == POWER_MENU_SLEEP_SUBMENU) {
                        if (g_power_dialog_sel == 0) {
                            /* Remember Setting: SCREEN SLEEP */
                            printf("[PWR] Setting saved: Mode -> SCREEN SLEEP\n");
                            s_inactivity_mode = INACTIVITY_MODE_SLEEP;
                            g_power_dialog_active = false;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
                        } else if (g_power_dialog_sel == 1) {
                            /* Remember Setting: SCREENSAVER */
                            printf("[PWR] Setting saved: Mode -> SCREENSAVER\n");
                            s_inactivity_mode = INACTIVITY_MODE_SCREENSAVER;
                            g_power_dialog_active = false;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
                        } else if (g_power_dialog_sel == 2) {
                            /* Enter SLEEP TIMER Submenu */
                            printf("[PWR] Menu -> SLEEP TIMER SUBMENU\n");
                            g_power_menu_state = POWER_MENU_TIMER_SUBMENU;
                            g_power_dialog_sel = (s_inactivity_timeout_ms <= 5000) ? 0 : ((s_inactivity_timeout_ms <= 10000) ? 1 : 2);
                            g_power_dialog_expiry_ms = now_ms + 12000;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_POWER_DIALOG);
                        } else if (g_power_dialog_sel == 3) {
                            /* Enter STYLE SELECT Submenu */
                            printf("[PWR] Menu -> SCREENSAVER STYLE SUBMENU\n");
                            g_power_menu_state = POWER_MENU_STYLE_SUBMENU;
                            g_power_dialog_sel = (s_screensaver_style == SCREENSAVER_STYLE_GIRL) ? 0 : 1;
                            g_power_dialog_expiry_ms = now_ms + 12000;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_POWER_DIALOG);
                        } else {
                            /* BACK to MAIN */
                            printf("[PWR] Submenu -> BACK to MAIN\n");
                            g_power_menu_state = POWER_MENU_MAIN;
                            g_power_dialog_sel = 2;
                            g_power_dialog_expiry_ms = now_ms + 12000;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_POWER_DIALOG);
                        }
                    } else if (g_power_menu_state == POWER_MENU_TIMER_SUBMENU) {
                        if (g_power_dialog_sel == 0) {
                            /* 5 SECONDS */
                            printf("[PWR] Setting saved: Timeout -> 5 SECONDS\n");
                            s_inactivity_timeout_ms = 5000;
                            g_power_dialog_active = false;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
                        } else if (g_power_dialog_sel == 1) {
                            /* 10 SECONDS */
                            printf("[PWR] Setting saved: Timeout -> 10 SECONDS\n");
                            s_inactivity_timeout_ms = 10000;
                            g_power_dialog_active = false;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
                        } else if (g_power_dialog_sel == 2) {
                            /* 30 SECONDS */
                            printf("[PWR] Setting saved: Timeout -> 30 SECONDS\n");
                            s_inactivity_timeout_ms = 30000;
                            g_power_dialog_active = false;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
                        } else {
                            /* BACK to SLEEP_SUBMENU */
                            printf("[PWR] Timer Submenu -> BACK to SLEEP SETTINGS\n");
                            g_power_menu_state = POWER_MENU_SLEEP_SUBMENU;
                            g_power_dialog_sel = 2;
                            g_power_dialog_expiry_ms = now_ms + 12000;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_POWER_DIALOG);
                        }
                    } else if (g_power_menu_state == POWER_MENU_STYLE_SUBMENU) {
                        if (g_power_dialog_sel == 0) {
                            /* ANIME GIRL */
                            printf("[PWR] Setting saved: Screensaver -> ANIME GIRL\n");
                            s_screensaver_style = SCREENSAVER_STYLE_GIRL;
                            g_power_dialog_active = false;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
                        } else if (g_power_dialog_sel == 1) {
                            /* SAKURA TREE */
                            printf("[PWR] Setting saved: Screensaver -> SAKURA TREE\n");
                            s_screensaver_style = SCREENSAVER_STYLE_TREE;
                            g_power_dialog_active = false;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
                        } else {
                            /* BACK to SLEEP_SUBMENU */
                            printf("[PWR] Style Submenu -> BACK to SLEEP SETTINGS\n");
                            g_power_menu_state = POWER_MENU_SLEEP_SUBMENU;
                            g_power_dialog_sel = 3;
                            g_power_dialog_expiry_ms = now_ms + 12000;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_POWER_DIALOG);
                        }
                    } else if (g_power_menu_state == POWER_MENU_THEME_SUBMENU) {
                        if (g_power_dialog_sel == 0) {
                            /* SAKURA THEME */
                            printf("[PWR] Theme Applied: SAKURA THEME\n");
                            s_theme = THEME_SAKURA;
                            g_power_dialog_active = false;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
                        } else if (g_power_dialog_sel == 1) {
                            /* HALLOWEEN THEME */
                            printf("[PWR] Theme Applied: HALLOWEEN SPECIAL EDITION\n");
                            s_theme = THEME_HALLOWEEN;
                            g_power_dialog_active = false;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_ALL);
                        } else {
                            /* BACK to MAIN */
                            printf("[PWR] Theme Submenu -> BACK to MAIN\n");
                            g_power_menu_state = POWER_MENU_MAIN;
                            g_power_dialog_sel = 3;
                            g_power_dialog_expiry_ms = now_ms + 12000;
                            s_wait_for_menu_release = true;
                            PlayerApp_UpdateDisplayEx(UI_UPDATE_POWER_DIALOG);
                        }
                    }
                }
            }
        }
        return;
    }

    /* 1. CHORD: Both onboard SW2 and SW3 held down simultaneously -> Select / Play-Pause */
    if (sw2_down && sw3_down) {
        if (!s_chord_pressed) {
            s_chord_pressed = true;
            printf("[BTN] CHORD (SW2+SW3) -> Select / Play-Pause!\n");
            PlayerApp_SelectPressed();
        }
        s_nav_up.is_down = false;
        s_nav_up.long_pressed = true;
        s_nav_down.is_down = false;
        s_nav_down.long_pressed = true;
        return;
    }

    if (!sw2_down && !sw3_down) {
        s_chord_pressed = false;
    }

    if (s_chord_pressed) {
        return;
    }

    /* --- UP (SW3 or Case UP): Short tap = Cursor UP, Long hold (>350ms) = Volume UP --- */
    if (up_active) {
        if (!s_nav_up.is_down) {
            s_nav_up.is_down = true;
            s_nav_up.press_start_ms = now_ms;
            s_nav_up.long_pressed = false;
        } else {
            if (!s_nav_up.long_pressed) {
                if ((now_ms - s_nav_up.press_start_ms) >= 350) {
                    s_nav_up.long_pressed = true;
                    s_nav_up.last_repeat_ms = now_ms;
                    printf("[BTN] UP Long Press -> Volume UP (%d)\n", g_current_volume + 1);
                    PlayerApp_VolumeUp();
                }
            } else {
                if ((now_ms - s_nav_up.last_repeat_ms) >= 120) {
                    s_nav_up.last_repeat_ms = now_ms;
                    PlayerApp_VolumeUp();
                }
            }
        }
    } else {
        if (s_nav_up.is_down) {
            s_nav_up.is_down = false;
            if (!s_nav_up.long_pressed) {
                printf("[BTN] UP Tap -> Cursor UP\n");
                PlayerApp_CursorUp();
            }
        }
    }

    /* --- DOWN (SW2 or Case DOWN): Short tap = Cursor DOWN, Long hold (>350ms) = Volume DOWN --- */
    if (down_active) {
        if (!s_nav_down.is_down) {
            s_nav_down.is_down = true;
            s_nav_down.press_start_ms = now_ms;
            s_nav_down.long_pressed = false;
        } else {
            if (!s_nav_down.long_pressed) {
                if ((now_ms - s_nav_down.press_start_ms) >= 350) {
                    s_nav_down.long_pressed = true;
                    s_nav_down.last_repeat_ms = now_ms;
                    printf("[BTN] DOWN Long Press -> Volume DOWN (%d)\n", g_current_volume - 1);
                    PlayerApp_VolumeDown();
                }
            } else {
                if ((now_ms - s_nav_down.last_repeat_ms) >= 120) {
                    s_nav_down.last_repeat_ms = now_ms;
                    PlayerApp_VolumeDown();
                }
            }
        }
    } else {
        if (s_nav_down.is_down) {
            s_nav_down.is_down = false;
            if (!s_nav_down.long_pressed) {
                printf("[BTN] DOWN Tap -> Cursor DOWN\n");
                PlayerApp_CursorDown();
            }
        }
    }

    /* --- SELECT (SW4 or Case SELECT):
     * Short tap (< 1.8s) on release = Select / Play-Pause
     * Long hold (>= 1.8s) = Multi-Level System Menu
     */
    if (sel_active) {
        if (!s_nav_sel.is_down) {
            s_nav_sel.is_down = true;
            s_nav_sel.press_start_ms = now_ms;
            s_nav_sel.long_pressed = false;
        } else {
            if (!s_nav_sel.long_pressed && (now_ms - s_nav_sel.press_start_ms >= 1800)) {
                s_nav_sel.long_pressed = true;
                g_power_dialog_active = true;
                g_power_menu_state = POWER_MENU_MAIN;
                g_power_dialog_sel = 0; /* Default to POWER OFF */
                g_power_dialog_expiry_ms = now_ms + 12000;
                s_wait_for_menu_release = true; /* CRITICAL: Prevent releasing this long-press from firing selection! */
                printf("[PWR] Long press (1.8s) -> Show Main Power Menu\n");
                PlayerApp_UpdateDisplayEx(UI_UPDATE_POWER_DIALOG);
            }
        }
    } else {
        if (s_nav_sel.is_down) {
            s_nav_sel.is_down = false;
            if (!s_nav_sel.long_pressed) {
                if ((now_ms - s_nav_sel.press_start_ms >= 30) && (now_ms - s_select_cooldown_ms >= 300)) {
                    s_select_cooldown_ms = now_ms;
                    printf("[BTN] SELECT Short Tap -> Select / Play-Pause\n");
                    PlayerApp_SelectPressed();
                }
            }
        }
    }
}

bool PlayerApp_HandleCommand(const char *cmd)
{
    if (cmd == NULL) return false;

    char cleanCmd[48];
    size_t src = 0;
    size_t dst = 0;

    while (cmd[src] != '\0' && isspace((unsigned char)cmd[src])) src++;
    while (cmd[src] != '\0' && dst < sizeof(cleanCmd) - 1) {
        if (cmd[src] != '\r' && cmd[src] != '\n') {
            cleanCmd[dst++] = cmd[src];
        }
        src++;
    }
    cleanCmd[dst] = '\0';

    if (strlen(cleanCmd) == 0) return false;

    printf("[BLE CMD] Received: '%s'\n", cleanCmd);

    s_last_user_activity_ms = get_uptime_ms();
    if (g_screensaver_active) PlayerApp_ExitScreensaver();
    if (g_screen_sleeping)    PlayerApp_WakeScreen();

    if (strcasecmp(cleanCmd, "PLAY") == 0) {
        if (AudioPlayer_GetState() == AUDIO_STATE_PAUSED) {
            AudioPlayer_Resume();
        } else {
            AudioPlayer_PlayTrack(AudioPlayer_GetCurrentTrackIndex());
        }
        PlayerApp_UpdateStatusLEDs();
        PlayerApp_SendStatus("BLE_PLAY");
        PlayerApp_UpdateDisplay();
        return true;
    } else if (strcasecmp(cleanCmd, "PAUSE") == 0) {
        AudioPlayer_Pause();
        PlayerApp_UpdateStatusLEDs();
        PlayerApp_SendStatus("BLE_PAUSE");
        PlayerApp_UpdateDisplay();
        return true;
    } else if (strcasecmp(cleanCmd, "STOP") == 0) {
        AudioPlayer_Stop();
        PlayerApp_UpdateStatusLEDs();
        PlayerApp_SendStatus("BLE_STOP");
        PlayerApp_UpdateDisplay();
        return true;
    } else if (strcasecmp(cleanCmd, "NEXT") == 0 || strcasecmp(cleanCmd, "FORWARD") == 0) {
        AudioPlayer_NextTrack();
        PlayerApp_UpdateStatusLEDs();
        PlayerApp_SendStatus("BLE_NEXT");
        PlayerApp_UpdateDisplay();
        return true;
    } else if (strcasecmp(cleanCmd, "PREV") == 0 || strcasecmp(cleanCmd, "BACKWARD") == 0) {
        AudioPlayer_PrevTrack();
        PlayerApp_UpdateStatusLEDs();
        PlayerApp_SendStatus("BLE_PREV");
        PlayerApp_UpdateDisplay();
        return true;
    } else if (strcasecmp(cleanCmd, "UP") == 0 || strcasecmp(cleanCmd, "CURSOR_UP") == 0) {
        PlayerApp_CursorUp();
        return true;
    } else if (strcasecmp(cleanCmd, "DOWN") == 0 || strcasecmp(cleanCmd, "CURSOR_DOWN") == 0) {
        PlayerApp_CursorDown();
        return true;
    } else if (strcasecmp(cleanCmd, "SELECT") == 0) {
        PlayerApp_SelectPressed();
        return true;
    } else if (strncasecmp(cleanCmd, "TRACK", 5) == 0 || strncasecmp(cleanCmd, "SELECT ", 7) == 0) {
        int trkNum = 1;
        const char *pNum = strpbrk(cleanCmd, "0123456789");
        if (pNum != NULL) {
            trkNum = atoi(pNum);
        }
        AudioPlayer_PlayTrack(trkNum - 1);
        PlayerApp_UpdateStatusLEDs();
        PlayerApp_SendStatus("BLE_TRACK_SELECT");
        PlayerApp_UpdateDisplay();
        return true;
    } else if (strncasecmp(cleanCmd, "VOL", 3) == 0) {
        int volVal = 15;
        const char *pNum = strpbrk(cleanCmd, "0123456789");
        if (pNum != NULL) {
            volVal = atoi(pNum);
        }
        if (volVal < 0) volVal = 0;
        if (volVal > 31) volVal = 31;
        g_current_volume = volVal;
        AudioPlayer_SetVolume(volVal);
        g_volume_overlay_active = true;
        g_volume_overlay_expiry_ms = get_uptime_ms() + 1500;
        PlayerApp_SendStatus("BLE_VOL_CHANGED");
        PlayerApp_UpdateDisplay();
        return true;
    } else if (strcasecmp(cleanCmd, "STATUS") == 0) {
        PlayerApp_SendStatus("BLE_STATUS_REQ");
        PlayerApp_UpdateDisplay();
        return true;
    }

    return false;
}

/* =========================================================================
 * USB Upload Engine (Serial Protocol over UART0)
 * ========================================================================= */

static bool s_usb_upload_active = false;

bool PlayerApp_IsUSBUploadActive(void)
{
    return s_usb_upload_active;
}

static uint16_t usb_calc_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc <<= 1;
        }
    }
    return crc;
}

static int usb_uart_read_line(char *buf, int max_len, uint32_t timeout_ms)
{
    int idx = 0;
    uint32_t t0 = get_uptime_ms();
    while (idx < max_len - 1) {
        if (MXC_UART_GetRXFIFOAvailable(MXC_UART0) > 0) {
            int c = MXC_UART_ReadCharacterRaw(MXC_UART0);
            if (c < 0) continue;
            t0 = get_uptime_ms(); /* Reset timeout when bytes are active */
            if (c == '\r') continue;
            if (c == '\n') {
                buf[idx] = '\0';
                return idx;
            }
            buf[idx++] = (char)c;
        } else {
            if (get_uptime_ms() - t0 >= timeout_ms) {
                return 0; /* Incomplete line timed out */
            }
        }
    }
    buf[idx] = '\0';
    return idx;
}

static int usb_uart_read_exact(uint8_t *buf, int len, uint32_t timeout_ms)
{
    int received = 0;
    uint32_t t0 = get_uptime_ms();
    while (received < len) {
        if (MXC_UART_GetRXFIFOAvailable(MXC_UART0) > 0) {
            int c = MXC_UART_ReadCharacterRaw(MXC_UART0);
            if (c >= 0) {
                buf[received++] = (uint8_t)c;
                t0 = get_uptime_ms();
            }
        } else {
            if (get_uptime_ms() - t0 >= timeout_ms) {
                return received;
            }
        }
    }
    return received;
}

void PlayerApp_EnterUSBUpload(void)
{
    s_usb_upload_active = true;
    printf("[USB] Entering USB Upload Mode...\n");
    fflush(stdout);

    /* 1. Stop audio playback and release I2S */
    AudioPlayer_Stop();
    MXC_I2S_Shutdown();
    AudioPlayer_SetVolume(0);

    /* 2. Indicate USB Mode with Board Blue LED (Active Low) */
    MXC_GPIO_OutSet(board_led_pins[LED_RED].port, board_led_pins[LED_RED].mask);
    MXC_GPIO_OutSet(board_led_pins[LED_GREEN].port, board_led_pins[LED_GREEN].mask);
    MXC_GPIO_OutClr(board_led_pins[LED_BLUE].port, board_led_pins[LED_BLUE].mask);

    /* 3. Render USB Trident Screen */
    display_render_usb_mode("CONNECTED TO PC", "MANAGER READY", -1, false, 0);

    /* 4. Flush UART0 RX FIFO and announce ready */
    MXC_UART_ClearRXFIFO(MXC_UART0);
    printf("PONG:SAKURA_V1\n");
    fflush(stdout);

    /* 5. Main USB Upload Loop */
    char line_buf[256];
    static uint8_t chunk_buf[16384];
    FIL upload_file;
    bool file_open = false;
    char current_upload_filename[160] = { 0 };
    uint32_t upload_total_bytes = 0;
    uint32_t upload_received_bytes = 0;
    bool show_exit_dialog = false;
    int exit_sel = 0;
    uint32_t btn_press_start_ms = 0;
    bool btn_long_pressed = false;

    while (s_usb_upload_active) {
        /* Check physical buttons for on-device exit */
        uint32_t now_ms = get_uptime_ms();
        uint32_t p2_in = MXC_GPIO_InGet(MXC_GPIO2, MXC_GPIO_PIN_0 | MXC_GPIO_PIN_1 | MXC_GPIO_PIN_2);
        bool case_down = ((p2_in & MXC_GPIO_PIN_0) == 0);
        bool case_sel  = ((p2_in & MXC_GPIO_PIN_1) == 0);
        bool case_up   = ((p2_in & MXC_GPIO_PIN_2) == 0);

        uint32_t p3_in = MXC_GPIO_InGet(MXC_GPIO3, MXC_GPIO_PIN_1);
        bool sw4_down  = ((p3_in & MXC_GPIO_PIN_1) == 0);

        bool up_pressed   = case_up || (PB_Get(1) != 0);
        bool down_pressed = case_down || (PB_Get(0) != 0);
        bool sel_pressed  = case_sel || sw4_down;

        if (sel_pressed) {
            if (btn_press_start_ms == 0) {
                btn_press_start_ms = now_ms;
                btn_long_pressed = false;
            } else if (!btn_long_pressed && (now_ms - btn_press_start_ms >= 1500)) {
                btn_long_pressed = true;
                show_exit_dialog = true;
                exit_sel = 0;
                display_render_usb_mode("USB UPLOAD MODE", "", -1, true, exit_sel);
            }
        } else {
            if (btn_press_start_ms != 0) {
                if (!btn_long_pressed && show_exit_dialog) {
                    /* Short press confirms selection */
                    if (exit_sel == 0) {
                        /* OK -> Exit USB Upload */
                        printf("[USB] User confirmed exit via on-device dialog.\n");
                        fflush(stdout);
                        if (file_open) {
                            f_sync(&upload_file);
                            f_close(&upload_file);
                            file_open = false;
                        }
                        s_usb_upload_active = false;
                        break;
                    } else {
                        /* CANCEL -> Dismiss dialog */
                        show_exit_dialog = false;
                        display_render_usb_mode("USB UPLOAD MODE", "WAITING FOR PC...", -1, false, 0);
                    }
                }
                btn_press_start_ms = 0;
                btn_long_pressed = false;
            }
        }

        if (show_exit_dialog) {
            if (up_pressed || down_pressed) {
                exit_sel = 1 - exit_sel;
                display_render_usb_mode("USB UPLOAD MODE", "", -1, true, exit_sel);
                MXC_Delay(MXC_DELAY_MSEC(250));
            }
        }

        /* Check for Serial Commands */
        int line_len = usb_uart_read_line(line_buf, sizeof(line_buf), 20);
        if (line_len <= 0) continue;

        /* Strip trailing whitespace */
        while (line_len > 0 && (line_buf[line_len - 1] == '\r' || line_buf[line_len - 1] == '\n' || line_buf[line_len - 1] == ' ')) {
            line_buf[--line_len] = '\0';
        }
        if (line_len == 0) continue;

        if (strcmp(line_buf, "PING") == 0) {
            printf("PONG:SAKURA_V1\n");
            fflush(stdout);
            display_render_usb_mode("CONNECTED TO PC", "MANAGER READY", -1, false, 0);
        } else if (strncmp(line_buf, "SET_BAUD ", 9) == 0) {
            uint32_t new_baud = (uint32_t)strtoul(line_buf + 9, NULL, 10);
            printf("BAUD_OK %lu\n", (unsigned long)new_baud);
            fflush(stdout);
            while (MXC_UART_GetActive(MXC_UART0) != E_NO_ERROR) {}
            MXC_Delay(MXC_DELAY_MSEC(40));
            if (new_baud > 921600) {
                MXC_UART_SetFrequency(MXC_UART0, new_baud, MXC_UART_APB_CLK);
            } else {
                MXC_UART_SetFrequency(MXC_UART0, new_baud, MXC_UART_IBRO_CLK);
            }
            MXC_UART_ClearRXFIFO(MXC_UART0);
        } else if (strcmp(line_buf, "STORAGE") == 0) {
            FATFS *pfs;
            DWORD fre_clust;
            FRESULT res = f_getfree("", &fre_clust, &pfs);
            if (res == FR_OK) {
                DWORD tot_sect = (pfs->n_fatent - 2) * pfs->csize;
                DWORD fre_sect = fre_clust * pfs->csize;
                uint32_t tot_mb = (uint32_t)(tot_sect / 2048);
                uint32_t fre_mb = (uint32_t)(fre_sect / 2048);
                printf("STORAGE:%lu:%lu\n", (unsigned long)tot_mb, (unsigned long)fre_mb);
            } else {
                printf("STORAGE:15200:14800\n");
            }
            fflush(stdout);
        } else if (strcmp(line_buf, "LIST") == 0) {
            DIR dir;
            FILINFO fno;
            FRESULT fr = f_opendir(&dir, "0:/");
            if (fr == FR_OK) {
                while (1) {
                    fr = f_readdir(&dir, &fno);
                    if (fr != FR_OK || fno.fname[0] == 0) break;
                    if (fno.fattrib & AM_DIR) continue;
                    int nlen = strlen(fno.fname);
                    if ((nlen > 4 && strcasecmp(fno.fname + nlen - 4, ".mp3") == 0) ||
                        (nlen > 4 && strcasecmp(fno.fname + nlen - 4, ".wav") == 0)) {
                        printf("FILE:%s:%lu\n", fno.fname, (unsigned long)fno.fsize);
                        fflush(stdout);
                    }
                }
                f_closedir(&dir);
            }
            printf("LIST_END\n");
            fflush(stdout);
        } else if (strncmp(line_buf, "DELETE ", 7) == 0) {
            char del_fn[160];
            const char *p = line_buf + 7;
            while (*p == ' ') p++;
            strncpy(del_fn, p, sizeof(del_fn) - 1);
            del_fn[sizeof(del_fn) - 1] = '\0';
            int dlen = strlen(del_fn);
            while (dlen > 0 && (del_fn[dlen - 1] == ' ' || del_fn[dlen - 1] == '\r' || del_fn[dlen - 1] == '\n')) {
                del_fn[--dlen] = '\0';
            }
            if (dlen >= 2 && del_fn[0] == '"' && del_fn[dlen - 1] == '"') {
                memmove(del_fn, del_fn + 1, dlen - 2);
                del_fn[dlen - 2] = '\0';
            }
            FRESULT fr = f_unlink(del_fn);
            if (fr == FR_OK) {
                printf("DELETE_OK\n");
            } else {
                printf("DELETE_ERR:%d\n", fr);
            }
            fflush(stdout);
        } else if (strncmp(line_buf, "UPLOAD_START ", 13) == 0) {
            char fname[160] = { 0 };
            uint32_t fsize = 0;
            const char *p = line_buf + 13;
            while (*p == ' ') p++;

            /* Format 1 (default from PC app): "UPLOAD_START <filename with spaces> <filesize>" */
            const char *last_sp = strrchr(p, ' ');
            if (last_sp && last_sp > p) {
                char *endp = NULL;
                unsigned long sz = strtoul(last_sp + 1, &endp, 10);
                if (endp && (*endp == '\0' || *endp == '\r' || *endp == '\n') && sz > 0) {
                    int nlen = (int)(last_sp - p);
                    if (nlen > 0 && nlen < (int)sizeof(fname)) {
                        strncpy(fname, p, nlen);
                        fname[nlen] = '\0';
                        fsize = (uint32_t)sz;
                    }
                }
            }

            /* Format 2 fallback: "UPLOAD_START <filesize> <filename with spaces>" */
            if (fsize == 0 || fname[0] == '\0') {
                if (isdigit((unsigned char)*p)) {
                    char *endp = NULL;
                    unsigned long sz = strtoul(p, &endp, 10);
                    if (endp && *endp == ' ') {
                        while (*endp == ' ') endp++;
                        if (*endp != '\0') {
                            strncpy(fname, endp, sizeof(fname) - 1);
                            fname[sizeof(fname) - 1] = '\0';
                            fsize = (uint32_t)sz;
                        }
                    }
                }
            }

            /* Strip optional surrounding quotes */
            int flen = strlen(fname);
            while (flen > 0 && (fname[flen - 1] == ' ' || fname[flen - 1] == '\r' || fname[flen - 1] == '\n')) {
                fname[--flen] = '\0';
            }
            if (flen >= 2 && fname[0] == '"' && fname[flen - 1] == '"') {
                memmove(fname, fname + 1, flen - 2);
                fname[flen - 2] = '\0';
            }

            if (fsize > 0 && fname[0] != '\0') {
                if (file_open) {
                    f_sync(&upload_file);
                    f_close(&upload_file);
                    file_open = false;
                }
                strncpy(current_upload_filename, fname, sizeof(current_upload_filename) - 1);
                current_upload_filename[sizeof(current_upload_filename) - 1] = '\0';
                upload_total_bytes = fsize;
                upload_received_bytes = 0;

                FRESULT fr = f_open(&upload_file, current_upload_filename, FA_CREATE_ALWAYS | FA_WRITE);
                if (fr == FR_OK) {
                    file_open = true;
                    display_render_usb_mode("RECEIVING FILE...", current_upload_filename, 0, false, 0);
                    printf("UPLOAD_READY\n");
                } else {
                    printf("UPLOAD_ERR:%d\n", fr);
                }
            } else {
                printf("UPLOAD_ERR:BAD_PARAMS\n");
            }
            fflush(stdout);
        } else if (strncmp(line_buf, "CHUNK ", 6) == 0) {
            uint32_t offset = 0, length = 0;
            uint16_t expected_crc = 0;
            if (sscanf(line_buf + 6, "%lu %lu %hx", (unsigned long *)&offset, (unsigned long *)&length, &expected_crc) >= 3) {
                if (length > sizeof(chunk_buf)) {
                    printf("CHUNK_ERR:TOO_LARGE\n");
                    fflush(stdout);
                    continue;
                }
                int r_len = usb_uart_read_exact(chunk_buf, length, 3000);
                if (r_len != (int)length) {
                    printf("NACK %lu\n", (unsigned long)offset);
                    fflush(stdout);
                    continue;
                }
                uint16_t calc_crc = usb_calc_crc16(chunk_buf, length);
                if (calc_crc != expected_crc) {
                    printf("NACK %lu\n", (unsigned long)offset);
                    fflush(stdout);
                    continue;
                }
                if (file_open) {
                    UINT bw = 0;
                    FRESULT fr = f_write(&upload_file, chunk_buf, length, &bw);
                    if (fr == FR_OK && bw == length) {
                        upload_received_bytes += length;
                        int pct = 0;
                        if (upload_total_bytes > 0) {
                            pct = (int)((upload_received_bytes * 100) / upload_total_bytes);
                            if (pct > 100) pct = 100;
                        }
                        static int last_display_pct = -1;
                        if (pct != last_display_pct && (pct % 5 == 0 || pct == 100)) {
                            last_display_pct = pct;
                            display_render_usb_mode("RECEIVING FILE...", current_upload_filename, pct, false, 0);
                        }
                        printf("ACK %lu\n", (unsigned long)offset);
                    } else {
                        printf("WRITE_ERR\n");
                    }
                } else {
                    printf("WRITE_ERR:NO_FILE\n");
                }
            } else {
                printf("CHUNK_ERR:BAD_SYNTAX\n");
            }
            fflush(stdout);
        } else if (strcmp(line_buf, "UPLOAD_FINISH") == 0) {
            if (file_open) {
                f_sync(&upload_file);
                f_close(&upload_file);
                file_open = false;
            }
            display_render_usb_mode("FILE SAVED OK!", current_upload_filename, 100, false, 0);
            printf("UPLOAD_OK\n");
            fflush(stdout);
        } else if (strcmp(line_buf, "UPLOAD_ABORT") == 0) {
            if (file_open) {
                f_close(&upload_file);
                file_open = false;
                f_unlink(current_upload_filename);
            }
            display_render_usb_mode("TRANSFER CANCELLED", "", -1, false, 0);
            printf("ABORT_OK\n");
            fflush(stdout);
        } else if (strcmp(line_buf, "DISCONNECT") == 0) {
            if (file_open) {
                f_sync(&upload_file);
                f_close(&upload_file);
                file_open = false;
            }
            printf("DISCONNECT_OK\n");
            fflush(stdout);
            while (MXC_UART_GetActive(MXC_UART0) != E_NO_ERROR) {}
            MXC_Delay(MXC_DELAY_MSEC(40));
            MXC_UART_SetFrequency(MXC_UART0, 115200, MXC_UART_IBRO_CLK);
            s_usb_upload_active = false;
            break;
        }
    }

    if (file_open) {
        f_sync(&upload_file);
        f_close(&upload_file);
    }

    printf("[USB] Exited USB Upload Mode, refreshing library...\n");
    fflush(stdout);
    display_render_splash(90, "Updating Tracks...");
    AudioPlayer_ScanTracks();
    s_last_user_activity_ms = get_uptime_ms();
    AudioPlayer_SetVolume(g_current_volume);
    PlayerApp_UpdateStatusLEDs();
    PlayerApp_UpdateDisplay();
}
