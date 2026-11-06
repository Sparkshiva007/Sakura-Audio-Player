/*************************************************************************************************/
/*!
 *  \file   periph_main.c
 *  \brief  Data transmitter BLE peripheral application.
 */
/*************************************************************************************************/

#include <stdio.h>
#include <string.h>
#include "wsf_types.h"
#include "util/bstream.h"
#include "wsf_msg.h"
#include "wsf_trace.h"
#include "wsf_buf.h"
#include "wsf_nvm.h"
#include "hci_api.h"
#include "sec_api.h"
#include "dm_api.h"
#include "smp_api.h"
#include "att_api.h"
#include "app_api.h"
#include "app_main.h"
#include "app_db.h"
#include "app_ui.h"
#include "svc_ch.h"
#include "svc_core.h"
#include "svc_wp.h"
#include "util/calc128.h"
#include "gatt/gatt_api.h"
#include "periph_api.h"

enum {
    PERIPH_GATT_SC_CCC_IDX,
    PERIPH_WP_DAT_CCC_IDX,
    PERIPH_NUM_CCC_IDX
};

static const appAdvCfg_t periphAdvCfg = {
    { 0, 0, 0 },  /* Continuous advertising - no 30-second timeout */
    { 96, 1600, 0 }
};

static const appSlaveCfg_t periphSlaveCfg = {
    1,
};

static const attCfg_t periphAttCfg = {
    15,
    241,
    ATT_MAX_TRANS_TIMEOUT,
    4
};

static const uint8_t periphAdvDataDisc[] = {
    2,
    DM_ADV_TYPE_FLAGS,
    DM_FLAG_LE_GENERAL_DISC | DM_FLAG_LE_BREDR_NOT_SUP,
    13,
    DM_ADV_TYPE_LOCAL_NAME,
    'A', 'u', 'd', 'i', 'o', '_', 'P', 'l', 'a', 'y', 'e', 'r'
};

static const attsCccSet_t periphCccSet[PERIPH_NUM_CCC_IDX] = {
    { GATT_SC_CH_CCC_HDL, ATT_CLIENT_CFG_INDICATE, DM_SEC_LEVEL_NONE },
    { WP_DAT_CH_CCC_HDL, ATT_CLIENT_CFG_NOTIFY, DM_SEC_LEVEL_NONE }
};

static struct {
    wsfHandlerId_t handlerId;
    periphRxCb_t rxCb;
    dmConnId_t connId;
    bool_t connected;
} periphCb;

static void periphDmCback(dmEvt_t *pDmEvt)
{
    dmEvt_t *pMsg;
    uint16_t len;

    len = DmSizeOfEvt(pDmEvt);

    if ((pMsg = WsfMsgAlloc(len)) != NULL) {
        memcpy(pMsg, pDmEvt, len);
        WsfMsgSend(periphCb.handlerId, pMsg);
    }
}

static void periphAttCback(attEvt_t *pEvt)
{
    attEvt_t *pMsg;

    if ((pMsg = WsfMsgAlloc(sizeof(attEvt_t) + pEvt->valueLen)) != NULL) {
        memcpy(pMsg, pEvt, sizeof(attEvt_t));
        pMsg->pValue = (uint8_t *)(pMsg + 1);
        memcpy(pMsg->pValue, pEvt->pValue, pEvt->valueLen);
        WsfMsgSend(periphCb.handlerId, pMsg);
    }
}

static void periphCccCback(attsCccEvt_t *pEvt) {}

uint8_t periphWpWriteCback(dmConnId_t connId, uint16_t handle, uint8_t operation, uint16_t offset,
                           uint16_t len, uint8_t *pValue, attsAttr_t *pAttr)
{
    printf("[BLE RX] Write Callback handle=0x%04X, len=%d: %.*s\n", handle, len, len, pValue);
    if (periphCb.rxCb != NULL) {
        periphCb.rxCb(pValue, len);
    }

    return ATT_SUCCESS;
}

