#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* MSDK peripheral drivers */
#include "board.h"
#include "dma.h"
#include "i2c.h"
#include "i2s.h"
#include "icc.h"
#include "led.h"
#include "max9867.h"
#include "mxc_delay.h"
#include "mxc_device.h"
#include "mxc_sys.h"
#include "nvic_table.h"

/* FatFS */
#include "ff.h"

/* Cordio WSF */
#include "wsf_os.h"

/* minimp3 */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#include "audio_player.h"
#include "player_app.h"
#include "sd_spi.h"

#define I2C_MASTER      MXC_I2C1
#define CODEC_MCLK_HZ   12288000

#define PCM_BUF_SAMPLES (MINIMP3_MAX_SAMPLES_PER_FRAME * 2)
#define PCM_BUF_BYTES   (PCM_BUF_SAMPLES * sizeof(int16_t))
#define MP3_IN_CHUNK    (16 * 1024)
#define MAX_TRACKS 16

static mp3dec_t        g_mp3d;
static FATFS           g_fatfs;
static FIL             g_file;

static int16_t         g_pcm_buf[2][PCM_BUF_SAMPLES];
static volatile int    g_tx_done = 0;
static volatile audio_state_t g_state = AUDIO_STATE_STOPPED;
static uint8_t         g_mp3_in[MP3_IN_CHUNK];
static bool            g_sd_mounted = false;

static char            g_track_files[MAX_TRACKS][64];
static char            g_track_names[MAX_TRACKS][64];
static int             g_num_tracks = 0;
static int             g_current_track_idx = 0;
static int             g_target_track_idx = 0;
static volatile bool   g_change_requested = false;

static volatile bool g_pump_active = false;
static volatile uint32_t g_track_file_size = 0;
static volatile uint32_t g_track_bytes_read = 0;
static volatile uint32_t g_samples_decoded = 0;
static volatile int      g_track_sample_rate = 44100;

int AudioPlayer_GetElapsedSec(void)
{
    if (g_track_sample_rate <= 0) return 0;
    return (int)(g_samples_decoded / g_track_sample_rate);
}

int AudioPlayer_GetTotalSec(void)
{
    int elapsed = AudioPlayer_GetElapsedSec();
    if (g_track_bytes_read <= 0 || g_track_file_size <= 0) return 0;
    int est = (int)(((uint64_t)elapsed * g_track_file_size) / g_track_bytes_read);
    return (est > 0) ? est : 0;
}

