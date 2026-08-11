/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_msc.c
 * Description        : CherryUSB MSC 大容量存储（RAM 模拟盘，10KB）。
 *                      介质为 bss 静态数组（20 扇区 × 512B），掉电丢失，
 *                      符合 RAM 盘语义；启动自动预格式化 FAT12（bss 清零后
 *                      无签名即重建），Windows 插上即用。
 *                      实验结论：10KB 卷可正常挂载/格式化（容量非限制因素）。
 ********************************************************************************/
#include "usbd_core.h"
#include "usbd_msc.h"

#define MSC_IN_EP  0x81
#define MSC_OUT_EP 0x02

/* 沿用原工程 VID/PID (WCH 0x1A86 / 0xFE0C) */
#define USBD_VID       0x1A86
#define USBD_PID       0xFE0C
#define USBD_MAX_POWER 100

#define USB_CONFIG_SIZE (9 + MSC_DESCRIPTOR_LEN)

#define MSC_MAX_MPS 64

/* ------------------------------------------------------------------------- */
/* 描述符                                                                     */
/* ------------------------------------------------------------------------- */
static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID, 0x0200, 0x01)
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x01, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    MSC_DESCRIPTOR_INIT(0x00, MSC_OUT_EP, MSC_IN_EP, MSC_MAX_MPS, 0x02)
};

static const uint8_t device_quality_descriptor[] = {
    /* device qualifier descriptor */
    0x0a,
    USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00,
    0x02,
    0x00,
    0x00,
    0x00,
    0x40,
    0x00,
    0x00,
};

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 }, /* Langid */
    "WCH",                        /* Manufacturer */
    "CH32L103 MSC RAM",           /* Product */
    "L103-MSC-20260811",          /* Serial Number（唯一，避免与旧设备实例冲突） */
};

/* 最小 BOS 描述符：避免 Windows 周期性请求 0x0F 时报错（同 CDC 版） */
static const uint8_t bos_descriptor[] = {
    0x05, /* bLength */
    USB_DESCRIPTOR_TYPE_BINARY_OBJECT_STORE, /* bDescriptorType = 0x0F */
    0x05, 0x00, /* wTotalLength = 5 */
    0x00, /* bNumDeviceCaps = 0 */
};

static const struct usb_bos_descriptor cdc_bos = {
    .string = bos_descriptor,
    .string_len = sizeof(bos_descriptor),
};

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    return config_descriptor;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    return device_quality_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    if (index >= (sizeof(string_descriptors) / sizeof(char *))) {
        return NULL;
    }
    return string_descriptors[index];
}

const struct usb_descriptor msc_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
    .bos_descriptor = &cdc_bos,
};

