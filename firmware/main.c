/*************************************************************************************************/
/*!
 * @file    main.c
 * @brief   Combined BLE Peripheral & SD Card Audio Player for MAX32655FTHR
 *
 *  Features:
 *    - Reads & plays MP3 and WAV music files from microSD card via MAX9867 codec
 *    - Controls tracks & playback using onboard buttons:
 *        * SW2 (P0.2): Backward Track (Previous Song)
 *        * SW3 (P0.3): Forward Track  (Next Song)
 *        * SW4 (P3.1): Pause & Play toggle
 *    - Full wireless BLE remote control (PLAY, PAUSE, STOP, NEXT, PREV, TRACK <N>, VOL <N>)
 *    - Real-time status notifications transmitted over BLE GATT custom service
 *    - RGB LED visual indicators (Green = Playing, Blue = Paused, Red = Stopped)
 */
/*************************************************************************************************/

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "icc.h"
#include "mxc_device.h"
#include "mxc_sys.h"
#include "mxc_delay.h"
#include "nvic_table.h"
#include "pb.h"
#include "wsf_types.h"
#include "wsf_trace.h"
#include "wsf_bufio.h"
#include "wsf_msg.h"
#include "wsf_assert.h"
#include "wsf_buf.h"
#include "wsf_heap.h"
#include "wsf_cs.h"
#include "wsf_timer.h"
#include "wsf_os.h"

#include "sec_api.h"
#include "hci_handler.h"
#include "dm_handler.h"
#include "l2c_handler.h"
#include "att_handler.h"
#include "smp_handler.h"
#include "l2c_api.h"
#include "att_api.h"
#include "smp_api.h"
#include "app_api.h"
#include "hci_core.h"
#include "app_terminal.h"

#if defined(HCI_TR_EXACTLE) && (HCI_TR_EXACTLE == 1)
#include "ll_init_api.h"
#endif

#include "pal_bb.h"
#include "pal_cfg.h"

#include "periph_api.h"
#include "app_ui.h"
#include "player_app.h"
#include "audio_player.h"
#include "display.h"
#include "uart.h"

#define PLATFORM_UART_TERMINAL_BUFFER_SIZE 2048U
#define MAX_COMMAND_LEN 64U

static wsfBufPoolDesc_t mainPoolDesc[] = { { 16, 8 }, { 32, 4 }, { 192, 8 }, { 256, 8 } };

int PreInit(void)
{
    return 0;
}

extern void (*const __isr_vector[])(void);