int AudioPlayer_GetProgressPct(void)
{
    if (g_track_file_size <= 0) return 0;
    int pct = (int)(((uint64_t)g_track_bytes_read * 100) / g_track_file_size);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

void AudioPlayer_Pump(void)
{
    /* Keep WSF dispatcher active during SPI flushes */
    wsfOsDispatcher();
}

static void i2s_dma_cb(int handle, int error) { g_tx_done = 1; }
static void DMA_IRQHandler(void) { MXC_DMA_Handler(); }

static int i2s_init(int sample_rate)
{
    MXC_SYS_ClockSourceEnable(MXC_SYS_CLOCK_ERFO);
    while (!(MXC_GCR->clkctrl & MXC_F_GCR_CLKCTRL_ERFO_RDY)) {}

    mxc_i2s_req_t req = { 0 };
    req.wordSize    = MXC_I2S_WSIZE_HALFWORD;
    req.sampleSize  = MXC_I2S_SAMPLESIZE_SIXTEEN;
    req.bitsWord    = 32;
    req.adjust      = MXC_I2S_ADJUST_LEFT;
    req.justify     = MXC_I2S_MSB_JUSTIFY;
    req.wsPolarity  = MXC_I2S_POL_NORMAL;
    req.channelMode = MXC_I2S_EXTERNAL_SCK_EXTERNAL_WS;
    req.stereoMode  = MXC_I2S_STEREO;
    req.bitOrder    = MXC_I2S_MSB_FIRST;
    req.clkdiv      = MXC_I2S_CalculateClockDiv(sample_rate, req.wordSize);
    req.rawData     = g_pcm_buf[0];
    req.txData      = g_pcm_buf[0];
    req.rxData      = NULL;
    req.length      = PCM_BUF_SAMPLES;

    int err = MXC_I2S_Init(&req);
    if (err != E_NO_ERROR) { printf("[I2S] Init failed: %d\n", err); return err; }

    MXC_I2S_RegisterDMACallback(i2s_dma_cb);
    MXC_NVIC_SetVector(DMA0_IRQn, DMA_IRQHandler);
    NVIC_EnableIRQ(DMA0_IRQn);
    return E_NO_ERROR;
}

int g_dbg_pmic_status = -1;
int g_dbg_codec_status = -1;

static const mxc_gpio_cfg_t s_fthr_i2c1 = {
    .port   = MXC_GPIO0,
    .mask   = MXC_GPIO_PIN_16 | MXC_GPIO_PIN_17,
    .func   = MXC_GPIO_FUNC_ALT1,
    .pad    = MXC_GPIO_PAD_WEAK_PULL_UP,
    .vssel  = MXC_GPIO_VSSEL_VDDIO,
    .drvstr = MXC_GPIO_DRVSTR_0
};

static uint8_t s_pmic_addr = 0x28;

static int pmic_write_reg(uint8_t reg, uint8_t val)
{
    mxc_i2c_req_t req;
    uint8_t buf[2] = { reg, val };
    req.i2c = I2C_MASTER; req.addr = s_pmic_addr;
    req.tx_buf = buf; req.tx_len = 2;
    req.rx_buf = NULL; req.rx_len = 0;
    req.restart = 0; req.callback = NULL;
    return MXC_I2C_MasterTransaction(&req);
}

static int pmic_write_buf(uint8_t reg, uint8_t *data, int len)
{
    mxc_i2c_req_t req;
    uint8_t buf[16];
    buf[0] = reg; memcpy(&buf[1], data, len);
    req.i2c = I2C_MASTER; req.addr = s_pmic_addr;
    req.tx_buf = buf; req.tx_len = len + 1;
    req.rx_buf = NULL; req.rx_len = 0;
    req.restart = 0; req.callback = NULL;
    return MXC_I2C_MasterTransaction(&req);
}

static int pmic_read_reg(uint8_t reg, uint8_t *dest)
{
    mxc_i2c_req_t req;
    uint8_t buf[1] = { reg };
    req.i2c = I2C_MASTER; req.addr = s_pmic_addr;
    req.tx_buf = buf; req.tx_len = 1;
    req.rx_buf = dest; req.rx_len = 1;
    req.restart = 0; req.callback = NULL;
    return MXC_I2C_MasterTransaction(&req);
}

static int pmic_enable_buck2(void)
{
    int err;
    uint8_t cmd_data[4];
    uint8_t resp = 0;

    MXC_SYS_ClockEnable(MXC_SYS_PERIPH_CLOCK_I2C1);
    MXC_I2C_Init(I2C_MASTER, TRUE, 0);
    MXC_GPIO_Config(&s_fthr_i2c1);
    MXC_I2C_SetFrequency(I2C_MASTER, 100000);
    MXC_I2C_SetTimeout(I2C_MASTER, 25000);

    /* Try 0x28 (7-bit address), then 0x50 (8-bit) */
    uint8_t hw_id = 0;
    s_pmic_addr = 0x28;
    for (int retry = 0; retry < 5; retry++) {
        err = pmic_read_reg(0x00, &hw_id);
        if (err == E_NO_ERROR && hw_id == 0x02) break;
        MXC_Delay(MXC_DELAY_MSEC(5));
    }

    if (hw_id != 0x02) {
        s_pmic_addr = 0x50;
        for (int retry = 0; retry < 5; retry++) {
            err = pmic_read_reg(0x00, &hw_id);
            if (err == E_NO_ERROR && hw_id == 0x02) break;
            MXC_Delay(MXC_DELAY_MSEC(5));
        }
    }

    if (hw_id != 0x02) {
        printf("[PMIC] Not detected (hw_id=0x%02X, err=%d)\n", hw_id, err);
        return -1;
    }
    printf("[PMIC] Found MAX20303 HW ID=0x%02X (addr=0x%02X)\n", hw_id, s_pmic_addr);

    cmd_data[0] = 0x00; cmd_data[1] = 0x01; cmd_data[2] = 0x14; cmd_data[3] = 0x00;
    pmic_write_buf(0x0F, cmd_data, 4);
    pmic_write_reg(0x17, 0x3A);
    MXC_Delay(MXC_DELAY_MSEC(10));
    pmic_read_reg(0x18, &resp);
    printf("[PMIC] BUCK2 resp=0x%02X\n", resp);
    return 0;
}

volatile FRESULT g_dbg_mount_fr = 99;

static int sd_mount(void)
{
    if (!sd_spi_is_card_inserted()) {
        printf("[SD] Card not detected.\n");
        g_sd_mounted = false;
        return E_NO_DEVICE;
    }
    FRESULT fr = f_mount(&g_fatfs, "0:", 1);
    g_dbg_mount_fr = fr;
    if (fr != FR_OK) { printf("[SD] Mount failed: %d\n", fr); g_sd_mounted = false; return E_COMM_ERR; }
    printf("[SD] Card mounted OK.\n");
    g_sd_mounted = true;
    return E_NO_ERROR;
}

static bool is_audio_file(const char *name)
{
    if (!name) return false;
    size_t len = strlen(name);
    if (len < 4) return false;
    const char *ext = name + len - 4;
    return (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0);
}

int AudioPlayer_ScanTracks(void)
{
    g_num_tracks = 0;
    if (g_sd_mounted) {
        DIR dir; FILINFO fno;
        FRESULT fr = f_opendir(&dir, "0:/");
        if (fr == FR_OK) {
            while (g_num_tracks < (MAX_TRACKS - 1)) {
                fr = f_readdir(&dir, &fno);
                if (fr != FR_OK || fno.fname[0] == 0) break;
                if (fno.fattrib & AM_DIR) continue;
                if (is_audio_file(fno.fname)) {
                    snprintf(g_track_files[g_num_tracks], sizeof(g_track_files[0]), "0:/%s", fno.fname);
                    snprintf(g_track_names[g_num_tracks], sizeof(g_track_names[0]), "%s", fno.fname);
                    printf("[PLAYLIST] Track [%d]: %s\n", g_num_tracks+1, fno.fname);
                    g_num_tracks++;
                }
            }
            f_closedir(&dir);
        }
    }
    if (g_num_tracks == 0 && g_sd_mounted) {
        const char *defaults[] = {
            "0:/track1.mp3","0:/track2.mp3","0:/1.mp3","0:/2.mp3",
            "0:/music1.mp3","0:/music2.mp3","0:/track.mp3"
        };
        int nd = sizeof(defaults)/sizeof(defaults[0]);
        for (int i = 0; i < nd && g_num_tracks < (MAX_TRACKS - 1); i++) {
            FIL t;
            if (f_open(&t, defaults[i], FA_READ) == FR_OK) {
                f_close(&t);
                snprintf(g_track_files[g_num_tracks], sizeof(g_track_files[0]), "%s", defaults[i]);
                const char *s = strrchr(defaults[i], '/');
                snprintf(g_track_names[g_num_tracks], sizeof(g_track_names[0]), "%s", s ? s+1 : defaults[i]);
                printf("[PLAYLIST] Fallback [%d]: %s\n", g_num_tracks+1, g_track_names[g_num_tracks]);
                g_num_tracks++;
            }
        }
    }

    /* Built-in Audio Test Tone directly in firmware Flash/ROM */
    if (g_num_tracks < MAX_TRACKS) {
        snprintf(g_track_files[g_num_tracks], sizeof(g_track_files[0]), "ROM:TEST_TONE");
        snprintf(g_track_names[g_num_tracks], sizeof(g_track_names[0]), "TEST TONE (ROM)");
        g_num_tracks++;
    }

    printf("[PLAYLIST] Total: %d tracks\n", g_num_tracks);
    return g_num_tracks;
}

bool AudioPlayer_Init(void)
{
    printf("[AUDIO] Init...\n");

    /* 1. PMIC BUCK2 */
    g_dbg_pmic_status = pmic_enable_buck2();

    /* 2. Enable 12.288 MHz active crystal oscillator (P0.14) */
    mxc_gpio_cfg_t osc;
    osc.port  = MXC_GPIO0;
    osc.mask  = MXC_GPIO_PIN_14;
    osc.func  = MXC_GPIO_FUNC_OUT;
    osc.pad   = MXC_GPIO_PAD_NONE;
    osc.vssel = MXC_GPIO_VSSEL_VDDIO;
    MXC_GPIO_Config(&osc);
    MXC_GPIO_OutSet(osc.port, osc.mask);
    MXC_Delay(MXC_DELAY_MSEC(30));

    /* 3. Initialize I2S at 48kHz */
    if (i2s_init(48000) != E_NO_ERROR) {
        printf("[AUDIO] I2S init failed.\n");
    }
    MXC_Delay(MXC_DELAY_MSEC(50));

    /* 4. Initialize MAX9867 Audio Codec */
    g_dbg_codec_status = max9867_init(I2C_MASTER, CODEC_MCLK_HZ, 48000);
    if (g_dbg_codec_status != E_NO_ERROR) {
        printf("[CODEC] Init failed err=%d\n", g_dbg_codec_status);
    } else {
        printf("[CODEC] OK.\n");
        max9867_set_volume(PlayerApp_GetVolume());
    }

    /* 5. Mount SD */
    if (sd_mount() != E_NO_ERROR) {
        printf("[AUDIO] SD Mount Failed.\n");
    }
    AudioPlayer_ScanTracks();
    return true;
}

static bool parse_wav_header(FIL *file, uint32_t *sr, uint16_t *ch, uint16_t *bps, uint32_t *doff)
{
    UINT br; uint8_t hdr[12];
    f_lseek(file, 0);
    if (f_read(file,hdr,12,&br)!=FR_OK||br<12) return false;
    if (memcmp(hdr,"RIFF",4)||memcmp(hdr+8,"WAVE",4)) return false;
    uint32_t off=12, fsz=f_size(file);
    *ch=2; *sr=44100; *bps=16; bool got_fmt=false;
    while (off+8<fsz) {
        uint8_t ch8[8]; f_lseek(file,off);
        if (f_read(file,ch8,8,&br)!=FR_OK||br<8) break;
        uint32_t csz=ch8[4]|(ch8[5]<<8)|(ch8[6]<<16)|(ch8[7]<<24);
        if (!memcmp(ch8,"fmt ",4)) {
            uint8_t fmt[16];
            if (f_read(file,fmt,16,&br)==FR_OK&&br>=16) {
                uint16_t fc=fmt[0]|(fmt[1]<<8);
                *ch=fmt[2]|(fmt[3]<<8);
                *sr=fmt[4]|(fmt[5]<<8)|(fmt[6]<<16)|(fmt[7]<<24);
                *bps=fmt[14]|(fmt[15]<<8);
                if (fc==1) got_fmt=true;
            }
        } else if (!memcmp(ch8,"data",4) && got_fmt) { *doff=off+8; return true; }
        off+=8+csz;
    }
    return false;
}


static void play_builtin_test(void)
{
    printf("[TEST] Playing 48kHz sine test tone...\n");
    MXC_I2S_Shutdown();
    if (i2s_init(48000) != E_NO_ERROR || max9867_init(I2C_MASTER, CODEC_MCLK_HZ, 48000) != E_NO_ERROR) {
        printf("[TEST] Re-init failed.\n");
        return;
    }
    max9867_set_volume(PlayerApp_GetVolume());

    static const int16_t s_sine1k[48] = {
            0,  2139,  4240,  6269,  8191,  9973, 11585, 12998,
        14188, 15136, 15825, 16244, 16383, 16244, 15825, 15136,
        14188, 12998, 11585,  9973,  8191,  6269,  4240,  2139,
            0, -2139, -4240, -6269, -8191, -9973,-11585,-12998,
       -14188,-15136,-15825,-16244,-16383,-16244,-15825,-15136,
       -14188,-12998,-11585, -9973, -8191, -6269, -4240, -2139
    };

    int buf_sel = 0;
    int play_sel = 0;
    bool dma_running = false;
    uint32_t total_blocks = 0;
    g_track_file_size = 60 * 48000;
    g_track_bytes_read = 0;
    g_samples_decoded = 0;
    g_track_sample_rate = 48000;
    g_pump_active = true;

    while (g_state != AUDIO_STATE_STOPPED && !g_change_requested) {
        wsfOsDispatcher();
        if (!dma_running) {
            PlayerApp_ProcessEvents();
        }
        while (g_state == AUDIO_STATE_PAUSED && !g_change_requested) {
            wsfOsDispatcher();
            PlayerApp_ProcessEvents();
            MXC_Delay(MXC_DELAY_MSEC(10));
        }
        if (g_state == AUDIO_STATE_STOPPED || g_change_requested) break;

        int pitch_mod = (total_blocks / 20) % 2;
        for (int i = 0; i < PCM_BUF_SAMPLES / 2; i++) {
            int16_t sample = pitch_mod ? s_sine1k[i % 48] : s_sine1k[(i / 2) % 48];
            g_pcm_buf[buf_sel][i * 2]     = sample;
            g_pcm_buf[buf_sel][i * 2 + 1] = sample;
        }
        total_blocks++;
        g_samples_decoded += PCM_BUF_SAMPLES / 2;
        g_track_bytes_read = (g_samples_decoded % (60 * 48000));

        if (dma_running) {
            while (!g_tx_done && g_state != AUDIO_STATE_STOPPED && !g_change_requested) {
                wsfOsDispatcher();
            }
            if (g_state == AUDIO_STATE_STOPPED || g_change_requested) break;
        }

        g_tx_done = 0;
        play_sel  = buf_sel;
        MXC_DMA_ReleaseChannel(0);
        MXC_I2S_TXDMAConfig(g_pcm_buf[play_sel], PCM_BUF_BYTES);
        dma_running = true;

        buf_sel ^= 1;

        PlayerApp_ProcessEvents();
    }

    g_pump_active = false;

    if (dma_running) {
        while (!g_tx_done && !g_change_requested) {
            wsfOsDispatcher();
            PlayerApp_ProcessEvents();
        }
        MXC_DMA_Stop(0);
        MXC_DMA_ReleaseChannel(0);
    }
    printf("[TEST] Done.\n");
}

static void play_wav(uint32_t sr, uint16_t ch, uint16_t bps, uint32_t doff)
{
    int buf_sel = 0;
    int play_sel = 0;
    bool dma_running = false;
    UINT br;

    printf("[WAV] %lu Hz, %d ch\n", (unsigned long)sr, ch);
    MXC_I2S_Shutdown();
    if (i2s_init((int)sr) != E_NO_ERROR || max9867_init(I2C_MASTER, CODEC_MCLK_HZ, (int)sr) != E_NO_ERROR) {
        printf("[WAV] Re-init failed.\n");
        return;
    }
    max9867_set_volume(PlayerApp_GetVolume());
    f_lseek(&g_file, doff);

    g_track_file_size = f_size(&g_file);
    g_track_bytes_read = f_tell(&g_file);
    g_samples_decoded = 0;
    g_track_sample_rate = sr;
    g_pump_active = true;

    while (g_state != AUDIO_STATE_STOPPED && !g_change_requested) {
        wsfOsDispatcher();
        if (!dma_running) {
            PlayerApp_ProcessEvents();
        }
        while (g_state == AUDIO_STATE_PAUSED && !g_change_requested) {
            wsfOsDispatcher();
            PlayerApp_ProcessEvents();
            MXC_Delay(MXC_DELAY_MSEC(10));
        }
        if (g_state == AUDIO_STATE_STOPPED || g_change_requested) break;

        UINT btr = PCM_BUF_BYTES;
        if (ch == 1) btr = (PCM_BUF_SAMPLES / 2) * sizeof(int16_t);
        f_read(&g_file, g_pcm_buf[buf_sel], btr, &br);
        if (br == 0) break;

        int rs = br / sizeof(int16_t), pb;
        if (ch == 1 && rs > 0) {
            for (int i = rs - 1; i >= 0; i--) {
                g_pcm_buf[buf_sel][i * 2]     = g_pcm_buf[buf_sel][i];
                g_pcm_buf[buf_sel][i * 2 + 1] = g_pcm_buf[buf_sel][i];
            }
            pb = rs * 2 * sizeof(int16_t);
        } else {
            pb = br;
        }

        g_samples_decoded += (pb / (2 * sizeof(int16_t)));
        g_track_bytes_read = f_tell(&g_file);

        if (dma_running) {
            while (!g_tx_done && g_state != AUDIO_STATE_STOPPED && !g_change_requested) {
                wsfOsDispatcher();
            }
            if (g_state == AUDIO_STATE_STOPPED || g_change_requested) break;
        }

        g_tx_done = 0;
        play_sel  = buf_sel;
        MXC_DMA_ReleaseChannel(0);
        MXC_I2S_TXDMAConfig(g_pcm_buf[play_sel], pb);
        dma_running = true;

        buf_sel ^= 1;

        /* Maximum cushion window: DMA has just started transmitting (26.1ms buffer).
         * Safely process events, button presses, screensaver ticks, and UI updates now */
        PlayerApp_ProcessEvents();
    }

    g_pump_active = false;

    if (dma_running) {
        while (!g_tx_done && !g_change_requested) {
            wsfOsDispatcher();
            PlayerApp_ProcessEvents();
        }
        MXC_DMA_Stop(0);
        MXC_DMA_ReleaseChannel(0);
    }
    printf("[WAV] Done.\n");
}

static void play_mp3(void)
{
    int buf_sel = 0;
    int play_sel = 0;
    uint32_t bytes_in = 0;
    bool eof = false, first_frame = true, dma_running = false;
    int sample_rate = 0, channels = 0;
    UINT br;

    mp3dec_init(&g_mp3d);
    printf("[MP3] Starting...\n");

    g_track_file_size = f_size(&g_file);
    g_track_bytes_read = 0;
    g_samples_decoded = 0;
    g_track_sample_rate = 44100;
    g_pump_active = true;

    while (g_state != AUDIO_STATE_STOPPED && !g_change_requested) {
        wsfOsDispatcher();
        if (!dma_running) {
            PlayerApp_ProcessEvents();
        }
        while (g_state == AUDIO_STATE_PAUSED && !g_change_requested) {
            wsfOsDispatcher();
            PlayerApp_ProcessEvents();
            MXC_Delay(MXC_DELAY_MSEC(10));
        }
        if (g_state == AUDIO_STATE_STOPPED || g_change_requested) break;

        /* Refill input buffer in bulk only when data drops below 4096 bytes.
         * Reading in large chunks prevents the 38Hz periodic SD SPI line tapping noise! */
        if (!eof && bytes_in < 4096) {
            uint32_t space = MP3_IN_CHUNK - bytes_in;
            f_read(&g_file, g_mp3_in + bytes_in, space, &br);
            bytes_in += br;
            if (br == 0) eof = true;
        }
        if (bytes_in == 0) break;

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&g_mp3d, g_mp3_in, (int)bytes_in, g_pcm_buf[buf_sel], &info);

        if (info.frame_bytes == 0) {
            if (eof) { printf("[MP3] EOF.\n"); break; }
            /* Skip 1 byte to avoid infinite loop on corrupt bitstream data */
            if (bytes_in > 1) { memmove(g_mp3_in, g_mp3_in + 1, bytes_in - 1); bytes_in--; }
            else bytes_in = 0;
            continue;
        }

        bytes_in -= info.frame_bytes;
        if (bytes_in > 0) memmove(g_mp3_in, g_mp3_in + info.frame_bytes, bytes_in);
        if (samples == 0) continue;

        if (first_frame) {
            sample_rate = info.hz;
            channels = info.channels;
            first_frame = false;
            printf("[MP3] %d Hz, %d ch\n", sample_rate, channels);
            MXC_I2S_Shutdown();
            if (i2s_init(sample_rate) != E_NO_ERROR) { printf("[MP3] I2S reinit fail\n"); break; }
            if (max9867_init(I2C_MASTER, CODEC_MCLK_HZ, sample_rate) != E_NO_ERROR) {
                printf("[MP3] Codec reinit failed\n");
            }
            max9867_set_volume(PlayerApp_GetVolume());
            g_track_sample_rate = sample_rate;
        }

        int pcm_bytes;
        if (info.channels == 1 && samples > 0) {
            for (int i = samples - 1; i >= 0; i--) {
                g_pcm_buf[buf_sel][i * 2]     = g_pcm_buf[buf_sel][i];
                g_pcm_buf[buf_sel][i * 2 + 1] = g_pcm_buf[buf_sel][i];
            }
            pcm_bytes = samples * 2 * sizeof(int16_t);
        } else {
            pcm_bytes = samples * info.channels * sizeof(int16_t);
        }

        g_samples_decoded += samples;
        uint32_t curr_pos = f_tell(&g_file);
        g_track_bytes_read = (curr_pos > bytes_in) ? (curr_pos - bytes_in) : curr_pos;

        if (dma_running) {
            while (!g_tx_done && g_state != AUDIO_STATE_STOPPED && !g_change_requested) {
                wsfOsDispatcher();
            }
            if (g_state == AUDIO_STATE_STOPPED || g_change_requested) break;
        }

        g_tx_done = 0;
        play_sel  = buf_sel;
        MXC_DMA_ReleaseChannel(0);
        MXC_I2S_TXDMAConfig(g_pcm_buf[play_sel], pcm_bytes);
        dma_running = true;

        buf_sel ^= 1;

        /* Maximum cushion window: DMA has just started streaming 1152 samples (26.1ms).
         * Safely process events, button presses, screensaver ticks, and UI updates now with zero stutter */
        PlayerApp_ProcessEvents();
    }

    g_pump_active = false;

    if (dma_running) {
        while (!g_tx_done && !g_change_requested) {
            wsfOsDispatcher();
            PlayerApp_ProcessEvents();
        }
        MXC_DMA_Stop(0);
        MXC_DMA_ReleaseChannel(0);
    }
    printf("[MP3] Done.\n");
}

