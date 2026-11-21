/* Just enough of mbedtls' Base64 for the host tests: ha_disco.c encodes the
 * live plant picture with it, and the tests build without ESP-IDF. Same
 * contract as the real one where ha_disco.c relies on it: with too small a
 * buffer it stores the size needed, terminator included, and fails. */
#ifndef STUB_MBEDTLS_BASE64_H
#define STUB_MBEDTLS_BASE64_H

#include <stddef.h>

static inline int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                                        const unsigned char *src, size_t slen)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = (slen + 2) / 3 * 4;
    if (!dst || dlen < need + 1) {
        *olen = need + 1;
        return -0x002A;   /* MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL */
    }
    size_t o = 0;
    for (size_t i = 0; i < slen; i += 3) {
        unsigned v = (unsigned)src[i] << 16;
        if (i + 1 < slen) v |= (unsigned)src[i + 1] << 8;
        if (i + 2 < slen) v |= src[i + 2];
        dst[o++] = (unsigned char)tab[(v >> 18) & 63];
        dst[o++] = (unsigned char)tab[(v >> 12) & 63];
        dst[o++] = (unsigned char)(i + 1 < slen ? tab[(v >> 6) & 63] : '=');
        dst[o++] = (unsigned char)(i + 2 < slen ? tab[v & 63] : '=');
    }
    dst[o] = '\0';
    *olen = o;
    return 0;
}

#endif
