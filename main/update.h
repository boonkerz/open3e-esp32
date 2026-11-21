/* One update for everything: firmware and storage partition from a single
 * package (update_pkg.h), uploaded through the browser or fetched by the
 * device itself.
 *
 * Order of events, chosen so that every failure leaves a device that starts:
 *
 *   1. The firmware part is streamed into the spare app partition, which the
 *      running system does not use. The storage part is collected in PSRAM.
 *   2. Only when the package is complete and both checksums match is anything
 *      in use touched: the user's files are read into memory, the storage
 *      partition is rewritten from PSRAM, the files are written back.
 *   3. Last of all the boot partition is switched.
 *
 * A transfer that breaks off therefore changes nothing. The one window that
 * remains is a power cut during step 2, which is seconds long and is why the
 * interface asks for a backup first.
 */
#ifndef O3E_UPDATE_H
#define O3E_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where the device looks for packages. Nothing is ever fetched unasked. */
#define UPDATE_SITE "https://esp32can.thomas-peterson.de"

/* A backup counts as "taken before the update" for this long. */
#define UPDATE_BACKUP_VALID_S 3600

typedef enum {
    UPDATE_IDLE = 0,
    UPDATE_CHECKING,     /* asking the site which version it has */
    UPDATE_RECEIVING,    /* upload or download in progress */
    UPDATE_INSTALLING,   /* rewriting the storage partition */
    UPDATE_DONE,         /* installed; the device restarts */
    UPDATE_FAILED,
} update_state_t;

typedef struct {
    update_state_t state;
    bool     online;           /* fetched by the device, not uploaded */
    uint32_t total, received;
    char     version[33];      /* of the package being installed */
    char     error[160];
    int      kept, keep_failed;
    /* What the last check found. */
    char     avail_channel[8];
    char     avail_version[33];
    uint32_t avail_size;
    int32_t  backup_age_s;     /* -1: no backup downloaded since boot */
} update_status_t;

void update_status(update_status_t *out);
const char *update_state_name(update_state_t s);

/* The backup rule: an update is refused unless /api/export was downloaded
 * within UPDATE_BACKUP_VALID_S. The export handler reports here. */
void update_note_backup(void);

/* Upload route. begin() claims the updater, feed() takes the body as it
 * arrives, finish() installs. After a true from finish() the caller restarts
 * the device. The signatures of feed() and abort() are what the HTTP upload
 * loop expects. */
bool update_begin(size_t total, char *err, size_t err_sz);
bool update_feed(const void *data, size_t len, char *err, size_t err_sz);
bool update_finish(char *err, size_t err_sz);
void update_abort(void);

/* Online route; both return at once and report through update_status().
 * `channel` is "release" or "dev". install restarts the device by itself. */
bool update_online_check(const char *channel, char *err, size_t err_sz);
bool update_online_install(const char *channel, char *err, size_t err_sz);

#endif /* O3E_UPDATE_H */
