/* reColossus: ISO 9660 reading for the first start (iso9660.h). A PS2 DVD carries an ISO 9660 volume (beside UDF)
 * that lists every file: the primary volume descriptor at sector 16, then the directory tree from its root record.
 * Directory records never cross a sector; a zero length byte means the rest of that sector is padding. */
#define _FILE_OFFSET_BITS 64
#include "ps5/iso9660.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define SECTOR 2048u
#define MAX_DEPTH 16

typedef struct {
    char *path; /* relative to the disc's root, with '/' */
    uint64_t offset, size;
    int dir;
} Entry;

struct RcIso {
    int fd;
    uint64_t image_size, total;
    Entry *entries;
    size_t count, capacity;
};

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int read_at(int fd, void *buf, size_t len, uint64_t at)
{
    uint8_t *b = (uint8_t *)buf;
    while (len) {
        ssize_t n = pread(fd, b, len, (off_t)at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        b += n;
        at += (uint64_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int add(RcIso *iso, const char *path, uint64_t offset, uint64_t size, int dir)
{
    if (iso->count == iso->capacity) {
        size_t cap = iso->capacity ? iso->capacity * 2 : 256;
        Entry *e = (Entry *)realloc(iso->entries, cap * sizeof *e);
        if (!e) return -1;
        iso->entries = e;
        iso->capacity = cap;
    }
    char *copy = strdup(path);
    if (!copy) return -1;
    iso->entries[iso->count++] = (Entry){copy, offset, size, dir};
    if (!dir) iso->total += size;
    return 0;
}

/* the record's name as a file name: ";1" and a lone trailing dot dropped */
static void record_name(const uint8_t *rec, char *out, size_t cap)
{
    size_t len = rec[32];
    if (len >= cap) len = cap - 1;
    memcpy(out, rec + 33, len);
    out[len] = 0;
    char *semi = strchr(out, ';');
    if (semi) *semi = 0;
    len = strlen(out);
    if (len > 1 && out[len - 1] == '.') out[len - 1] = 0;
}

static int walk(RcIso *iso, uint32_t lba, uint32_t length, const char *prefix, int depth)
{
    if (depth > MAX_DEPTH) return RC_ISO_DAMAGED;
    uint64_t at = (uint64_t)lba * SECTOR;
    if (at + length > iso->image_size || length > 64u * 1024u * 1024u) return RC_ISO_DAMAGED;
    uint8_t *dir = (uint8_t *)malloc(length ? length : 1);
    if (!dir) return RC_ISO_DAMAGED;
    if (read_at(iso->fd, dir, length, at)) {
        free(dir);
        return RC_ISO_DAMAGED;
    }
    int rc = RC_ISO_OK;
    for (uint32_t pos = 0; pos < length && rc == RC_ISO_OK;) {
        const uint8_t *rec = dir + pos;
        if (rec[0] == 0) { /* padding to the next sector */
            pos = (pos / SECTOR + 1) * SECTOR;
            continue;
        }
        if (rec[0] < 34 || pos + rec[0] > length) {
            rc = RC_ISO_DAMAGED;
            break;
        }
        const uint8_t name_len = rec[32];
        if (name_len == 1 && (rec[33] == 0 || rec[33] == 1)) { /* . and .. */
            pos += rec[0];
            continue;
        }
        char name[256], path[1024];
        record_name(rec, name, sizeof name);
        snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "/" : "", name);
        const uint32_t extent = le32(rec + 2), size = le32(rec + 10);
        const int is_dir = (rec[25] & 0x02) != 0;
        if (!is_dir && (uint64_t)extent * SECTOR + size > iso->image_size) rc = RC_ISO_DAMAGED;
        else if (add(iso, path, (uint64_t)extent * SECTOR, size, is_dir)) rc = RC_ISO_DAMAGED;
        else if (is_dir) rc = walk(iso, extent, size, path, depth + 1);
        pos += rec[0];
    }
    free(dir);
    return rc;
}

RcIso *rc_iso_open(const char *path, int *error)
{
    *error = RC_ISO_NOT_A_DISC;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    uint8_t pvd[SECTOR];
    if (fstat(fd, &st) || (uint64_t)st.st_size < 17u * SECTOR || read_at(fd, pvd, SECTOR, 16u * SECTOR) ||
        pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) {
        close(fd);
        return NULL;
    }
    RcIso *iso = (RcIso *)calloc(1, sizeof *iso);
    if (!iso) {
        close(fd);
        return NULL;
    }
    iso->fd = fd;
    iso->image_size = (uint64_t)st.st_size;
    const uint8_t *root = pvd + 156;
    int rc = walk(iso, le32(root + 2), le32(root + 10), "", 0);
    if (rc != RC_ISO_OK) {
        *error = rc;
        rc_iso_close(iso);
        return NULL;
    }
    *error = RC_ISO_OK;
    return iso;
}

uint64_t rc_iso_total(const RcIso *iso) { return iso->total; }

int rc_iso_read_file(RcIso *iso, const char *name, uint8_t **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    for (size_t i = 0; i < iso->count; i++) {
        const Entry *e = &iso->entries[i];
        if (e->dir || strchr(e->path, '/') || strcasecmp(e->path, name)) continue;
        uint8_t *buf = (uint8_t *)malloc(e->size ? e->size : 1);
        if (!buf) return RC_ISO_DAMAGED;
        if (read_at(iso->fd, buf, e->size, e->offset)) {
            free(buf);
            return RC_ISO_DAMAGED;
        }
        *data = buf;
        *size = e->size;
        return RC_ISO_OK;
    }
    return RC_ISO_NO_FILE;
}

static int make_dir(const char *path)
{
    if (mkdir(path, 0777) && errno != EEXIST) return -1;
    chmod(path, 0777);
    return 0;
}

int rc_iso_extract(RcIso *iso, const char *target, void (*progress)(unsigned percent, uint64_t total))
{
    if (make_dir(target)) return RC_ISO_WRITE;
    if (progress) progress(0, iso->total);
    const size_t chunk = 4u * 1024u * 1024u;
    uint8_t *buf = (uint8_t *)malloc(chunk);
    if (!buf) return RC_ISO_WRITE;
    uint64_t done = 0;
    unsigned shown = 0;
    int rc = RC_ISO_OK;
    for (size_t i = 0; i < iso->count && rc == RC_ISO_OK; i++) {
        const Entry *e = &iso->entries[i];
        char out[1400];
        snprintf(out, sizeof out, "%s/%s", target, e->path);
        if (e->dir) {
            if (make_dir(out)) rc = RC_ISO_WRITE;
            continue;
        }
        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            rc = RC_ISO_WRITE;
            break;
        }
        for (uint64_t at = 0; at < e->size && rc == RC_ISO_OK;) {
            size_t n = (size_t)((e->size - at) < chunk ? (e->size - at) : chunk);
            if (read_at(iso->fd, buf, n, e->offset + at)) {
                rc = RC_ISO_DAMAGED;
                break;
            }
            for (size_t w = 0; w < n;) {
                ssize_t k = write(fd, buf + w, n - w);
                if (k < 0 && errno == EINTR) continue;
                if (k <= 0) {
                    rc = RC_ISO_WRITE;
                    break;
                }
                w += (size_t)k;
            }
            at += n;
            done += n;
            unsigned percent = iso->total ? (unsigned)(done * 100 / iso->total) : 100;
            if (progress && percent != shown) progress(shown = percent, iso->total);
        }
        if (close(fd)) rc = RC_ISO_WRITE;
    }
    free(buf);
    return rc;
}

void rc_iso_close(RcIso *iso)
{
    if (!iso) return;
    for (size_t i = 0; i < iso->count; i++) free(iso->entries[i].path);
    free(iso->entries);
    if (iso->fd >= 0) close(iso->fd);
    free(iso);
}