void AudioPlayer_Stop(void)   { g_state=AUDIO_STATE_STOPPED; }
void AudioPlayer_Pause(void)  { if (g_state==AUDIO_STATE_PLAYING) { g_state=AUDIO_STATE_PAUSED; printf("[AUDIO] Paused.\n"); } }
void AudioPlayer_Resume(void) { if (g_state==AUDIO_STATE_PAUSED)  { g_state=AUDIO_STATE_PLAYING; printf("[AUDIO] Resumed.\n"); } }

audio_state_t AudioPlayer_PlayPauseToggle(void)
{
    if (g_state==AUDIO_STATE_PLAYING)      AudioPlayer_Pause();
    else if (g_state==AUDIO_STATE_PAUSED)  AudioPlayer_Resume();
    else                                   AudioPlayer_PlayTrack(g_current_track_idx);
    return g_state;
}

bool          AudioPlayer_IsPlaying(void)             { return g_state==AUDIO_STATE_PLAYING; }
bool          AudioPlayer_IsPaused(void)              { return g_state==AUDIO_STATE_PAUSED; }
audio_state_t AudioPlayer_GetState(void)              { return g_state; }
int           AudioPlayer_GetCurrentTrackIndex(void)  { return g_current_track_idx; }
int           AudioPlayer_GetTotalTracks(void)        { return g_num_tracks; }
bool          AudioPlayer_IsSDMounted(void)           { return g_sd_mounted; }
void          AudioPlayer_SetVolume(int vol)          { max9867_set_volume(vol); }

