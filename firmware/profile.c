/* OpenEar profile storage - see profile.h. Licence: MIT. */
#include "profile.h"

#include <string.h>

#include "flc.h"
#include "icc.h"
#include "mxc_device.h"
#include "mxc_errors.h"

#define PROFILE_MAGIC 0x4F454152UL /* "OEAR" */
#define PROFILE_VERSION 1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t length;
    profile_t body;
    uint32_t crc;
} stored_t;

/* flash writes go in 16-byte (128-bit) units */
#define STORED_WORDS ((sizeof(stored_t) + 15) / 16 * 4)

static uint32_t crc32(const uint8_t *d, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFUL;
    while (n--) {
        c ^= *d++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320UL & (0U - (c & 1U)));
        }
    }
    return ~c;
}

/* end of the firmware image in flash, from the linker script */
extern uint32_t __load_data, _data, _edata;

static int image_overlaps(void)
{
    uint32_t end = (uint32_t)&__load_data + ((uint32_t)&_edata - (uint32_t)&_data);
    return end > PROFILE_ADDR;
}

/* Erase + write with interrupts and the instruction cache off, as the SDK
 * requires. The caller must have stopped audio first (see main.c). */
static int flash_rewrite(const uint32_t *words, uint32_t bytes)
{
    int err;
    if (image_overlaps()) {
        return E_BAD_STATE; /* firmware grew into the profile page */
    }
    __disable_irq();
    MXC_ICC_Disable(MXC_ICC0);
    err = MXC_FLC_PageErase(PROFILE_ADDR);
    if (err == E_NO_ERROR && words) {
        err = MXC_FLC_Write(PROFILE_ADDR, bytes, (uint32_t *)words);
    }
    MXC_ICC_Enable(MXC_ICC0);
    __enable_irq();
    return err;
}

int profile_save(const profile_t *p)
{
    static uint32_t words[STORED_WORDS];
    stored_t *s = (stored_t *)words;

    memset(words, 0xFF, sizeof(words));
    s->magic = PROFILE_MAGIC;
    s->version = PROFILE_VERSION;
    s->length = sizeof(profile_t);
    s->body = *p;
    s->crc = crc32((const uint8_t *)&s->body, sizeof(profile_t));

    return flash_rewrite(words, sizeof(words));
}

int profile_load(profile_t *p)
{
    const stored_t *s = (const stored_t *)PROFILE_ADDR;

    if (s->magic != PROFILE_MAGIC || s->version != PROFILE_VERSION ||
        s->length != sizeof(profile_t)) {
        return E_NOT_SUPPORTED;
    }
    if (crc32((const uint8_t *)&s->body, sizeof(profile_t)) != s->crc) {
        return E_BAD_STATE;
    }
    *p = s->body;
    return E_NO_ERROR;
}

int profile_erase(void)
{
    return flash_rewrite(NULL, 0);
}
