/* The update package: firmware and storage image in one file.
 *
 * An update used to be two files in the right order, with a backup before
 * and a restore after, because the second file erased the selection. One
 * file takes the order out of the user's hands, and its checksums take out
 * the worst case of the old way: an upload that broke off after it had
 * started to overwrite the filesystem. The device receives the whole package
 * into memory and checks it before it touches anything.
 *
 * Layout, little-endian, 64-byte header followed by the two images:
 *
 *    0  magic      "O3EUPD1\0"
 *    8  version    firmware version string, NUL-padded          (32)
 *   40  fw_len     application image, as built
 *   44  fw_crc     CRC-32 of it
 *   48  st_len     storage image with the erased tail cut off
 *   52  st_crc     CRC-32 of those bytes
 *   56  st_full    size of the storage image before cutting
 *   60  hdr_crc    CRC-32 of bytes 0..59
 *
 * The storage image is a 4 MiB partition of which half is erased flash; that
 * half is not shipped. The device erases what follows the written part, which
 * gives the same bytes.
 *
 * CRC-32 is zlib's. It guards against a damaged transfer, not against a
 * forged package -- the web interface has no login either; whoever can reach
 * it can already flash anything.
 *
 * No ESP-IDF in here: tools/make_update.py writes the format, and the host
 * tests check that both sides agree.
 */
#ifndef O3E_UPDATE_PKG_H
#define O3E_UPDATE_PKG_H

#include <stddef.h>
#include <stdint.h>

#define UPDATE_PKG_HEADER 64
#define UPDATE_PKG_MAGIC  "O3EUPD1"

typedef struct {
    char     version[33];
    uint32_t fw_len, fw_crc;
    uint32_t st_len, st_crc;
    uint32_t st_full;
} update_pkg_t;

/* zlib-compatible; start with 0 and feed the previous result back in. */
uint32_t update_crc32(uint32_t crc, const void *data, size_t len);

/* Read the header of a package of `total` bytes. Returns NULL when it is
 * sound, a reason otherwise. Only the first UPDATE_PKG_HEADER bytes of `buf`
 * are looked at, so this can run before the rest has arrived. */
const char *update_pkg_header(const uint8_t *buf, size_t have, size_t total,
                              update_pkg_t *out);

/* Check both images of a complete package against the header. */
const char *update_pkg_verify(const uint8_t *buf, size_t len, const update_pkg_t *p);

#endif /* O3E_UPDATE_PKG_H */
