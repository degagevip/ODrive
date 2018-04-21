/*
*
* Zero-config node ID negotiation
* -------------------------------
*
* A heartbeat message is a message with a 8 byte unique serial number as payload.
* A regular message is any message that is not a heartbeat message.
*
* All nodes MUST obey these four rules:
*
* a) At a given point in time, a node MUST consider a node ID taken (by others)
*   if any of the following is true:
*     - the node received a (not self-emitted) heartbeat message with that node ID
*       within the last second
*     - the node attempted and failed at sending a heartbeat message with that
*       node ID within the last second (failed in the sense of not ACK'd)
*
* b) At a given point in time, a node MUST NOT consider a node ID self-assigned
*   if, within the last second, it did not succeed in sending a heartbeat
*   message with that node ID.
*
* c) At a given point in time, a node MUST NOT send any heartbeat message with
*   a node ID that is taken.
*
* d) At a given point in time, a node MUST NOT send any regular message with
*   a node ID that is not self-assigned.
*
* Hardware allocation
* -------------------
*   RX FIFO0:
*       - filter bank 0: heartbeat messages
*/

#include "interface_can.hpp"
#include "crc.hpp"

#include <can.h>
#include <stm32f4xx_hal.h>
#include <cmsis_os.h>

// defined in can.c
extern CAN_HandleTypeDef hcan1;
extern CAN_HandleTypeDef hcan2;
extern CAN_HandleTypeDef hcan3;

static CAN_context* ctxs[3] = { nullptr, nullptr, nullptr };

struct CAN_context* get_can_ctx(CAN_HandleTypeDef *hcan) {
#if defined(CAN1)
    if (hcan->Instance == CAN1) return ctxs[0];
#endif
#if defined(CAN2)
    if (hcan->Instance == CAN2) return ctxs[1];
#endif
#if defined(CAN3)
    if (hcan->Instance == CAN3) return ctxs[2];
#endif
    return nullptr;
}


void server_thread(CAN_context* ctx) {
    for (;;) {
        osDelay(1000);

        //uint8_t data[] = { ctx->node_id };
        uint8_t data[8];
        *(uint64_t*)data = ctx->serial_number;

        CAN_TxHeaderTypeDef header = {
            .StdId = 0x700u + ctx->node_id,
            .ExtId = 0,
            .IDE = CAN_ID_STD,
            .RTR = CAN_RTR_DATA,
            .DLC = sizeof(data),
            .TransmitGlobalTime = DISABLE
        };
        uint32_t n_mailbox;
        HAL_CAN_AddTxMessage(ctx->handle, &header, data, &n_mailbox);
    }
}

bool serve_on_can(CAN_context& ctx, CAN_TypeDef *port, uint64_t serial_number) {
    MX_CAN1_Init(); // TODO: flatten
#if defined(CAN1)
    if (port == CAN1) ctx.handle = &hcan1, ctxs[0] = &ctx; else
#endif
#if defined(CAN2)
    // TODO: move CubeMX stuff into this file so all symbols are defined
    //if (port == CAN2) ctx.handle = &hcan2, ctxs[1] = &ctx; else
#endif
#if defined(CAN3)
    if (port == CAN3) ctx.handle = &hcan3, ctxs[2] = &ctx; else
#endif
    return false; // fail if none of the above checks matched

    HAL_StatusTypeDef status;

    ctx.node_id = calc_crc<uint8_t, 1>(0, (const uint8_t*)UID_BASE, 12);
    ctx.serial_number = serial_number;

    // Set up heartbeat filter
    CAN_FilterTypeDef sFilterConfig = {
        .FilterIdHigh = ((0x700u + ctx.node_id) << 5) | (0x0 << 2), // own heartbeat (standard ID, no RTR)
        .FilterIdLow = (0x700u << 5) | (0x0 << 2), // any heartbeat (standard ID, no RTR)
        .FilterMaskIdHigh = (0x7ffu << 5) | (0x3 << 2),
        .FilterMaskIdLow = (0x780u << 5) | (0x3 << 2),
        .FilterFIFOAssignment = CAN_RX_FIFO0,
        .FilterBank = 0,
        .FilterMode = CAN_FILTERMODE_IDMASK,
        .FilterScale = CAN_FILTERSCALE_16BIT, // two 16-bit filters
        .FilterActivation = ENABLE,
        .SlaveStartFilterBank = 0
    };
    status = HAL_CAN_ConfigFilter(ctx.handle, &sFilterConfig);
    if (status != HAL_OK)
        return false;

    status = HAL_CAN_Start(ctx.handle);
    if (status != HAL_OK)
        return false;

    status = HAL_CAN_ActivateNotification(ctx.handle,
        CAN_IT_TX_MAILBOX_EMPTY |
        CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_RX_FIFO1_MSG_PENDING | /* we probably only want this */
        CAN_IT_RX_FIFO0_FULL | CAN_IT_RX_FIFO1_FULL |
        CAN_IT_RX_FIFO0_OVERRUN | CAN_IT_RX_FIFO1_OVERRUN |
        CAN_IT_WAKEUP | CAN_IT_SLEEP_ACK |
        CAN_IT_ERROR_WARNING | CAN_IT_ERROR_PASSIVE |
        CAN_IT_BUSOFF | CAN_IT_LAST_ERROR_CODE |
        CAN_IT_ERROR);
    if (status != HAL_OK)
        return false;
    
    server_thread(&ctx);
    return true;
}

