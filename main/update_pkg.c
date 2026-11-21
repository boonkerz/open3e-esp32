#include "update_pkg.h"

#include <string.h>

uint32_t update_crc32(uint32_t crc, const void *data, size_t len)
{
    /* Half a table: sixteen entries instead of 256, and megabytes per second
     * all the same. */
    static const uint32_t t[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
        0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ t[crc & 0x0F];
        crc = (crc >> 4) ^ t[crc & 0x0F];
    }
    return ~crc;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

const char *update_pkg_header(const uint8_t *buf, size_t have, size_t total,
                              update_pkg_t *out)
{
    if (have < UPDATE_PKG_HEADER || total < UPDATE_PKG_HEADER) {
        return "this file is too short to be an update package";
    }
    if (memcmp(buf, UPDATE_PKG_MAGIC, sizeof(UPDATE_PKG_MAGIC)) != 0) {
        /* The two files people have at hand instead. */
        if (buf[0] == 0xE9) {
            return "this is a bare firmware image, not an update package";
        }
        return "this file is not an update package";
    }
    if (update_crc32(0, buf, 60) != le32(buf + 60)) {
        return "the package header is damaged";
    }
    memcpy(out->version, buf + 8, 32);
    out->version[32] = '\0';
    out->fw_len = le32(buf + 40);
    out->fw_crc = le32(buf + 44);
    out->st_len = le32(buf + 48);
    out->st_crc = le32(buf + 52);
    out->st_full = le32(buf + 56);
    if (out->fw_len == 0 || out->st_len == 0 || out->st_len > out->st_full) {
        return "the package header makes no sense";
    }
    if ((uint64_t)UPDATE_PKG_HEADER + out->fw_len + out->st_len != total) {
        return "the package is incomplete";
    }
    return NULL;
}

const char *update_pkg_verify(const uint8_t *buf, size_t len, const update_pkg_t *p)
{
    if ((uint64_t)UPDATE_PKG_HEADER + p->fw_len + p->st_len != len) {
        return "the package is incomplete";
    }
    const uint8_t *fw = buf + UPDATE_PKG_HEADER;
    if (update_crc32(0, fw, p->fw_len) != p->fw_crc) {
        return "the firmware in the package is damaged";
    }
    if (update_crc32(0, fw + p->fw_len, p->st_len) != p->st_crc) {
        return "the storage image in the package is damaged";
    }
    return NULL;
}
