/* PC simulation stubs for OpenEar's audio_io.c (see ../audio_sim.c) */
#ifndef SIM_MXC_DEVICE_H
#define SIM_MXC_DEVICE_H
#include <stdint.h>
typedef struct {
    volatile uint32_t ctrl, status, src, dst, cnt, srcrld, dstrld, cntrld;
} mxc_dma_ch_regs_t;
typedef struct {
    mxc_dma_ch_regs_t ch[8];
} mxc_dma_regs_t;
extern mxc_dma_regs_t sim_dma;
#define MXC_DMA (&sim_dma)
typedef int IRQn_Type;
#define MXC_DMA_CH_GET_IRQ(ch) ((IRQn_Type)(100 + (ch)))
void NVIC_DisableIRQ(IRQn_Type n);
void NVIC_SetPriority(IRQn_Type n, uint32_t p);
void __disable_irq(void);
void __enable_irq(void);
#define __DMB() do { } while (0)
/* 0.2.x used the DWT */
typedef struct { volatile uint32_t CTRL, CYCCNT; } sim_dwt_t;
typedef struct { volatile uint32_t DEMCR; } sim_coredebug_t;
extern sim_dwt_t sim_dwt;
extern sim_coredebug_t sim_coredebug;
#define DWT (&sim_dwt)
#define CoreDebug (&sim_coredebug)
#define CoreDebug_DEMCR_TRCENA_Msk 1u
#define DWT_CTRL_CYCCNTENA_Msk 1u
extern uint32_t SystemCoreClock;
typedef struct { int dummy; } mxc_i2c_regs_t;
extern mxc_i2c_regs_t sim_i2c1;
#define MXC_I2C1 (&sim_i2c1)
/* channel status bits (dma_regs.h); the 0.3 firmware reads/clears them via
 * OE_DMA_STATUS / OE_DMA_W1C, modelled by sim.c like the chip:
 * STATUS (bit 0) reads 1 while the channel is enabled, flags are W1C */
#define MXC_F_DMA_STATUS_STATUS (1u << 0)
#define MXC_F_DMA_STATUS_IPEND (1u << 1)
#define MXC_F_DMA_STATUS_CTZ_IF (1u << 2)
#define MXC_F_DMA_STATUS_RLD_IF (1u << 3)
#define MXC_F_DMA_STATUS_BUS_ERR (1u << 4)
#define MXC_F_DMA_STATUS_TO_IF (1u << 6)
uint32_t sim_dma_status(int ch);
void sim_dma_w1c(int ch, uint32_t f);
uint32_t sim_dma_cnt(int ch);
#define OE_DMA_STATUS(ch) sim_dma_status(ch)
#define OE_DMA_W1C(ch, f) sim_dma_w1c((ch), (f))
#define OE_DMA_CNT(ch) sim_dma_cnt(ch)
#endif
