/* The update package as tools/make_update.py writes it, read by the code the
 * firmware reads it with. The package is made by the Makefile from two small
 * stand-in files, so a change on either side that the other does not follow
 * fails here and not on a device in a boiler room.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/update_pkg.h"

static int fail;

static void ok(const char *what, int cond, const char *detail)
{
    if (!cond) {
        fail++;
        printf("  FAIL %s %s\n", what, detail ? detail : "");
    }
}

int main(int argc, char **argv)
{
    ok("CRC-32 is zlib's", update_crc32(0, "123456789", 9) == 0xCBF43926u, NULL);
    ok("and can be fed in pieces",
       update_crc32(update_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u, NULL);

    FILE *f = fopen(argc > 1 ? argv[1] : "build/update_test.bin", "rb");
    if (!f) {
        puts("cannot open the test package");
        return 1;
    }
    static uint8_t buf[64 * 1024], bad[64 * 1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    update_pkg_t p;
    const char *e = update_pkg_header(buf, n, n, &p);
    ok("header is accepted", e == NULL, e);
    ok("version comes through", strcmp(p.version, "9.8.7+test") == 0, p.version);
    /* The stand-ins: 5000 bytes of firmware, a 32 KiB storage image of which
     * 9000 bytes are used -- three flash blocks once the tail is cut. */
    ok("firmware length", p.fw_len == 5000, NULL);
    ok("storage tail is cut to whole blocks", p.st_len == 12288 && p.st_full == 32768, NULL);
    ok("total length adds up", n == UPDATE_PKG_HEADER + p.fw_len + p.st_len, NULL);
    e = update_pkg_verify(buf, n, &p);
    ok("both images check out", e == NULL, e);
    ok("the header is enough to judge it before the rest arrived",
       update_pkg_header(buf, UPDATE_PKG_HEADER, n, &p) == NULL, NULL);

    /* What goes wrong in practice. */
    e = update_pkg_header(buf, n, n - 100, &p);
    ok("a truncated upload", e && strstr(e, "incomplete"), e);
    e = update_pkg_header(buf, 10, n, &p);
    ok("less than a header", e && strstr(e, "too short"), e);

    memcpy(bad, buf, n);
    bad[20] ^= 1;
    e = update_pkg_header(bad, n, n, &p);
    ok("a damaged header", e && strstr(e, "header is damaged"), e);

    memcpy(bad, buf, n);
    bad[UPDATE_PKG_HEADER + 1234] ^= 0x40;
    ok("header alone does not notice a damaged image",
       update_pkg_header(bad, n, n, &p) == NULL, NULL);
    e = update_pkg_verify(bad, n, &p);
    ok("but the check does", e && strstr(e, "firmware"), e);

    memcpy(bad, buf, n);
    bad[n - 5] ^= 0x01;
    update_pkg_header(bad, n, n, &p);
    e = update_pkg_verify(bad, n, &p);
    ok("a damaged storage image", e && strstr(e, "storage"), e);

    memset(bad, 0, sizeof(bad));
    bad[0] = 0xE9;
    e = update_pkg_header(bad, 5000, 5000, &p);
    ok("the bare firmware file is named for what it is", e && strstr(e, "bare firmware"), e);
    memset(bad, 0x55, 200);
    e = update_pkg_header(bad, 200, 200, &p);
    ok("anything else", e && strstr(e, "not an update package"), e);

    puts(fail ? "FAILED" : "all checks passed");
    return fail ? 1 : 0;
}
