#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <asm/ioctl.h>
#include <string.h>
#include <strings.h>
#include <libgen.h>
#include <errno.h>
#include <grp.h>
#include <fcntl.h>
#include <assert.h>
#define __USE_GNU
#include <dlfcn.h>

#define __USE_LARGEFILE64
#include <sys/stat.h>
#include <linux/videodev.h>
#include <dirent.h>

#define VIDEOGROUP "video"
#define VIDEODEV "/dev/video"
static int fake_fd;

static int is_videodev(const char *name)
{
    char dir[PATH_MAX];
    char resolved[PATH_MAX];
    char *dname;
    char *d;
    const char *bname;

    strcpy(dir, name);
    bname = basename(dir);

    dname = dirname(dir);
    if(realpath(dname, resolved) == NULL) return 0;
    d = resolved + strlen(resolved);
    *d = '/';
    ++d;
    strcpy(d, bname);

    return (strcmp(VIDEODEV, resolved) == 0);
}

#define MKSTAT(name, stat) \
int name(int ver, const char *path, struct stat *buf) \
{ \
    int rv; \
    static int (*orig)(int, const char *, struct stat *) = NULL; \
    static gid_t vidgid; \
    struct group *grp; \
 \
    if(orig == NULL) { \
	orig = dlsym(RTLD_NEXT, #name); \
	if(orig == NULL) return -1; \
	grp = getgrnam(VIDEOGROUP); \
	if(grp == NULL) { \
	    return -1; \
	} \
	vidgid = grp->gr_gid; \
    } \
    rv = orig(ver, path, buf); \
    if(rv != 0 && is_videodev(path)) { \
	memset(buf, 0, sizeof *buf); \
	buf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP; \
	buf->st_rdev = makedev(81, 10); \
	buf->st_gid = vidgid; \
    } \
 \
    return 0; \
}

MKSTAT(__xstat, stat)
MKSTAT(__xstat64, stat64)
MKSTAT(__lxstat, stat)
MKSTAT(__lxstat64, stat64)

#define MKFSTAT(name, stat) \
int name(int ver, int fd, struct stat *buf) \
{ \
    static int (*orig)(int, int fd, struct stat *buf) = NULL; \
    \
    if(orig == NULL) { \
	orig = dlsym(RTLD_NEXT, #name); \
	if(orig == NULL) return -1; \
    } \
    if(fd != fake_fd) { \
        return orig(ver, fd, buf); \
    } \
    memset(buf, 0, sizeof *buf); \
    buf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP; \
    buf->st_rdev = makedev(81, 10); \
 \
    return 0; \
}

MKFSTAT(__fxstat, stat)
MKFSTAT(__fxstat64, stat64)

#define MKFCNTL(name) \
int name(int fd, int cmd, ...) \
{ \
    va_list argp; \
    static int (*orig)(int, int, ...) = NULL; \
    int rv; \
 \
    if(orig == NULL) { \
	orig = dlsym(RTLD_NEXT, #name); \
	if(orig == NULL) return -1; \
    } \
    if(fd != fake_fd) { \
	va_start(argp, cmd); \
	rv = orig(fd, cmd, argp); \
	va_end(argp); \
    } else { \
	rv = 0; \
    } \
 \
    return rv; \
} 

MKFCNTL(fcntl);
MKFCNTL(fcntl64);
MKFCNTL(__fcntl);
 
#define MKOPEN(name) \
int name(const char *path, int flags, ...) \
{ \
    static int (*orig)(const char *, int flags, ...) = NULL; \
    va_list argp; \
    int rv; \
     \
    if(orig == NULL) { \
	orig = dlsym(RTLD_NEXT, #name); \
	if(orig == NULL) return -1; \
    } \
    if(is_videodev(path)) { \
	rv = orig("/dev/null", O_RDONLY); \
	fake_fd = rv; \
    } else { \
	va_start(argp, flags); \
	rv = orig(path, flags, argp); \
	va_end(argp); \
    } \
 \
    return rv; \
}

MKOPEN(open)
MKOPEN(open64)
MKOPEN(__open)
MKOPEN(__open64)

int select(
        int nfds,
        fd_set *rd,
        fd_set *wr,
        fd_set *exc,
        struct timeval *tv
    )
{
    static int (*orig)(
        int ,
        fd_set *,
        fd_set *,
        fd_set *,
        struct timeval *) = NULL;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "select");
	if(orig == NULL) return -1;
    }
    if(fake_fd == -1 || nfds < fake_fd || !FD_ISSET(fake_fd, rd)) {
        return orig(nfds, rd, wr, exc, tv);
    }

    return 1;
}

ssize_t read(int fd, void *buf, size_t count)
{
    static ssize_t (*orig)(int, void *, size_t) = NULL;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "read");
	if(orig == NULL) return -1;
    }
    if(fd != fake_fd) {
	return orig(fd, buf, count);
    }
    strncpy(buf, "Test", count > 5 ? 5 : count);

    return 5;
}