/* ------------------------------------------------------------------------- */
/* 事件回调：MSC 类由 usbd_msc 内部处理，无需额外逻辑                          */
/* ------------------------------------------------------------------------- */
static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    switch (event) {
        case USBD_EVENT_RESET:
        case USBD_EVENT_CONNECTED:
        case USBD_EVENT_DISCONNECTED:
        case USBD_EVENT_RESUME:
        case USBD_EVENT_SUSPEND:
        case USBD_EVENT_CONFIGURED:
        case USBD_EVENT_SET_REMOTE_WAKEUP:
        case USBD_EVENT_CLR_REMOTE_WAKEUP:
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------------------- */
/* RAM 介质：10KB（20 扇区 × 512B），bss 静态数组                             */
/*   - 掉电丢失，符合 RAM 模拟盘语义；bss 由启动代码清零                       */
/*   - 越界写返回 -1（WRITE FAULT），绝不静默丢弃                             */
/* ------------------------------------------------------------------------- */
#define BLOCK_SIZE  512
#define BLOCK_COUNT 20 /* 10KB */

__attribute__((aligned(4))) static uint8_t mass_block[BLOCK_COUNT][BLOCK_SIZE];

void usbd_msc_get_cap(uint8_t busid, uint8_t lun, uint32_t *block_num, uint32_t *block_size)
{
    *block_num = BLOCK_COUNT; /* 真实容量：20 块 × 512B = 10KB */
    *block_size = BLOCK_SIZE;
}

int usbd_msc_sector_read(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t *buffer, uint32_t length)
{
    if ((sector + (length / BLOCK_SIZE)) > BLOCK_COUNT) {
        return -1;
    }
    memcpy(buffer, &mass_block[sector][0], length);
    return 0;
}

int usbd_msc_sector_write(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t *buffer, uint32_t length)
{
    if ((sector + (length / BLOCK_SIZE)) > BLOCK_COUNT) {
        return -1;
    }
    memcpy(&mass_block[sector][0], buffer, length);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 启动预格式化 FAT12：Windows 对超小卷拒绝 format，但可挂载已有 FAT12 卷     */
/*   - 布局（20 扇区）：引导 1 + FAT 2 + 根目录 2 + 数据 15（簇=1 扇区）      */
/*   - 只写扇区 0(引导 BPB) 与 1-2(FAT×2)，根目录/数据区保持 bss 清零态       */
/*     （根目录 0x00 = 空目录结束符，数据区全 0 = 未分配，均为合法态）         */
/*   - 触发条件：扇区 0 无 0x55AA 签名；bss 清零后必然触发（每次上电重建，    */
/*     符合 RAM 盘掉电丢失语义）                                              */
/* ------------------------------------------------------------------------- */
#define FAT12_ROOT_ENTRIES 32
#define FAT12_RESERVED     1
#define FAT12_NUM_FATS     2
#define FAT12_FAT_SECTORS  1

static const uint8_t fat12_boot_sector[BLOCK_SIZE] = {
    0xEB, 0x3C, 0x90,                       /* BS_jmpBoot */
    'M', 'S', 'D', 'O', 'S', '5', '.', '0', /* BS_OEMName */
    0x00, 0x02,                             /* BPB_BytsPerSec = 512 */
    0x01,                                   /* BPB_SecPerClus = 1 */
    FAT12_RESERVED, 0x00,                   /* BPB_RsvdSecCnt = 1 */
    FAT12_NUM_FATS,                         /* BPB_NumFATs = 2 */
    FAT12_ROOT_ENTRIES, 0x00,               /* BPB_RootEntCnt = 32 */
    BLOCK_COUNT, 0x00,                      /* BPB_TotSec16 = 20 */
    0xF8,                                   /* BPB_Media = fixed disk */
    FAT12_FAT_SECTORS, 0x00,                /* BPB_FATSz16 = 1 */
    0x3F, 0x00,                             /* BPB_SecPerTrk = 63 */
    0xFF, 0x00,                             /* BPB_NumHeads = 255 */
    0x00, 0x00, 0x00, 0x00,                 /* BPB_HiddSec = 0 */
    0x00, 0x00, 0x00, 0x00,                 /* BPB_TotSec32 = 0 */
    0x80,                                   /* BS_DrvNum */
    0x00,                                   /* BS_Reserved1 */
    0x29,                                   /* BS_BootSig */
    0x12, 0x34, 0x56, 0x78,                 /* BS_VolID */
    'C', 'H', '3', '2', 'L', '1', '0', '3', ' ', ' ', ' ', /* BS_VolLab (11B) */
    'F', 'A', 'T', '1', '2', ' ', ' ', ' ', /* BS_FilSysType (8B) */
    /* 引导代码区（0x3E 起）全 0：USB 盘不引导，Windows 挂载不执行 */
    [510] = 0x55, [511] = 0xAA,             /* 签名 */
};

static void msc_preattach_format(void)
{
    uint8_t fat_table[BLOCK_SIZE] = { 0xF8, 0xFF, 0xFF }; /* FAT[0]=0xFF8, FAT[1]=0xFFF，其余 0=空闲 */

    if (*(volatile uint16_t *)&mass_block[0][510] == 0xAA55) {
        return; /* 已是合法 FAT12 卷 */
    }
    /* bss 已清零，直接写 BPB + FAT×2 到扇区 0-2；根目录/数据区保持 0 即可 */
    memcpy(&mass_block[0][0], fat12_boot_sector, BLOCK_SIZE);
    memcpy(&mass_block[1][0], fat_table, BLOCK_SIZE);
    memcpy(&mass_block[2][0], fat_table, BLOCK_SIZE);
}

/* ------------------------------------------------------------------------- */
/* 对外接口                                                                   */
/* ------------------------------------------------------------------------- */
static struct usbd_interface intf0;

void msc_ram_init(uint8_t busid)
{
    msc_preattach_format(); /* bss 清零后自动重建 FAT12 卷（纯内存，微秒级） */
    usbd_desc_register(busid, &msc_descriptor);
    /* usbd_msc_init_intf 自注册端点（含 usbd_add_endpoint），应用勿重复添加 */
    usbd_add_interface(busid, usbd_msc_init_intf(busid, &intf0, MSC_OUT_EP, MSC_IN_EP));
#ifdef CONFIG_CH32_USBFS
    usbd_initialize(busid, 0x50000000, usbd_event_handler); /* USBFS 基址 */
#else
    usbd_initialize(busid, 0x40005C00, usbd_event_handler); /* USBD (F103 兼容) 基址 */
#endif
}
