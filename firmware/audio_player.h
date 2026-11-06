/******************************************************************************
 *
 * Audio Player Driver & SD Card Track Engine - MAX32655FTHR
 *
 ******************************************************************************/

#ifndef AUDIO_PLAYER_H_
#define AUDIO_PLAYER_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_STATE_STOPPED = 0,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED
} audio_state_t;

/**
 * @brief  Initialize PMIC BUCK2, active oscillator, I2S DMA, MAX9867 codec, and mount SD Card.
 *         Scans root directory of SD card for MP3/WAV tracks.
 * @return true if initialization & SD mount successful, false otherwise.
 */
bool AudioPlayer_Init(void);

/**
 * @brief  Scan / re-scan SD card for playable audio files (.mp3, .wav).
 * @return Number of playable tracks found.
 */
int AudioPlayer_ScanTracks(void);

/**
 * @brief  Request playing selected track index (0 to total_tracks - 1).
 * @param  track_index Index of track in playlist.
 * @return true if valid track index.
 */
bool AudioPlayer_PlayTrack(int track_index);

/**
 * @brief  Toggle between Play and Pause states.
 *         If STOPPED, starts playing current/first track.
 *         If PLAYING, pauses playback.
 *         If PAUSED, resumes playback.
 * @return Current audio state after toggle.
 */
audio_state_t AudioPlayer_PlayPauseToggle(void);

/**
 * @brief  Pause current audio playback.
 */
void AudioPlayer_Pause(void);

/**
 * @brief  Resume current audio playback if paused.
 */
void AudioPlayer_Resume(void);

/**
 * @brief  Advance to the next track (Forward Track).
 * @return true on success.
 */
bool AudioPlayer_NextTrack(void);

/**
 * @brief  Go back to the previous track (Backward Track).
 * @return true on success.
 */
bool AudioPlayer_PrevTrack(void);

/**
 * @brief  Stop playback completely.
 */
void AudioPlayer_Stop(void);

/**
 * @brief  Process audio state transitions & track changes (call from main loop).
 */
void AudioPlayer_Process(void);

/**
 * @brief  Check if audio is currently playing.
 */
bool AudioPlayer_IsPlaying(void);

/**
 * @brief  Check if audio is currently paused.
 */
bool AudioPlayer_IsPaused(void);

/**
 * @brief  Get current audio state.
 */
audio_state_t AudioPlayer_GetState(void);

/**
 * @brief  Get currently loaded track index.
 */
int AudioPlayer_GetCurrentTrackIndex(void);

/**
 * @brief  Get filename of currently loaded track.
 */
const char *AudioPlayer_GetCurrentTrackFilename(void);

/**
 * @brief  Get filename of track by index (0 to total_tracks - 1).
 */
const char *AudioPlayer_GetTrackName(int track_index);

/**
 * @brief  Check if SD card filesystem is currently mounted.
 */
bool AudioPlayer_IsSDMounted(void);

/**
 * @brief  Get total number of tracks found on SD card.
 */
int AudioPlayer_GetTotalTracks(void);

/**
 * @brief  Set volume level (0 to 31).
 */
void AudioPlayer_SetVolume(int vol);

/**
 * @brief  Get currently active or requested track index.
 */
int AudioPlayer_GetActiveTrackIndex(void);

/**
 * @brief  Get currently active or requested track filename.
 */
const char *AudioPlayer_GetActiveTrackFilename(void);

/**
 * @brief  Check if a track change is currently pending.
 */
bool AudioPlayer_IsChangeRequested(void);
void AudioPlayer_Pump(void);
void AudioPlayer_PowerDown(void);
int  AudioPlayer_GetElapsedSec(void);
int  AudioPlayer_GetTotalSec(void);
int  AudioPlayer_GetProgressPct(void);

extern int g_dbg_pmic_status;
extern int g_dbg_codec_status;

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PLAYER_H_ */