int ioctl(int fd, int request, ...)
{
    va_list argp;

    static int (*orig)(int, int, ...) = NULL;
    int rv;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "ioctl");
	if(orig == NULL) return -1;
    }
    if(fd != fake_fd) {
	va_start(argp, request);
	rv = orig(fd, request, argp);
	va_end(argp);
	return rv;
    }

    return 0;
}

int close(int fd)
{
    static int (*orig)(int) = NULL;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "close");
	if(orig == NULL) return -1;
    }
    if(fd == fake_fd) {
	fake_fd = -1;
    }

    return close(fd);
}

typedef struct {
    DIR *dx_dir;
    enum { DxOther = 1, DxDev, DxDevFnd, DxDevEnd } dx_fnd;
    union {
	struct dirent dirent;
	struct dirent64 dirent64;
    } de;
} dx_t;

DIR *opendir(const char *path)
{
    static DIR *(*orig)(const char *) = NULL;
    char resolved[PATH_MAX];
    DIR *dir;
    dx_t *d;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "opendir");
    }
    dir = orig(path);
    if(dir == NULL) return dir;

    if(realpath(path, resolved) == NULL) return NULL;
    d = malloc(sizeof *d);
    if(d == NULL) return NULL;
    d->dx_dir = dir;
    if((strcmp("/dev", resolved) == 0)
    || (strcmp("/dev/", resolved) == 0)) {
	d->dx_fnd = DxDev;
    } else {
	d->dx_fnd = DxOther;
    }

    return (DIR *)d;
}

#define MKREADDIR(name, dirent)  \
struct dirent *name(DIR *dir)  \
{  \
    static struct dirent * (*orig)(DIR *) = NULL;  \
    struct dirent *de;  \
    dx_t *d;  \
  \
    d = (dx_t *)dir;  \
    if(d->dx_fnd == DxDevEnd) return NULL;  \
  \
    if(orig == NULL) {  \
	orig = dlsym(RTLD_NEXT, #name);  \
	if(orig == NULL) return NULL;  \
    }  \
  \
    de = orig(d->dx_dir);  \
    switch(d->dx_fnd) {  \
	case DxDev:  \
	    if(de != NULL) {  \
		if(strcmp("video", de->d_name) == 0) {  \
		    d->dx_fnd = DxDevFnd;  \
		}  \
	    } else {  \
		memset(&d->de, 0, sizeof(struct dirent64));  \
		d->de.dirent.d_type = DT_CHR;  \
		strcpy(d->de.dirent.d_name, "video");  \
		de = &d->de.dirent;  \
		d->dx_fnd = DxDevEnd;  \
	    }  \
	    return de;  \
	case DxDevFnd:  \
	case DxOther:  \
	default:  \
	    return de;  \
    }  \
} 

MKREADDIR(readdir, dirent)
MKREADDIR(readdir64, dirent64)
 
int closedir(DIR *dir)
{
    static int (*orig)(DIR *) = NULL;
    dx_t *d;
    int rv;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "closedir");
	if(orig == NULL) return -1;
    }
    d = (dx_t *)dir;
    rv = orig(d->dx_dir);
    free(d);

    return rv;
}