int AudioPlayer_GetActiveTrackIndex(void)
{
    if (g_change_requested) return g_target_track_idx;
    return g_current_track_idx;
}

const char *AudioPlayer_GetActiveTrackFilename(void)
{
    int idx = AudioPlayer_GetActiveTrackIndex();
    if (g_num_tracks>0 && idx>=0 && idx<g_num_tracks)
        return g_track_names[idx];
    return "NONE";
}

bool AudioPlayer_IsChangeRequested(void)
{
    return g_change_requested;
}

const char *AudioPlayer_GetCurrentTrackFilename(void)
{
    if (g_num_tracks>0 && g_current_track_idx>=0 && g_current_track_idx<g_num_tracks)
        return g_track_names[g_current_track_idx];
    return "NONE";
}

const char *AudioPlayer_GetTrackName(int idx)
{
    if (idx>=0 && idx<g_num_tracks) return g_track_names[idx];
    return "";
}

static void play_track_internal(int track_index)
{
    if (g_num_tracks==0) AudioPlayer_ScanTracks();
    if (g_num_tracks==0) { printf("[AUDIO] No tracks.\n"); return; }
    if (track_index<0) track_index=0;
    if (track_index>=g_num_tracks) track_index=g_num_tracks-1;

    g_current_track_idx=track_index;
    const char *fp = g_track_files[g_current_track_idx];
    printf("[AUDIO] Track [%d/%d]: %s\n", g_current_track_idx+1, g_num_tracks, fp);

    if (strcmp(fp, "ROM:TEST_TONE") == 0) {
        g_state=AUDIO_STATE_PLAYING;
        PlayerApp_SetCursorTrackIndex(track_index);
        PlayerApp_UpdateStatusLEDs(); PlayerApp_UpdateDisplay();
        play_builtin_test();
        MXC_I2S_Shutdown();
        if (!g_change_requested) {
            g_state=AUDIO_STATE_STOPPED;
            PlayerApp_UpdateStatusLEDs(); PlayerApp_UpdateDisplay();
        }
        return;
    }

    if (!g_sd_mounted) {
        if (sd_mount()!=E_NO_ERROR) { printf("[AUDIO] SD not mounted.\n"); return; }
    }

    FRESULT fr = f_open(&g_file, fp, FA_READ);
    if (fr!=FR_OK) {
        printf("[AUDIO] Open failed fr=%d\n", fr);
        g_state=AUDIO_STATE_STOPPED;
        PlayerApp_UpdateStatusLEDs(); PlayerApp_UpdateDisplay();
        return;
    }

    g_state=AUDIO_STATE_PLAYING;
    PlayerApp_SetCursorTrackIndex(track_index);
    PlayerApp_UpdateStatusLEDs(); PlayerApp_UpdateDisplay();

    uint32_t rr; uint16_t rc, rb; uint32_t ro; bool is_wav=false;
    if (parse_wav_header(&g_file,&rr,&rc,&rb,&ro)) is_wav=true;
    else { size_t cl=strlen(fp); if (cl>4 && strcasecmp(fp+cl-4,".wav")==0) is_wav=true; }

    if (is_wav) {
        uint32_t wr=44100; uint16_t wc=2, wbp=16; uint32_t wdo=44;
        if (parse_wav_header(&g_file,&wr,&wc,&wbp,&wdo)) play_wav(wr,wc,wbp,wdo);
        else printf("[AUDIO] Bad WAV header.\n");
    } else {
        play_mp3();
    }

    f_close(&g_file);
    MXC_I2S_Shutdown();

    if (!g_change_requested) {
        if (g_num_tracks > 1) {
            printf("[AUDIO] Track finished naturally, auto-advancing to next track...\n");
            AudioPlayer_NextTrack();
        } else {
            g_state = AUDIO_STATE_STOPPED;
            PlayerApp_UpdateStatusLEDs();
            PlayerApp_UpdateDisplay();
        }
    }
}