void tx_complete_callback(CAN_HandleTypeDef *hcan, uint8_t mailbox_idx) {
    __asm volatile ("bkpt");
    if (!get_can_ctx(hcan))
        return;
    get_can_ctx(hcan)->TxMailboxCompleteCallbackCnt++;
}

void tx_aborted_callback(CAN_HandleTypeDef *hcan, uint8_t mailbox_idx) {
    __asm volatile ("bkpt");
    if (!get_can_ctx(hcan))
        return;
    get_can_ctx(hcan)->TxMailboxAbortCallbackCnt++;
}

void tx_error(CAN_context *ctx, uint8_t mailbox_idx) {
    // give up node_id
    // TODO: only give up node ID if the TX that failed was the heartbeat
    ctx->node_id = 0;
}

void HAL_CAN_TxMailbox0CompleteCallback(CAN_HandleTypeDef *hcan) { tx_complete_callback(hcan, 0); }
void HAL_CAN_TxMailbox1CompleteCallback(CAN_HandleTypeDef *hcan) { tx_complete_callback(hcan, 1); }
void HAL_CAN_TxMailbox2CompleteCallback(CAN_HandleTypeDef *hcan) { tx_complete_callback(hcan, 2); }
void HAL_CAN_TxMailbox0AbortCallback(CAN_HandleTypeDef *hcan) { tx_aborted_callback(hcan, 0); }
void HAL_CAN_TxMailbox1AbortCallback(CAN_HandleTypeDef *hcan) { tx_aborted_callback(hcan, 1); }
void HAL_CAN_TxMailbox2AbortCallback(CAN_HandleTypeDef *hcan) { tx_aborted_callback(hcan, 2); }

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan) {
    CAN_context *ctx = get_can_ctx(hcan);
    if (!ctx) return;

    CAN_RxHeaderTypeDef header;
    uint8_t data[8];
    HAL_StatusTypeDef status = HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &header, data);
    if (status != HAL_OK) {
        ctx->unexpected_errors++;
        return;
    }

    if ((header.StdId & 0x780u) == 0x700u) {
        ctx->received_ack++;
    } else {
        ctx->unhandled_messages++;
    }
}

void HAL_CAN_RxFifo0FullCallback(CAN_HandleTypeDef *hcan) { if (get_can_ctx(hcan)) get_can_ctx(hcan)->RxFifo0FullCallbackCnt++; }

void HAL_CAN_RxFifo1MsgPendingCallback(CAN_HandleTypeDef *hcan) { if (get_can_ctx(hcan)) get_can_ctx(hcan)->RxFifo1MsgPendingCallbackCnt++; }
void HAL_CAN_RxFifo1FullCallback(CAN_HandleTypeDef *hcan) { if (get_can_ctx(hcan)) get_can_ctx(hcan)->RxFifo1FullCallbackCnt++; }
void HAL_CAN_SleepCallback(CAN_HandleTypeDef *hcan) { if (get_can_ctx(hcan)) get_can_ctx(hcan)->SleepCallbackCnt++; }
void HAL_CAN_WakeUpFromRxMsgCallback(CAN_HandleTypeDef *hcan) { if (get_can_ctx(hcan)) get_can_ctx(hcan)->WakeUpFromRxMsgCallbackCnt++; }

void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan) {
    __asm volatile ("bkpt");
    CAN_context *ctx = get_can_ctx(hcan);
    if (!ctx) return;

    // handle transmit errors in all three mailboxes
    if (hcan->ErrorCode & HAL_CAN_ERROR_TX_ALST0)
        SET_BIT(hcan->Instance->sTxMailBox[0].TIR, CAN_TI0R_TXRQ);
    else if (hcan->ErrorCode & HAL_CAN_ERROR_TX_TERR0)
        tx_error(ctx, 0);
    hcan->ErrorCode &= ~(HAL_CAN_ERROR_TX_ALST0 | HAL_CAN_ERROR_TX_TERR0);

    if (hcan->ErrorCode & HAL_CAN_ERROR_TX_ALST1)
        SET_BIT(hcan->Instance->sTxMailBox[1].TIR, CAN_TI1R_TXRQ);
    else if (hcan->ErrorCode & HAL_CAN_ERROR_TX_TERR1)
        tx_error(ctx, 1);
    hcan->ErrorCode &= ~(HAL_CAN_ERROR_TX_ALST1 | HAL_CAN_ERROR_TX_TERR1);

    if (hcan->ErrorCode & HAL_CAN_ERROR_TX_ALST2)
        SET_BIT(hcan->Instance->sTxMailBox[2].TIR, CAN_TI2R_TXRQ);
    else if (hcan->ErrorCode & HAL_CAN_ERROR_TX_TERR2)
        tx_error(ctx, 2);
    hcan->ErrorCode &= ~(HAL_CAN_ERROR_TX_ALST2 | HAL_CAN_ERROR_TX_TERR2);

    if (hcan->ErrorCode)
        ctx->unexpected_errors++;
}

