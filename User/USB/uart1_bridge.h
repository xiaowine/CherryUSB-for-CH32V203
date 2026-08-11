/********************************** (C) COPYRIGHT *******************************
 * File Name          : uart1_bridge.h
 * Description        : UART1 桥接层接口（串口驱动 + FIFO 数据面 + 主循环轮询）。
 ********************************************************************************/
#ifndef UART1_BRIDGE_H
#define UART1_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

/* UART1(PA9/PA10, 115200 8N1) 初始化，RX 中断进 FIFO，兼作调试串口 */
void UART1_Simple_Init(void);

/* printf 重定向入口：字符进 TX FIFO，由 cdc_poll 统一发往 UART1 */
void uart1_putc(uint8_t c);

/* 主循环轮询：UART1 RX FIFO -> USB IN；USB OUT FIFO -> UART1 TX */
void cdc_poll(void);

/* ---- 桥接数据面（USB 层与 UART1 层共享的 FIFO） ---- */
void bridge_tx_push(uint8_t c);  /* USB EP2 OUT 回调与 printf 共用入口 */
bool bridge_tx_pop(uint8_t *c);  /* 取出待发往 UART1 的字节，空则 false */
void bridge_rx_push(uint8_t c);  /* UART1 RX 中断入口 */
bool bridge_rx_pop(uint8_t *c);  /* 取出待上传 USB 的字节，空则 false */

#endif /* UART1_BRIDGE_H */
