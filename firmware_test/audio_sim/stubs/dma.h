#include "mxc_device.h"
typedef struct { int ch; void *source; void *dest; int len; } mxc_dma_srcdst_t;
int MXC_DMA_SetSrcReload(mxc_dma_srcdst_t srcdst);
void MXC_DMA_Handler(void);
int MXC_DMA_Stop(int ch);
int MXC_DMA_ReleaseChannel(int ch);
int MXC_DMA_ChannelGetFlags(int ch);
int MXC_DMA_ChannelClearFlags(int ch, int flags);
