/******************************************************************************
 *
 * Audio Player Application Controller & BLE Command Handler
 *
 ******************************************************************************/

#ifndef PLAYER_APP_H_
#define PLAYER_APP_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*playerSender_t)(const char *msg);

typedef enum {
    UI_UPDATE_ALL = 0,
    UI_UPDATE_CURSOR,
    UI_UPDATE_VOLUME,
    UI_UPDATE_POWER_DIALOG,
    UI_UPDATE_TIMELINE
} ui_update_type_t;

typedef enum {
    POWER_MENU_MAIN = 0,
    POWER_MENU_SLEEP_SUBMENU = 1,
    POWER_MENU_TIMER_SUBMENU = 2,
    POWER_MENU_STYLE_SUBMENU = 3,
    POWER_MENU_THEME_SUBMENU = 4
} power_menu_t;

typedef enum {
    THEME_SAKURA    = 0,
    THEME_HALLOWEEN = 1
} player_theme_t;

typedef enum {
    SCREENSAVER_STYLE_GIRL = 0,
    SCREENSAVER_STYLE_TREE = 1
} screensaver_style_t;

typedef enum {
    INACTIVITY_MODE_SCREENSAVER = 0,
    INACTIVITY_MODE_SLEEP       = 1
} inactivity_mode_t;

void PlayerApp_Init(void);
void PlayerApp_SetSender(playerSender_t sender);
bool PlayerApp_HandleCommand(const char *cmd);
void PlayerApp_SW2_Pressed(void);
void PlayerApp_SW3_Pressed(void);
void PlayerApp_SW4_Pressed(void);
void PlayerApp_CursorUp(void);
void PlayerApp_CursorDown(void);
void PlayerApp_SelectPressed(void);
int  PlayerApp_GetCursorTrackIndex(void);
void PlayerApp_SetCursorTrackIndex(int idx);
void PlayerApp_ProcessEvents(void);
uint32_t PlayerApp_GetUptimeMs(void);
void PlayerApp_SendStatus(const char *event);
void PlayerApp_UpdateStatusLEDs(void);
void PlayerApp_UpdateDisplay(void);
void PlayerApp_UpdateDisplayEx(ui_update_type_t type);
void PlayerApp_PowerOff(void);
void PlayerApp_VolumeUp(void);
void PlayerApp_VolumeDown(void);
int  PlayerApp_GetVolume(void);
void PlayerApp_EnterScreenSleep(void);
void PlayerApp_WakeScreen(void);
bool PlayerApp_IsScreenSleeping(void);
void PlayerApp_EnterScreensaver(void);
void PlayerApp_ExitScreensaver(void);
bool PlayerApp_IsScreensaverActive(void);
inactivity_mode_t PlayerApp_GetInactivityMode(void);
void PlayerApp_SetInactivityMode(inactivity_mode_t mode);
uint32_t PlayerApp_GetInactivityTimeoutMs(void);
void PlayerApp_SetInactivityTimeoutMs(uint32_t ms);
player_theme_t PlayerApp_GetTheme(void);
void PlayerApp_SetTheme(player_theme_t theme);
screensaver_style_t PlayerApp_GetScreensaverStyle(void);
void PlayerApp_SetScreensaverStyle(screensaver_style_t style);
void PlayerApp_EnterUSBUpload(void);
bool PlayerApp_IsUSBUploadActive(void);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_APP_H_ */