void SystemInit(void)
{
    /* Wipe all NVIC pending and enabled interrupts before enabling IRQ */
    for (int i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    SCB->VTOR = (uint32_t)__isr_vector;
    MXC_ICC_Enable(MXC_ICC0);
    SCB->CPACR |= SCB_CPACR_CP10_Msk | SCB_CPACR_CP11_Msk;
    __DSB();
    __ISB();

    MXC_SYS_Clock_Select(MXC_SYS_CLOCK_IPO);
    MXC_SYS_SetClockDiv(MXC_SYS_CLOCK_DIV_1);
    SystemCoreClockUpdate();

    __enable_irq();
}

#if defined(HCI_TR_EXACTLE) && (HCI_TR_EXACTLE == 1)
static LlRtCfg_t mainLlRtCfg;
#endif

extern void StackInitPeriph(void);

static void sendCommandReply(const char *msg)
{
    if (msg == NULL) return;

    char frame[MAX_COMMAND_LEN + 2];
    size_t len = strlen(msg);

    if (len >= sizeof(frame) - 2) {
        len = sizeof(frame) - 2;
    }

    memcpy(frame, msg, len);
    frame[len++] = '\n';
    frame[len] = '\0';

    if (!PeriphTXData((uint8_t *)frame, (uint16_t)len)) {
        printf("BLE reply send buffer / connection not ready\n");
    }
}

volatile bool g_sw2_pending = false;
volatile bool g_sw3_pending = false;
volatile bool g_sw4_pending = false;

static void sw2_handler(void *pb) { (void)pb; g_sw2_pending = true; }
static void sw3_handler(void *pb) { (void)pb; g_sw3_pending = true; }

/* SW4 pin (Wakeup / PB2 / P3.1) */
static const mxc_gpio_cfg_t sw4_pin = {
    MXC_GPIO3, MXC_GPIO_PIN_1, MXC_GPIO_FUNC_IN, MXC_GPIO_PAD_PULL_UP, MXC_GPIO_VSSEL_VDDIO, MXC_GPIO_DRVSTR_0
};

static void sw4_gpio_handler(void *cbdata)
{
    (void)cbdata;
    g_sw4_pending = true;
}

static void GPIOWAKE_IRQHandler(void)
{
    MXC_GPIO_Handler(3);
}

static void appRxCallback(uint8_t *data, uint16_t len)
{
    char cmd[MAX_COMMAND_LEN + 1];
    size_t cmdLen = 0;

    if (data == NULL || len == 0) return;

    cmdLen = (len < MAX_COMMAND_LEN) ? len : MAX_COMMAND_LEN;
    memcpy(cmd, data, cmdLen);
    cmd[cmdLen] = '\0';

    if (cmdLen > 0 && cmd[cmdLen - 1] == '\n') {
        cmd[--cmdLen] = '\0';
    }

    printf("BLE RX command: %s\n", cmd);

    if (strncmp(cmd, "PING", 4) == 0) {
        sendCommandReply("PONG");
    } else if (PlayerApp_HandleCommand(cmd)) {
        /* handled by player app */
    } else {
        sendCommandReply("UNKNOWN_CMD");
    }
}

static void mainWsfInit(void)
{
#if defined(HCI_TR_EXACTLE) && (HCI_TR_EXACTLE == 1)
    static BbRtCfg_t mainBbRtCfg;

    PalBbLoadCfg((PalBbCfg_t *)&mainBbRtCfg);
    LlGetDefaultRunTimeCfg(&mainLlRtCfg);
    PalCfgLoadData(PAL_CFG_ID_LL_PARAM, &mainLlRtCfg.maxAdvSets, sizeof(LlRtCfg_t) - 9);
#endif

    uint32_t memUsed;
    WsfCsEnter();
    memUsed = WsfBufIoUartInit(WsfHeapGetFreeStartAddress(), PLATFORM_UART_TERMINAL_BUFFER_SIZE);
    WsfHeapAlloc(memUsed);
    WsfCsExit();

#if defined(HCI_TR_EXACTLE) && (HCI_TR_EXACTLE == 1)
    const uint16_t maxRptBufSize = 12 + 2 + 255;
    const uint16_t aclBufSize = 12 + mainLlRtCfg.maxAclLen + 4 + BB_DATA_PDU_TAILROOM;

    mainPoolDesc[2].len = maxRptBufSize;
    mainPoolDesc[2].num = mainLlRtCfg.maxAdvReports;
    mainPoolDesc[3].len = aclBufSize;
    mainPoolDesc[3].num = mainLlRtCfg.numTxBufs + mainLlRtCfg.numRxBufs;
#endif

    const uint8_t numPools = sizeof(mainPoolDesc) / sizeof(mainPoolDesc[0]);

    WsfCsEnter();
    memUsed = WsfBufInit(numPools, mainPoolDesc);
    WsfHeapAlloc(memUsed);
    WsfCsExit();

    WsfOsInit();
    WsfTimerInit();

#if (WSF_TOKEN_ENABLED == TRUE) || (WSF_TRACE_ENABLED == TRUE)
    WsfTraceRegisterHandler(WsfBufIoWrite);
    WsfTraceEnable(TRUE);
#endif

    AppTerminalInit();

#if 0
#if defined(HCI_TR_EXACTLE) && (HCI_TR_EXACTLE == 1)
    WsfCsEnter();
    LlInitRtCfg_t llCfg = { .pBbRtCfg = &mainBbRtCfg,
                            .wlSizeCfg = 4,
                            .rlSizeCfg = 4,
                            .plSizeCfg = 4,
                            .pLlRtCfg = &mainLlRtCfg,
                            .pFreeMem = WsfHeapGetFreeStartAddress(),
                            .freeMemAvail = WsfHeapCountAvailable() };

    memUsed = LlInit(&llCfg);
    WsfHeapAlloc(memUsed);
    WsfCsExit();

    bdAddr_t bdAddr;
    PalCfgLoadData(PAL_CFG_ID_BD_ADDR, bdAddr, sizeof(bdAddr_t));
    LlSetBdAddr((uint8_t *)&bdAddr);
#endif
#endif

    PlayerApp_SetSender(sendCommandReply);
    PlayerApp_Init();

    /* Register & Enable Pushbutton Callbacks */
    PB_RegisterCallback(0, (pb_callback)sw2_handler); /* SW2 -> Backward */
    PB_IntEnable(0);
    PB_RegisterCallback(1, (pb_callback)sw3_handler); /* SW3 -> Forward */
    PB_IntEnable(1);

    /* Configure SW4 (P3.1 / Port 3) GPIO Interrupt -> Pause / Play */
    MXC_GPIO_Config(&sw4_pin);
    MXC_GPIO_RegisterCallback(&sw4_pin, sw4_gpio_handler, NULL);
    MXC_GPIO_IntConfig(&sw4_pin, MXC_GPIO_INT_FALLING);
    MXC_GPIO_EnableInt(sw4_pin.port, sw4_pin.mask);
    MXC_NVIC_SetVector(GPIOWAKE_IRQn, GPIOWAKE_IRQHandler);
    NVIC_EnableIRQ(GPIOWAKE_IRQn);

#if 0
    StackInitPeriph();
    PeriphStart();

    PeriphRegisterRXCallback(appRxCallback);
#endif
}

int main(void)
{

    Board_Init();
    Console_Init();
    printf("\n[BOOT] Sakura Audio Player starting on MAX32655...\n");

    /* Immediately initialize ST7789 display and overwrite frozen screen */
    display_hw_init();
    display_render_splash(20, "Booting MSDK...");

    /* Enable 32MHz ERFO external oscillator early so both Audio and BLE have stable clock */
    MXC_SYS_ClockSourceEnable(MXC_SYS_CLOCK_ERFO);
    while (!(MXC_GCR->clkctrl & MXC_F_GCR_CLKCTRL_ERFO_RDY)) {}

    display_render_splash(45, "Mounting SD...");

    /* Initialize Audio Hardware & SD card */
    AudioPlayer_Init();

    display_render_splash(80, "Starting BLE...");

    mainWsfInit();

    /* Disable UART0 interrupts so PC USB upload protocol can poll UART0 RX without Cordio wiping FIFO */
    NVIC_DisableIRQ(UART0_IRQn);
    MXC_UART_DisableInt(MXC_UART0, 0xFFFFFFFF);
    MXC_UART_ClearFlags(MXC_UART0, 0xFFFFFFFF);

    display_render_splash(100, "Sakura Ready!");

    /* Switch from splash to active music player UI */
    PlayerApp_UpdateDisplay();

    while (1) {
        wsfOsDispatcher();
        PlayerApp_ProcessEvents();
        AudioPlayer_Process();

        if (!WsfOsActive()) {
            /* Idle */
        }
    }

    return 0;
}