bool AudioPlayer_PlayTrack(int track_index)
{
    if (g_num_tracks==0) AudioPlayer_ScanTracks();
    if (g_num_tracks==0) return false;
    if (track_index<0) track_index=0;
    if (track_index>=g_num_tracks) track_index=g_num_tracks-1;
    g_target_track_idx=track_index;
    g_change_requested=true;
    PlayerApp_SetCursorTrackIndex(track_index);
    return true;
}

bool AudioPlayer_NextTrack(void)
{
    if (g_num_tracks==0) AudioPlayer_ScanTracks();
    if (g_num_tracks==0) return false;
    return AudioPlayer_PlayTrack((g_current_track_idx+1)%g_num_tracks);
}

bool AudioPlayer_PrevTrack(void)
{
    if (g_num_tracks==0) AudioPlayer_ScanTracks();
    if (g_num_tracks==0) return false;
    return AudioPlayer_PlayTrack((g_current_track_idx-1+g_num_tracks)%g_num_tracks);
}

static bool g_last_card_inserted = true;

void AudioPlayer_Process(void)
{
    if (g_state != AUDIO_STATE_PLAYING) {
        bool ins = sd_spi_is_card_inserted();
        if (ins != g_last_card_inserted) {
            g_last_card_inserted = ins;
            if (!ins) { printf("[SD] Removed!\n"); g_sd_mounted=false; g_num_tracks=0; PlayerApp_UpdateDisplay(); }
            else { printf("[SD] Inserted!\n"); sd_mount(); AudioPlayer_ScanTracks(); PlayerApp_UpdateDisplay(); }
        }
    }
    if (g_change_requested) {
        g_change_requested=false;
        play_track_internal(g_target_track_idx);
    }
}

