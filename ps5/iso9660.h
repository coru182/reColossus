/* reColossus: reads a PS2 DVD image (.iso, 2048-byte sectors, ISO 9660) on the console: its files listed, a few read
 * into memory to check them, and the whole disc copied into a folder at the first start. */
#ifndef RECOLOSSUS_ISO9660_H
#define RECOLOSSUS_ISO9660_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RC_ISO_OK,
    RC_ISO_NOT_A_DISC, /* no ISO 9660 volume in it */
    RC_ISO_DAMAGED,    /* a directory or a file lies past the end of the image */
    RC_ISO_WRITE,      /* the folder could not be written: most often, no room */
    RC_ISO_NO_FILE     /* rc_iso_read_file: no such file */
};

typedef struct RcIso RcIso;

/* Opens the image and lists its files. Null on failure, with the reason in *error. */
RcIso *rc_iso_open(const char *path, int *error);
/* The bytes of every file on the disc. */
uint64_t rc_iso_total(const RcIso *iso);
/* A file at the top of the disc (name compared without case), read whole: *data is malloc'd. */
int rc_iso_read_file(RcIso *iso, const char *name, uint8_t **data, size_t *size);
/* Copies every file into target (created), with the disc's folders. progress gets 0 once (total bytes), then each new
 * percentage. */
int rc_iso_extract(RcIso *iso, const char *target, void (*progress)(unsigned percent, uint64_t total));
void rc_iso_close(RcIso *iso);

#ifdef __cplusplus
}
#endif
#endif