static void periphSetup(dmEvt_t *pMsg)
{
    AppAdvSetData(APP_ADV_DATA_DISCOVERABLE, sizeof(periphAdvDataDisc), (uint8_t *)periphAdvDataDisc);
    AppAdvSetData(APP_ADV_DATA_CONNECTABLE, sizeof(periphAdvDataDisc), (uint8_t *)periphAdvDataDisc);
    AppAdvStart(APP_MODE_AUTO_INIT);
}

static void periphProcMsg(dmEvt_t *pMsg)
{
    uint8_t uiEvent = APP_UI_NONE;

    switch (pMsg->hdr.event) {
    case DM_RESET_CMPL_IND:
        periphSetup(pMsg);
        uiEvent = APP_UI_RESET_CMPL;
        break;

    case DM_ADV_START_IND:
        uiEvent = APP_UI_ADV_START;
        break;

    case DM_ADV_STOP_IND:
        uiEvent = APP_UI_ADV_STOP;
        AppAdvStart(APP_MODE_AUTO_INIT);
        break;

    case DM_CONN_OPEN_IND:
        periphCb.connId = (dmConnId_t)pMsg->hdr.param;
        periphCb.connected = TRUE;
        uiEvent = APP_UI_CONN_OPEN;
        break;

    case DM_CONN_CLOSE_IND:
        periphCb.connected = FALSE;
        uiEvent = APP_UI_CONN_CLOSE;
        AppAdvStart(APP_MODE_AUTO_INIT);
        break;

    default:
        break;
    }

    if (uiEvent != APP_UI_NONE) {
        AppUiAction(uiEvent);
    }
}

bool_t PeriphTXData(uint8_t *data, uint16_t len)
{
    if (!periphCb.connected) {
        return FALSE;
    }

    AttsHandleValueNtf(periphCb.connId, WP_DAT_HDL, len, data);
    return TRUE;
}

void PeriphRegisterRXCallback(periphRxCb_t cb)
{
    periphCb.rxCb = cb;
}

void PeriphHandlerInit(wsfHandlerId_t handlerId)
{
    uint8_t addr[6] = { 0 };
    APP_TRACE_INFO0("PeriphHandlerInit");
    AppGetBdAddr(addr);
    APP_TRACE_INFO6("MAC Addr: %02x:%02x:%02x:%02x:%02x:%02x", addr[5], addr[4], addr[3], addr[2],
                    addr[1], addr[0]);
    APP_TRACE_INFO1("Adv local name: %s", &periphAdvDataDisc[5]);

    periphCb.handlerId = handlerId;
    periphCb.rxCb = NULL;

    pAppSlaveCfg = (appSlaveCfg_t *)&periphSlaveCfg;
    pAppAdvCfg = (appAdvCfg_t *)&periphAdvCfg;
    pAttCfg = (attCfg_t *)&periphAttCfg;

    AppSlaveInit();
    AppServerInit();
}

void PeriphHandler(wsfEventMask_t event, wsfMsgHdr_t *pMsg)
{
    if (pMsg != NULL) {
        APP_TRACE_INFO1("Periph got evt %d", pMsg->event);

        if (pMsg->event >= ATT_CBACK_START && pMsg->event <= ATT_CBACK_END) {
            AppServerProcAttMsg(pMsg);
        } else if (pMsg->event >= DM_CBACK_START && pMsg->event <= DM_CBACK_END) {
            AppSlaveProcDmMsg((dmEvt_t *)pMsg);
        }

        periphProcMsg((dmEvt_t *)pMsg);
    }
}

void PeriphStart(void)
{
    DmRegister(periphDmCback);
    DmConnRegister(DM_CLIENT_ID_APP, periphDmCback);
    AttRegister(periphAttCback);
    AttConnRegister(AppServerConnCback);
    AttsCccRegister(PERIPH_NUM_CCC_IDX, (attsCccSet_t *)periphCccSet, periphCccCback);

    SvcCoreGattCbackRegister(GattReadCback, GattWriteCback);
    SvcCoreAddGroup();
    SvcWpCbackRegister(NULL, periphWpWriteCback);
    SvcWpAddGroup();

    GattSetSvcChangedIdx(PERIPH_GATT_SC_CCC_IDX);

    DmDevReset();
}
