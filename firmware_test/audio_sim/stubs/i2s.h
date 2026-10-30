#include "mxc_device.h"
typedef enum { MXC_I2S_WSIZE_BYTE, MXC_I2S_WSIZE_HALFWORD, MXC_I2S_WSIZE_WORD } mxc_i2s_wsize_t;
#define MXC_I2S_SAMPLESIZE_SIXTEEN 16
#define MXC_I2S_ADJUST_LEFT 0
#define MXC_I2S_LSB_JUSTIFY 0
#define MXC_I2S_POL_NORMAL 0
#define MXC_I2S_EXTERNAL_SCK_EXTERNAL_WS 0
#define MXC_I2S_LSB_FIRST 0
#define MXC_I2S_MONO_RIGHT_CH 3
#define MXC_I2S_MONO_LEFT_CH 2
typedef struct {
    int wordSize, sampleSize, bitsWord, adjust, justify, wsPolarity, channelMode, stereoMode, bitOrder;
    int clkdiv;
    void *rawData, *txData, *rxData;
    int length;
} mxc_i2s_req_t;
int MXC_I2S_Init(mxc_i2s_req_t *req);
int MXC_I2S_CalculateClockDiv(int fs, int wsize);
void MXC_I2S_RegisterDMACallback(void (*cb)(int, int));
int MXC_I2S_RXDMAConfig(void *dst, int len);
int MXC_I2S_TXDMAConfig(void *src, int len);
void MXC_I2S_RXDisable(void);
void MXC_I2S_TXDisable(void);
void MXC_I2S_Flush(void);
