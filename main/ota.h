/* Firmware update over the web interface.
 *
 * The board lives in a boiler room, so an update that boots into something
 * broken must not require a USB cable to recover. Two mechanisms cover that:
 *
 *  - The image is written to the *other* app partition; the running one is
 *    untouched until the new image has been written and verified in full.
 *  - The bootloader's rollback support is enabled, so a new image that never
 *    reports itself healthy is reverted on the next reset. ota_mark_healthy()
 *    is what makes it permanent, and it is only called once the device is
 *    actually reachable again.
 */
#ifndef O3E_OTA_H
#define O3E_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* 17 bytes because that is the width of esp_partition_t::label. */
    char     running[17];     /* partition label the current firmware runs from */
    char     version[32];
    char     idf_version[32];
    bool     pending_verify;  /* this boot is on trial and can still roll back */
    uint32_t update_size;     /* bytes accepted so far during an upload */
} ota_info_t;

void ota_info(ota_info_t *out);

/* Confirm the running image. After this the bootloader stops treating it as a
 * trial and will not revert to the previous one. */
void ota_mark_healthy(void);

/* Start the health timer: if the device is still serving requests after a
 * grace period, the running image is confirmed. Called once at boot. */
void ota_arm_health_check(void);

/* Streaming write. begin() picks the target partition, write() is called for
 * each chunk of the upload, end() validates and switches the boot partition.
 * `err` receives a reason on failure. */
bool ota_begin(size_t total_size, char *err, size_t err_sz);
bool ota_write(const void *data, size_t len, char *err, size_t err_sz);
bool ota_end(char *err, size_t err_sz);
void ota_abort(void);

/* end() in two steps, for an update that also replaces the storage partition:
 * the image is validated first, the storage is written, and only then does
 * the boot partition change -- so a failure in between leaves the device
 * starting the firmware it was running. */
bool ota_validate(char *err, size_t err_sz);
bool ota_activate(char *err, size_t err_sz);

/* ------------------------------------------------------------------ */
/* Storage partition                                                    */
/*
 * The firmware image does not carry the datapoint database or the web
 * interface -- those live on the storage partition. Updating them had meant
 * fetching a USB cable, which is the wrong answer for a device on a wall and
 * a particularly bad one while the interface is still changing daily.
 *
 * Two routes, because they have very different costs:
 *   - storage_file_*: replace one file (a few KiB). The everyday case.
 *   - storage_image_*: overwrite the whole 4 MiB partition. Needed when the
 *     datapoint database changes, and it wipes the saved selection with it.
 */

/* Replace a single file under the storage mount. `path` is checked against the
 * mount point, so an upload cannot escape it. */
bool storage_file_begin(const char *path, char *err, size_t err_sz);
bool storage_file_write(const void *data, size_t len, char *err, size_t err_sz);
bool storage_file_end(char *err, size_t err_sz);
void storage_file_abort(void);

/* Overwrite the entire storage partition with a littlefs image. The filesystem
 * is unmounted first, so the device must reboot afterwards.
 *
 * What the user put on that partition survives it: begin() reads the
 * selection, the scan result and whatever else is not part of an image into
 * memory, end() mounts the new filesystem and writes it back. That used to be
 * the user's job -- download a backup, flash, restore -- and whoever skipped
 * it lost a twenty-minute scan. The image may be shorter than the partition
 * (an update package ships it without its erased tail); end() erases the
 * rest. */
bool storage_image_begin(size_t total_size, char *err, size_t err_sz);
bool storage_image_write(const void *data, size_t len, char *err, size_t err_sz);
bool storage_image_end(char *err, size_t err_sz);
void storage_image_abort(void);
/* How the carrying-over went, after end(): files written back, files that
 * could not be. */
void storage_image_kept(int *kept, int *failed);

#endif /* O3E_OTA_H */
