/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_dc_low_level_init.c
 * Description        : USB 底层初始化 —— 通过 CONFIG_CH32_USBFS 宏在
 *                      USBD (F103 兼容 @0x40005C00) 与 USBFS (@0x50000000) 间切换。
 *                      宏由 CMake 的 CHERRYUSB_USB_PORT 选项定义，见 CMakeLists.txt。
 ********************************************************************************/
#include "usbd_core.h"
#include "ch32v20x_conf.h"

#ifdef CONFIG_CH32_USBFS
#include "usb_ch32_usbfs_reg.h"
#endif

#ifndef CONFIG_CH32_USBFS
/* =========================================================================
 * USBD (F103 兼容 @0x40005C00) 的中断挂在 IRQ36 (USB_LP_CAN1_RX0) 上，
 * 手册标注的 IRQ59 (USBFS) 属于另一外设。这里把 36 号向量接到
 * CherryUSB fsdev 端口的中断入口 USBD_IRQHandler。
 * 注：切到 CONFIG_CH32_USBFS 时由 ch32fs 端口自带 USBFS_IRQHandler，无需本包装。
 * ========================================================================= */
extern void USBD_IRQHandler(uint8_t busid);

void USB_LP_CAN1_RX0_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USB_LP_CAN1_RX0_IRQHandler(void)
{
    USBD_IRQHandler(0);
}
#endif /* !CONFIG_CH32_USBFS */

void usb_dc_low_level_init(void)
{
    RCC_ClocksTypeDef RCC_ClocksStatus = {0};

    /* USB 需要 48MHz 时钟，按系统主频选择 PLL 分频（两端口通用） */
    RCC_GetClocksFreq(&RCC_ClocksStatus);
    if (RCC_ClocksStatus.SYSCLK_Frequency == 144000000) {
        RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_Div3);
    } else if (RCC_ClocksStatus.SYSCLK_Frequency == 96000000) {
        RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_Div2);
    } else if (RCC_ClocksStatus.SYSCLK_Frequency == 48000000) {
        RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_Div1);
    }

#ifdef CONFIG_CH32_USBFS
    /* ============ USBFS (0x50000000, IRQ59)：接 FS_DP/FS_DM 的板 ============ */
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBFS, ENABLE);

    /* SIE 复位 + FIFO 清除（对照官方 EVT USBFS_Device_Init） */
    USBFSH->BASE_CTRL = USBFS_UC_RESET_SIE | USBFS_UC_CLR_ALL;
    Delay_Us(10);
    USBFSH->BASE_CTRL = 0x00;

    /* 上拉由端口 BASE_CTRL SYS_CTRL=1x（内部 1.5K）提供，无需额外配置 */
    /* 中断：端口自带 USBFS_IRQHandler（向量 59） */
    NVIC_EnableIRQ(USBFS_IRQn);

#else
    /* ============ USBD (0x40005C00, IRQ36)：当前 V203C8T6 板 ============ */
    /* USBD (F103 兼容) 时钟在 APB1，位 23 */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, ENABLE);

    /* D+/D- 引脚（PA11/PA12）输入浮空 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    GPIOA->CFGHR &= 0xFFF00FFF;
    GPIOA->OUTDR &= ~(3 << 11);
    GPIOA->CFGHR |= 0x00044000;

    /* 内部上拉（USBD 的上拉控制） */
    EXTEN->EXTEN_CTR |= EXTEN_USBD_PU_EN;

    /* 中断走 36 号向量，包装函数 USB_LP_CAN1_RX0_IRQHandler 在 usb_cdc_bridge.c */
    NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
#endif
}
