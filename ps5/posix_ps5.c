/* reColossus (from reKing's posix_ps5.c): the C library functions the payload SDK's libc.a implements as raw syscalls.
 * A PS5 title may not issue a syscall itself: the console ends it with SYSTEM_ILLEGAL_FUNCTION_CALL (0xA002030A).
 * Defined here, they keep libc.a's versions out of the link (build-ps5.sh checks the linked program has none left).
 * mmap and mprotect go through libkernel as reUndr's ps5/src/memory_ps5.c does (MIT, bystritskiy). libc.a keeps
 * readlink, pathconf and fork in an object the link needs for other names, so those three are rc_* here and the link
 * binds the names to them with --defsym (build-ps5.sh), as the RADV recipe does for malloc.
 *
 * Also the threads' stacks: the runtime starts its threads with std::thread, which takes the system's default stack.
 * The game's thread runs the recompiled code, nested deeply; the link sends every pthread_create through
 * __wrap_pthread_create (--wrap=pthread_create), which gives a thread started without a stack size at least
 * RC_THREAD_STACK. */
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

extern int sceKernelMmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset, void **result);
extern int sceKernelMprotect(const void *addr, size_t len, int prot);

/* libkernel's errors are 0x8002xxxx with the errno in the low bits */
static int to_errno(int e) { return e & 0xffff; }

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    void *mapped = NULL;
    int e = sceKernelMmap(addr, len, prot, flags, fd, offset, &mapped);
    if (e) {
        errno = to_errno(e);
        return MAP_FAILED;
    }
    return mapped;
}

int mprotect(const void *addr, size_t len, int prot) {
    int e = sceKernelMprotect(addr, len, prot);
    if (e) {
        errno = to_errno(e);
        return -1;
    }
    return 0;
}

/* no symbolic links in anything this app reads */
ssize_t rc_readlink(const char *restrict path, char *restrict buf, size_t size) {
    (void)path;
    (void)buf;
    (void)size;
    errno = EINVAL;
    return -1;
}

/* the limits of the console's file systems */
long rc_pathconf(const char *path, int name) {
    (void)path;
    switch (name) {
    case _PC_NAME_MAX: return 255;
    case _PC_PATH_MAX: return 1024;
    default: errno = EINVAL; return -1;
    }
}

/* a PS5 app is one process */
pid_t rc_fork(void) {
    errno = ENOSYS;
    return -1;
}

#define RC_THREAD_STACK (8u * 1024u * 1024u)

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg) {
    pthread_attr_t own;
    size_t size = 0;
    if (attr && pthread_attr_getstacksize(attr, &size) == 0 && size >= RC_THREAD_STACK)
        return __real_pthread_create(thread, attr, start, arg);
    if (attr) {
        /* a copy of the caller's attributes with the larger stack: only the detach state is ever set here */
        int detach = PTHREAD_CREATE_JOINABLE;
        pthread_attr_getdetachstate(attr, &detach);
        pthread_attr_init(&own);
        pthread_attr_setdetachstate(&own, detach);
    } else {
        pthread_attr_init(&own);
    }
    pthread_attr_setstacksize(&own, RC_THREAD_STACK);
    int rc = __real_pthread_create(thread, &own, start, arg);
    pthread_attr_destroy(&own);
    return rc;
}

/* realpath: the PS5's libc module doesn't answer the import (the call jumps to an unmapped address: the 0.1.0 and
 * 0.1.1 crash, from std::filesystem::relative -> weakly_canonical -> canonical in the runtime's disc index). Here
 * without symbolic links (none on the console's data paths): the path made absolute and its . and .. resolved, and
 * then it must exist, as POSIX's realpath requires (weakly_canonical relies on ENOENT for the part that doesn't). */
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char *realpath(const char *restrict path, char *restrict resolved) {
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    if (!*path) {
        errno = ENOENT;
        return NULL;
    }
    char joined[PATH_MAX * 2];
    if (path[0] == '/') {
        if (strlen(path) >= sizeof joined) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        strcpy(joined, path);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) return NULL;
        if (strlen(cwd) + 1 + strlen(path) >= sizeof joined) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        strcpy(joined, cwd);
        strcat(joined, "/");
        strcat(joined, path);
    }
    char out[PATH_MAX];
    size_t len = 0;
    out[0] = '/';
    len = 1;
    for (const char *p = joined; *p;) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != '/') end++;
        size_t n = (size_t)(end - p);
        if (n == 1 && p[0] == '.') {
            /* nothing */
        } else if (n == 2 && p[0] == '.' && p[1] == '.') {
            if (len > 1) {
                len--;                                  /* the trailing component */
                while (len > 1 && out[len - 1] != '/') len--;
                if (len > 1) len--;                     /* its slash, unless it is the root's */
            }
        } else {
            if (len + (len > 1) + n >= sizeof out) {
                errno = ENAMETOOLONG;
                return NULL;
            }
            if (len > 1) out[len++] = '/';
            memcpy(out + len, p, n);
            len += n;
        }
        p = end;
    }
    out[len] = 0;
    struct stat st;
    if (stat(out, &st) != 0) return NULL; /* errno from stat: ENOENT for a missing part */
    if (!resolved) {
        resolved = malloc(len + 1);
        if (!resolved) {
            errno = ENOMEM;
            return NULL;
        }
    }
    memcpy(resolved, out, len + 1);
    return resolved;
}
