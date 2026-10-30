/* PC stub of the FatFS API used by recorder.c: one in-memory file. */
#ifndef FF_STUB_H
#define FF_STUB_H
#include <stdint.h>
typedef unsigned int UINT;
typedef enum { FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE } FRESULT;
typedef struct { int dummy; } FATFS;
typedef struct { int dummy; } FILINFO;
typedef struct { long pos; } FIL;
#define FA_CREATE_ALWAYS 0x08
#define FA_WRITE 0x02
FRESULT f_mount(FATFS *fs, const char *path, int opt);
FRESULT f_stat(const char *path, FILINFO *fi);
FRESULT f_open(FIL *f, const char *path, int mode);
FRESULT f_write(FIL *f, const void *buf, UINT n, UINT *bw);
FRESULT f_lseek(FIL *f, long ofs);
FRESULT f_close(FIL *f);
#endif