void AudioPlayer_PowerDown(void)
{
    AudioPlayer_Stop();
    MXC_I2S_Shutdown();
    max9867_shutdown();

    /* Disable PMIC BUCK2 (1.8V audio supply) */
    uint8_t cmd_data[4] = { 0x00, 0x00, 0x14, 0x00 };
    pmic_write_buf(0x0F, cmd_data, 4);
    pmic_write_reg(0x17, 0x3A);

    /* Disable 12.288MHz active oscillator (P0.14) */
    mxc_gpio_cfg_t osc;
    osc.port  = MXC_GPIO0;
    osc.mask  = MXC_GPIO_PIN_14;
    osc.func  = MXC_GPIO_FUNC_OUT;
    osc.pad   = MXC_GPIO_PAD_NONE;
    osc.vssel = MXC_GPIO_VSSEL_VDDIO;
    MXC_GPIO_Config(&osc);
    MXC_GPIO_OutClr(osc.port, osc.mask);

    /* Disable SD Socket Power (P0.15) */
    mxc_gpio_cfg_t sd_pwr = { MXC_GPIO0, MXC_GPIO_PIN_15, MXC_GPIO_FUNC_OUT, MXC_GPIO_PAD_NONE, MXC_GPIO_VSSEL_VDDIO, MXC_GPIO_DRVSTR_0 };
    MXC_GPIO_Config(&sd_pwr);
    MXC_GPIO_OutClr(sd_pwr.port, sd_pwr.mask);
}