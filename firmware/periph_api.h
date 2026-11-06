/*************************************************************************************************/
/*!
 *  \file   periph_api.h
 *  \brief  Proprietary data transfer server sample application API.
 */
/*************************************************************************************************/

#ifndef AUDIO_PLAYER_PERIPH_API_H_
#define AUDIO_PLAYER_PERIPH_API_H_

#include "wsf_os.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*periphRxCb_t)(uint8_t *data, uint16_t len);

void PeriphRegisterRXCallback(periphRxCb_t cb);
bool_t PeriphTXData(uint8_t *data, uint16_t len);
void PeriphStart(void);
void PeriphHandlerInit(wsfHandlerId_t handlerId);
void PeriphHandler(wsfEventMask_t event, wsfMsgHdr_t *pMsg);

#ifdef __cplusplus
};
#endif

#endif /* AUDIO_PLAYER_PERIPH_API_H_ */
