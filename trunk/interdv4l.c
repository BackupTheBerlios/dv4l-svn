/* Copyright (C) 2008 Free Software Foundation, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Wolfgang Beck <bewo at users.berlios.de> 2008
 */

#include <sys/types.h>
#include <sys/select.h>
#include <sys/mman.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <libraw1394/raw1394.h>
#include <libiec61883/iec61883.h>
#include <libdv/dv.h>
#include <asm/ioctl.h>
#ifdef HAVE_XATTR
#include <sys/xattr.h>
#endif
#include <string.h>
#include <strings.h>
#include <time.h>
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
#include "config.h"
#include "version.h"
#include "normfile.h"
#include "scale.h"
#include "palettes.h"
#include "util.h"

#define VIDEOGROUP "video"
#define VIDEODEV "/dev/video0"
#define VIDEOV4L "/dev/v4l"
#define VIDEOV4LDEV VIDEOV4L "/video0"
static int fake_fd = -1;


typedef enum { DvIdle = 1, DvRead, DvMmap } dv4l_run_t;

typedef struct {
    int vfd;
    char vdevname[PATH_MAX];
    char vdevalt[PATH_MAX];
    char vdevbase[PATH_MAX];
    unsigned char *vrbuf;
    unsigned char *vpmmap;
    int vimgsz;
    raw1394handle_t vraw;
    iec61883_dv_fb_t viec;
    dv_decoder_t *vdvdec;
    struct video_capability vcap;
    struct video_channel vchan;
    struct video_picture vpic;
    struct video_window vwin;
    uint8_t *vframe[3];
    int vpitches[3];
    int vcomplete;
    struct timeval vlastcomplete;
    dv4l_run_t vstate;
    int vnoredir;
    int vminor;
    int vrgbonly;
    time_t vtime;
} vid_context_t;

static vid_context_t vctx = { 0 };

static int is_videodev(const char *name);

#define MKSTAT_COMMON(name, stat) \
int common_##name(gid_t vidgid, const char *path, struct stat *buf) \
{ \
    char resolved[PATH_MAX]; \
    int rv; \
 \
debug("#1" #name " <%s>\n", path); \
    rv = -1; \
    if(vctx.vnoredir == 0) { \
	if(is_videodev(path)) { \
debug(#name " is videodev <%s>\n", path); \
	    memset(buf, 0, sizeof *buf); \
	    buf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP; \
	    buf->st_rdev = makedev(81, vctx.vminor); \
	    buf->st_gid = vidgid; \
	    buf->st_blksize = 4096; \
	    buf->st_nlink = 1; \
	    buf->st_atime = time(NULL); \
	    buf->st_mtime = vctx.vtime; \
	    buf->st_ctime = vctx.vtime; \
	    rv = 0; \
	} else { \
	    normalize(path, resolved); \
debug("#2" #name " <%s>\n", resolved); \
	    if(strcmp(VIDEOV4L, resolved) == 0) { \
debug("#3" #name " <%s>\n", resolved); \
		memset(buf, 0, sizeof *buf); \
		buf->st_mode = S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR \
				| S_IRGRP | S_IWGRP | S_IXGRP; \
		buf->st_gid = vidgid; \
		rv = 0; \
	    } \
	} \
    } \
 \
    return rv; \
}

MKSTAT_COMMON(lstat, stat)
MKSTAT_COMMON(lstat64, stat64)
MKSTAT_COMMON(__xstat, stat)
MKSTAT_COMMON(__xstat64, stat64)
MKSTAT_COMMON(__lxstat, stat)
MKSTAT_COMMON(__lxstat64, stat64)

#define MKXSTAT(name, stat) \
static int (*orig##name)(int, const char *, struct stat *) = NULL; \
int name(int ver, const char *path, struct stat *buf) \
{ \
    int rv; \
    static gid_t vidgid; \
    struct group *grp; \
 \
    if(orig##name == NULL) { \
	orig##name = dlsym(RTLD_NEXT, #name); \
	if(orig##name == NULL) return -1; \
	grp = getgrnam(VIDEOGROUP); \
	if(grp == NULL) { \
	    return -1; \
	} \
	vidgid = grp->gr_gid; \
    } \
    rv = orig##name(ver, path, buf); \
    if(rv == -1) { \
	rv = common_##name(vidgid, path, buf); \
    } \
debug(#name " path <%s> noredir %d rv %d\n", path, vctx.vnoredir, rv); \
 \
    return rv; \
}

MKXSTAT(__xstat, stat)
MKXSTAT(__xstat64, stat64)
MKXSTAT(__lxstat, stat)
MKXSTAT(__lxstat64, stat64)

#define MKSTAT(name, stat) \
static int (*orig##name)(const char *, struct stat *) = NULL; \
int name(const char *path, struct stat *buf) \
{ \
    int rv; \
    static gid_t vidgid; \
    struct group *grp; \
 \
    if(orig##name == NULL) { \
	orig##name = dlsym(RTLD_NEXT, #name); \
	if(orig##name == NULL) return -1; \
	grp = getgrnam(VIDEOGROUP); \
	if(grp == NULL) { \
	    return -1; \
	} \
	vidgid = grp->gr_gid; \
    } \
    rv = orig##name(path, buf); \
    if(rv == -1) { \
	rv = common_##name(vidgid, path, buf); \
    } \
debug(#name " path <%s> noredir %d rv %d\n", path, vctx.vnoredir, rv); \
 \
    return rv; \
}

MKSTAT(lstat, stat)
MKSTAT(lstat64, stat64)

int access(const char *path, int mode)
{
    int (*orig)(const char *, int) = NULL;
    int rv;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "access");
	if(orig == NULL) return -1;
    }
    rv = orig(path, mode);
    if(rv == -1) {
	if(is_videodev(path)) {
log("access <%s>\n", path);
	    rv = 0;
	}
    }

    return rv;
}

#define LOOK4FREEDEV
static int scan_dev(char *devname, const char *devtmpl)
{
#ifdef LOOK4FREEDEV
    struct stat statb;
    int i;

    /*
     * This code looks for a free video device number
     *
     * As there is no real benefit for the user in doing so,
     * this code is currently not used.
     *
     * To access a real V4L device under /dev/video0, an
     * application can just be started without dv4lstart.
     *
     * As there might be a compelling use case I don't know of,
     * this code stays for the time being.
     */
    for(i = 0; i < 10; ++i) {
	sprintf(devname, devtmpl, i);
	if(stat(devname, &statb) < 0) {
	    debug("stat <%s> ok\n", devname);
	    return i;
	}
    }
#else
    sprintf(devname, devtmpl, 0);

    return 0;
#endif

    return -1;
}

static void init_vctx()
{
    int m;

    vctx.vnoredir = 1;
    /*
     * see scan_dev comment for rationale behin this code
     */
    if(getenv("DV4L_NEWDEV") != NULL) {
	m = scan_dev(vctx.vdevname, "/dev/video%d");
    } else {
	strcpy(vctx.vdevname, VIDEODEV);
	m = 0;
    }
    if(m < 0) {
	m = scan_dev(vctx.vdevname, VIDEOV4L "/video%d");
	if(m < 0) {
	    strcpy(vctx.vdevname, VIDEODEV);
	    m = 0;
	} else {
	    sprintf(vctx.vdevalt, "/dev/video%d", m);
	}
    } else {
	sprintf(vctx.vdevalt, VIDEOV4L "/video%d", m);
    }
    vctx.vminor = m;
    vctx.vtime = time(NULL);
    sprintf(vctx.vdevbase, "video%d", m);
    vctx.vnoredir = 0;
}

/*
 * check if a name matches our simulated device
 */
static int is_videodev(const char *name)
{
    char dir[PATH_MAX];
    char resolved[PATH_MAX];
    char *dname;
    char *d;
    const char *bname;

    if(vctx.vdevname[0] == '\0') {
	init_vctx();
    }
    strcpy(dir, name);
    bname = basename(dir);

    dname = dirname(dir);
    if(normalize(dname, resolved) == NULL) return 0;
    d = resolved + strlen(resolved);
    *d = '/';
    ++d;
    strcpy(d, bname);

    debug("is_videovdev devname <%s> devalt <%s> resolved <%s>\n", 
	    vctx.vdevname, vctx.vdevalt, resolved);

    return (strcmp(vctx.vdevname, resolved) == 0)
    || (strcmp(vctx.vdevalt, resolved) == 0);
}

static inline unsigned long  timediff(
    const struct timeval *a,
    const struct timeval *b
)
{
    return ((a->tv_sec * 1000) + (a->tv_usec / 1000))
	 - ((b->tv_sec * 1000) + (b->tv_usec / 1000));
}

extern char **environ;

#undef HIDE_PRELOAD
#define PRELOAD "LD_PRELOAD="
static void strip_environ()
{
#ifdef HIDE_PRELOAD
    char **cp;
    char **dp;

    for(cp = environ, dp = NULL; *cp != NULL; ++cp) {
	if(strncmp(*cp, PRELOAD, (sizeof PRELOAD) - 1) == 0) {
	    dp = cp;
	}
    }
    if(dp != NULL) {
	memcpy(dp, dp + 1, (cp - dp) * sizeof cp);
    }
#endif
}

#define XSTR(s) STR(s)
#define STR(s) #s

static char **addlib(char *const envp[])
{
    char * const *cp;
    int sz;
    char **envp2;

    for(cp = (char *const *)envp; *cp != NULL; ++cp) ;
    sz = cp - envp;
    envp2 = malloc((sz + 1) * sizeof *cp);
    if(envp2 == NULL) return NULL;
    memcpy(envp2, envp, sz * sizeof *cp);
    envp2[sz] = "LD_PRELOAD=" XSTR(DV4LLIBNAME);
    envp2[sz + 1] = NULL;
    #if 0
    int i;
    for(i = 0; envp2[i] != NULL; ++i) debug("*cp <%s>\n", envp2[i]);
    #endif

    return envp2;
}

char *getenv(const char *name)
{
    static char *(*orig)(const char *name) = NULL;
    const char *dv4l_env;
    char *err;
    int lvl;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "getenv");
	if(orig == NULL) return NULL;
	dv4l_env = getenv("DV4L_VERBOSE");
	if(dv4l_env != NULL) {
	    lvl = strtol(dv4l_env, &err, 0);
	    if(*dv4l_env != '\0' && *err == '\0') {
		set_tracelevel(lvl);
		log("set tracelevel to %d\n", lvl);
	    }
	}
	dv4l_env = getenv("DV4L_COLORCORR");
	set_color_correction(dv4l_env != NULL);

	dv4l_env = getenv("DV4L_RGBONLY");
	vctx.vrgbonly = (dv4l_env != NULL);
    }

    if(strcmp(name, "LD_PRELOAD") == 0) {
	return NULL;
    } else {
	return orig(name);
    }
}

int execve(const char *fname, char *const argv[], char *const envp[])
{
    static int (*orig)
	(const char *fname, char *const argv[], char *const envp[]) = NULL;
    char **envp2;
    int rv;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "execve");
	if(orig == NULL) return -1;
    };
debug("execve <%s>\n", fname);
    envp2 = addlib(envp);
    rv = orig(fname, argv, envp2);
    free(envp2);

    return rv;
}

int fexecve(int fd, char *const argv[], char *const envp[])
{
    static int (*orig)
	(int, char *const argv[], char *const envp[]) = NULL;
    char **envp2;
    int rv;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "execve");
	if(orig == NULL) return -1;
    };
debug("fexecve\n");
    envp2 = addlib(envp);
    rv = orig(fd, argv, envp2);
    free(envp2);

    return rv;
}

#ifdef HIDE_PRELOAD
pid_t fork()
{
    static pid_t (*orig)() = NULL;
    pid_t rv;
    char **env0;
    char **env1;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "fork");
	if(orig == NULL) return -1;
    }
    env0 = environ;
    env1 = addlib(env0);
    environ = env1;

    rv = orig();
    environ = env0;

    return rv;
}
#endif

#define MKFSTAT(name, stat) \
static int (*orig##name)(int, int fd, struct stat *buf) = NULL; \
int name(int ver, int fd, struct stat *buf) \
{ \
    \
    if(orig##name == NULL) { \
	orig##name = dlsym(RTLD_NEXT, #name); \
	if(orig##name == NULL) return -1; \
    } \
    if(fd != fake_fd) { \
        return orig##name(ver, fd, buf); \
    } \
    memset(buf, 0, sizeof *buf); \
    buf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP; \
    buf->st_rdev = makedev(81, 10); \
debug(#name " videodev\n"); \
 \
    return 0; \
}

MKFSTAT(__fxstat, stat)
MKFSTAT(__fxstat64, stat64)

#define MKFCNTL(name) \
static int (*orig##name)(int, int, ...) = NULL; \
int name(int fd, int cmd, ...) \
{ \
    va_list argp; \
    char *p; \
    int rv; \
 \
    if(orig##name == NULL) { \
	orig##name = dlsym(RTLD_NEXT, #name); \
	if(orig##name == NULL) return -1; \
    } \
    if(fd != fake_fd) { \
	va_start(argp, cmd); \
	p = va_arg(argp, char *); \
	rv = orig##name(fd, cmd, p); \
	va_end(argp); \
    } else { \
debug(#name " %d videodev\n", cmd); \
	rv = 0; \
    } \
 \
    return rv; \
} 

MKFCNTL(fcntl);
MKFCNTL(fcntl64);
MKFCNTL(__fcntl);
MKFCNTL(__fcntl64);
 
#define MAXBUF 2
#define MKMMAP(name) \
void *name(void *start, size_t len, int prot, int flags, int fd, off_t off) \
{ \
    static void *(*orig)(void *, size_t, int, int, int, off_t) = NULL; \
    void *rv; \
    \
    if(orig == NULL) { \
	orig = dlsym(RTLD_NEXT, #name); \
	if(orig == NULL) return NULL; \
    } \
    if((fake_fd != fd) || (fake_fd == -1) || (flags & MAP_ANONYMOUS)) { \
	rv = orig(start, len, prot, flags, fd, off); \
    } else if(fake_fd != -1) { \
	vctx.vpmmap = malloc(MAXBUF * vctx.vimgsz); \
	if(vctx.vpmmap == NULL) return MAP_FAILED; \
	rv = vctx.vpmmap; \
    } else { \
	err("invalid fd in " #name "\n"); \
	rv = MAP_FAILED; \
    } \
    log(#name " fd %d rv 0x%lx\n", fd, rv); \
 \
    return rv; \
}

MKMMAP(mmap);
MKMMAP(mmap2);
MKMMAP(mmap64);
MKMMAP(__mmap64);

int munmap(void *start, size_t length)
{
    static int (*orig)(void *, size_t) = NULL;
    int rv;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "munmap");
	if(orig == NULL) return -1;
    }
debug("munmap 0x%lx\n", start);
    if(vctx.vpmmap != start) {
	rv = orig(start, length);
    } else {
	if(vctx.vstate == DvIdle) {
	    if(vctx.vpmmap != NULL) {
		free(vctx.vpmmap);
		vctx.vpmmap = NULL;
	    }
	}
	rv = 0;
    }
debug("#3munmap rv %d\n", rv);

    return rv;
}

static inline int frame_process(
	vid_context_t *vc,
        const unsigned char *data
    )
{
    static unsigned char *tmp = NULL;
    unsigned char *scal_dst;
    unsigned char *pal_dst;
    unsigned char *dst;
    int w;
    int h;
    int rv;

    w = vc->vcap.maxwidth;
    h = vc->vcap.maxheight;
    if(tmp == NULL) {
        tmp = malloc(w * h * 3);
    }
    dst = vc->vrbuf;
    if(dst != NULL) {
	dv_decode_full_frame(vc->vdvdec, data,
			e_dv_color_rgb,
			vc->vframe,
			vc->vpitches);
	switch(vc->vpic.palette) {
	    case VIDEO_PALETTE_YUV420P: scal_dst = tmp; pal_dst = dst; break;
	    case VIDEO_PALETTE_RGB24:
	    default:
		scal_dst = dst;
		pal_dst = dst;
		break;
	}
	scale(vc->vframe[0], scal_dst, w, h, vc->vwin.width, vc->vwin.height);
	rv = palette_conv(scal_dst, pal_dst,
			vc->vpic.palette, vc->vwin.width,
			vc->vwin.height);
    } else {
	rv = 0;
    }

    return rv;
}

static int frame_recv(
	unsigned char *data,
	int len,
	int complete,
	void *arg
    )
{
    vid_context_t *vc;
    int w;
    int h;

    vc = (vid_context_t *)arg;
    if(complete) {
	dv_parse_header(vc->vdvdec, data);
	if(vc->vpitches[0] == 0) {
	    if(vc->vframe[0] != NULL) free(vc->vframe[0]);
	    w = vc->vdvdec->width;
	    h = vc->vdvdec->height;
	    vc->vframe[0] = malloc(w * h * 3);
	    vc->vpitches[0] = w * 3;
	    vc->vcap.maxwidth = w;
	    vc->vcap.maxheight = h;
	}
	frame_process(vc, data);
	vc->vcomplete = 1;
    } else {
	vc->vcomplete = 0;
    }

    return 0;
}

static void get_camsize(vid_context_t *vc)
{
    fd_set rfds;
    int rv;

    if(iec61883_dv_fb_start(vc->viec, 63) < 0) {
	return ;
    }
    FD_ZERO(&rfds);
debug("get_camsize\n");
    while(vc->vcap.maxwidth == 0) {
	FD_SET(vc->vfd, &rfds);
	rv = select(vc->vfd + 1, &rfds, NULL, NULL, NULL);
	if(rv > 0) {
	    if(FD_ISSET(vc->vfd, &rfds)) {
		raw1394_loop_iterate(vc->vraw);
	    }
	}
    }
    iec61883_dv_fb_stop(vc->viec);
    vc->vimgsz = vc->vcap.maxwidth * vc->vcap.maxheight * 3;
debug("vimgsz %d\n", vc->vimgsz);
}

#define MKOPEN(name) \
static int (*orig##name)(const char *, int flags, ...) = NULL; \
int name(const char *path, int flags, ...) \
{ \
    va_list argp; \
    static int libdv_inited = 0; \
    mode_t mode; \
    int rv; \
     \
    if(orig##name == NULL) { \
	orig##name = dlsym(RTLD_NEXT, #name); \
	if(orig##name == NULL) return -1; \
	strip_environ(); \
    } \
log(#name " <%s>\n", path); \
    if(is_videodev(path)) { \
	if(fake_fd != -1) { \
debug(#name " videodev fake_fd already init'd\n"); \
	    return fake_fd; \
	} \
debug("#1 dv4l open\n"); \
	rv = orig##name("/dev/null", O_RDONLY); \
	fake_fd = rv; \
	if(!libdv_inited) { \
	    vctx.vraw = raw1394_new_handle_on_port(0); \
	    if(vctx.vraw == NULL) { \
		return -1; \
	    } \
debug("#1 dv4l open libdv_init\n"); \
	    vctx.viec = iec61883_dv_fb_init(vctx.vraw, frame_recv, &vctx); \
	    if(vctx.viec == NULL) return -1; \
	    dv_init(0, 0); \
	    vctx.vdvdec = dv_decoder_new(FALSE, FALSE, FALSE); \
	    if(vctx.vdvdec == NULL) { \
		return -1; \
	    } \
	    dv_set_quality(vctx.vdvdec, DV_QUALITY_BEST); \
	    vctx.vframe[0] = NULL; \
	    vctx.vframe[1] = NULL; \
	    vctx.vframe[2] = NULL; \
	    vctx.vpitches[1] = 0; \
	    vctx.vpitches[2] = 0; \
	    vctx.vpic.palette = VIDEO_PALETTE_RGB24; \
	    vctx.vpic.depth = get_depth(vctx.vpic.palette); \
	    libdv_inited = 1; \
	} \
	vctx.vstate = DvRead; \
	vctx.vpitches[0] = 0; \
	vctx.vfd = raw1394_get_fd(vctx.vraw); \
	vctx.vrbuf = NULL; \
	vctx.vpmmap = NULL; \
	gettimeofday(&vctx.vlastcomplete, NULL); \
	vctx.vcap.maxwidth = 0; \
	vctx.vcap.maxheight = 0; \
log("#2 dv4l open vfd %d fake_fd %d\n", vctx.vfd, fake_fd); \
	get_camsize(&vctx); \
	vctx.vwin.width = vctx.vcap.maxwidth; \
	vctx.vwin.height = vctx.vcap.maxheight; \
debug("#3 dv4l open\n"); \
	iec61883_dv_set_buffers(iec61883_dv_fb_get_dv(vctx.viec), 1000); \
	if(iec61883_dv_fb_start(vctx.viec, 63) < 0) { \
debug("#4 dv4l open\n"); \
	    return -1; \
	} \
debug("#5 dv4l open\n"); \
    } else { \
	if(flags & O_CREAT) { \
	    va_start(argp, flags); \
	    mode = va_arg(argp, mode_t); \
	    rv = orig##name(path, flags, mode); \
	    va_end(argp); \
	} else { \
	    rv = orig##name(path, flags); \
	} \
debug("#5 dv4l open rv %d err <%s>\n", rv, strerror(errno)); \
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
    int rv;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "select");
	if(orig == NULL) return -1;
    }
    if(fake_fd == -1 || nfds < fake_fd || !FD_ISSET(fake_fd, rd)) {
	return orig(nfds, rd, wr, exc, tv);
    }
    if(FD_ISSET(fake_fd, rd)) {
	FD_CLR(fake_fd, rd);
	FD_SET(vctx.vfd, rd);
	if(vctx.vfd >= nfds) {
	    nfds = vctx.vfd + 1;
	}
	rv = orig(nfds, rd, wr, exc, tv);
	if(FD_ISSET(vctx.vfd, rd)) {
	    FD_SET(fake_fd, rd);
	    FD_CLR(vctx.vfd, rd);
	}
    } else {
	rv = orig(nfds, rd, wr, exc, tv);
    }

    return rv;
}

ssize_t read(int fd, void *buf, size_t count)
{
    static ssize_t (*orig)(int, void *, size_t) = NULL;
    fd_set rfds;
    int cnt;
    struct timeval readstart;
    struct timeval selstart;
    unsigned long td;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "read");
	if(orig == NULL) return -1;
    }
    if(fd != fake_fd) {
	cnt = orig(fd, buf, count);
	return cnt;
    }
    /*
     * get time that has elapsed since last complete frame
     */
    gettimeofday(&readstart, NULL);
    td = timediff(&readstart, &vctx.vlastcomplete);

    FD_ZERO(&rfds);
    while(1) {
	FD_SET(vctx.vfd, &rfds);
	vctx.vrbuf = buf;
	gettimeofday(&selstart, NULL);
	if(select(vctx.vfd + 1, &rfds, NULL, NULL, NULL) > 0) {
	    raw1394_loop_iterate(vctx.vraw);
	    if(vctx.vcomplete) {
		/*
		 * check if we had skipped enough frames to
		 * only get the latest frame; this avoids jittery
		 * display
		 */
		gettimeofday(&vctx.vlastcomplete, NULL);
		td += timediff(&vctx.vlastcomplete, &selstart);
		if(vctx.vstate == DvRead || td > 20) return count;
	    }
	}
    }
printf("td %ld\n",td);

    return count;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    static ssize_t (*orig)(int, const void *, size_t) = NULL;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "write");
	if(orig == NULL) return -1;
    }
    if(fd != fake_fd) {
	return orig(fd, buf, count);
    }
debug("dv4l write\n");

    return count;
}

int ioctl(int fd, int request, ...)
{
    va_list argp;
    static int (*orig)(int, int, ...) = NULL;
    struct video_capability *vcap;
    struct video_channel *vchan;
    struct video_picture *vpic;
    struct video_window *vwin;
    struct video_mbuf *vmbuf;
    struct video_mmap *vmmap;
    char *p;
    int rv;
    int frame;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "ioctl");
	if(orig == NULL) return -1;
    }
    if(fd != fake_fd) {
	va_start(argp, request);
	p = va_arg(argp, char *);
	rv = orig(fd, request, p);
	va_end(argp);
	return rv;
    }
debug("ioctl videodev fd %d req %d\n", fd, request);
    switch(request) {
	case VIDIOCGCAP:
	    va_start(argp, request);
	    vcap = va_arg(argp, struct video_capability *);
	    va_end(argp);
	    strncpy(vcap->name, "DV4Linux dv1394 to V4L", sizeof(vcap->name));
	    vcap->channels = 1;
	    vcap->type = VID_TYPE_CAPTURE;
            vcap->audios = 0;
log("report max w %d h %d\n", vctx.vcap.maxwidth, vctx.vcap.maxheight);
            vcap->maxwidth = vctx.vcap.maxwidth;
            vcap->maxheight = vctx.vcap.maxheight;
            vcap->minwidth = 176;
            vcap->minheight = 144;
	    return 0;
	case VIDIOCGCHAN:
            va_start(argp, request);
            vchan = va_arg(argp, struct video_channel *);
            va_end(argp);
            vchan->channel = 0;
            strncpy(vchan->name, "DVCam", sizeof vchan->name);
            vchan->tuners = 0;
            vchan->type = VIDEO_TYPE_CAMERA;
            vchan->flags = 0;
            vchan->norm = VIDEO_MODE_AUTO;
debug("#2dv4l ioctl\n");
            return 0;
        case VIDIOCSCHAN:
debug("#3dv4l ioctl\n");
	    return 0;
        case VIDIOCGPICT:
            va_start(argp, request);
            vpic = va_arg(argp, struct video_picture *);
            va_end(argp);
            vpic->brightness = 32768;
            vpic->hue = 32768;
            vpic->colour = 32768;
            vpic->contrast = 32768;
            vpic->whiteness = 32768;
            vpic->depth = get_depth(vctx.vpic.palette);
            vpic->palette = vctx.vpic.palette;
            return 0;
        case VIDIOCSPICT:
            va_start(argp, request);
            vpic = va_arg(argp, struct video_picture *);
            va_end(argp);
debug("#5dv4l ioctl\n");
	    if(vpic->palette == VIDEO_PALETTE_RGB24
	    || (vpic->palette == VIDEO_PALETTE_YUV420P
		&& vctx.vrgbonly == 0)) {
log("set palette %d\n", vpic->palette);
		vctx.vpic.palette = vpic->palette;
		return 0;
	    } else {
log("VIDIOCSPICT unsupported palette\n");
		return -1;
	    }
        case VIDIOCGWIN:
            va_start(argp, request);
            vwin = va_arg(argp, struct video_window *);
            va_end(argp);
            vwin->x = 0;
            vwin->y = 0;
            vwin->width = vctx.vwin.width;
            vwin->height = vctx.vwin.height;
            vwin->clips = NULL;
            vwin->clipcount = 0;
            return 0;
        case VIDIOCSWIN:
            va_start(argp, request);
            vwin = va_arg(argp, struct video_window *);
            va_end(argp);
debug("#6dv4l ioctl set to w %d h %d\n", vwin->width, vwin->height);
	    vctx.vwin.width = vwin->width;
	    vctx.vwin.height = vwin->height;
            return 0;
	case VIDIOCGMBUF:
debug("VIDIOCGMBUF\n");

	    va_start(argp, request);
	    vmbuf = va_arg(argp, struct video_mbuf *);
	    va_end(argp);
	    vmbuf->size = MAXBUF * vctx.vimgsz;
	    vmbuf->frames = MAXBUF;
	    vmbuf->offsets[0] = 0;
	    vmbuf->offsets[1] = vctx.vimgsz;
	    vctx.vstate = DvMmap;
	    return 0;
	case VIDIOCSYNC:
	    va_start(argp, request);
	    frame = *(va_arg(argp, int *));
	    va_end(argp);
	    read(fake_fd, vctx.vpmmap + vctx.vimgsz * frame, vctx.vimgsz);
	    vctx.vstate = DvMmap;
	    return 0;
	case VIDIOCMCAPTURE:
	    va_start(argp, request);
	    vmmap = va_arg(argp, struct video_mmap *);
	    va_end(argp);
	    if(vctx.vpmmap != NULL) {
		if(vmmap->format == VIDEO_PALETTE_RGB24
		|| (vmmap->format == VIDEO_PALETTE_YUV420P
		    && vctx.vrgbonly == 0)) {
		    vctx.vpic.palette = vmmap->format;
		    vctx.vwin.width = vmmap->width;
		    vctx.vwin.height = vmmap->height;
		    vctx.vstate = DvMmap;
		    rv = 0;
		} else {
log("unsupported/disabled palette %d\n", vmmap->format);
		    rv = -1;
		}
	    } else {
log("VIDIOCSYNC no mem mapped\n");
		rv = -1;
	    }
	    return rv;
	case VIDIOCGCAPTURE:
	case VIDIOCGFBUF:
	default:
log("unsupported ioctl %d\n", request);
	    errno = EINVAL;
	    return -1;
    }

    return 0;
}

int close(int fd)
{
    static int (*orig)(int) = NULL;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "close");
	if(orig == NULL) return -1;
	strip_environ();
    }
    if(fd == fake_fd) {
log("close fake_fd");
	iec61883_dv_fb_stop(vctx.viec);
	vctx.vstate = DvIdle;
	fake_fd = -1;
    }

    return orig(fd);
}

typedef struct dx_s {
    DIR *dx_dir;
    enum { DxOther = 1, DxDev, DxDevFnd, DxDevEnd, DxMkDir } dx_fnd;
    union {
	struct dirent dirent;
	struct dirent64 dirent64;
    } de;
    struct dx_s *next;
} dx_t;

#define HASHSZ 13
static dx_t *dxtab[HASHSZ];

static void dxtab_init()
{
    int i;

    for(i = 0; i < HASHSZ; ++i) dxtab[i] = NULL;
}

static dx_t **dxtab_find0(const DIR *dir)
{
    dx_t *p;
    dx_t **pp;

    for(pp = &dxtab[(unsigned long)dir % HASHSZ], p = *pp;
	p != 0 && p->dx_dir != dir;
	pp = &p->next, p = *pp) ;

    return pp;
}

static dx_t *dxtab_find(const DIR *dir)
{
    dx_t **pp;

    pp = dxtab_find0(dir);

    return *pp;
}

static void dxtab_add(dx_t *d)
{
    dx_t **pp;

    pp = dxtab_find0(d->dx_dir);
    d->next = *pp;
    *pp = d;
}

static void dxtab_rm(const DIR *dir)
{
    dx_t *p;
    dx_t **pp;

    pp = dxtab_find0(dir);
    p = *pp;
    if(p != NULL) {
	*pp = p->next;
	free(p);
    }
}

DIR *opendir(const char *path)
{
    static DIR *(*orig)(const char *) = NULL;
    char resolved[PATH_MAX];
    DIR *dir;
    dx_t *d;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "opendir");
	if(orig == NULL) return NULL;
	dxtab_init();
	if(vctx.vdevname[0] == '\0') {
	    init_vctx();
	}
    }
    dir = orig(path);
    normalize(path, resolved);
    if(dir == NULL) {
	if(strcmp("/dev/v4l", resolved) == 0) {
	    /*
	     * simulate non-existing /dev/v4l directory
	     */
	    d = malloc(sizeof *d);
	    if(d == NULL) return NULL;
	    memset(d, 0, sizeof *d);
	    d->dx_dir = orig("/");;
	    d->dx_fnd = DxMkDir;
	    dxtab_add(d);
	    return d->dx_dir;
	} else {
	    return dir;
	}
    }

    d = malloc(sizeof *d);
    if(d == NULL) return NULL;
    d->dx_dir = dir;
    log("opendir <%s>\n", resolved);
    if((strcmp("/dev", resolved) == 0)
    || (strcmp("/dev/v4l", resolved) == 0)) {
	d->dx_fnd = DxDev;
    } else {
	d->dx_fnd = DxOther;
    }
    dxtab_add(d);

    return d->dx_dir;
}

DIR *fdopendir(int fd)
{
    static DIR *(*orig)(int) = NULL;
    dx_t *d;
    DIR *dir;

debug("fdopendir");
    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "fdopendir");
	if(orig == NULL) return NULL;
    }
    dir = orig(fd);
    if(dir == NULL) return NULL;

    d = malloc(sizeof *d);
    if(d == NULL) return NULL;

    d->dx_dir = dir;
    d->dx_fnd = DxOther;
    dxtab_add(d);

    return d->dx_dir;
}

#define MKCOMMON_RDIR(name, dirent) \
struct dirent *common_##name(dx_t *d, struct dirent *de) \
{ \
    switch(d->dx_fnd) {  \
	case DxDev:  \
	    if(de == NULL) { \
		memset(&d->de, 0, sizeof(struct dirent64));  \
		d->de.dirent.d_type = DT_CHR;  \
		strcpy(d->de.dirent.d_name, vctx.vdevbase);  \
		log("common_" #name " inserting <%s>\n", vctx.vdevbase); \
		de = &d->de.dirent;  \
		d->dx_fnd = DxDevEnd;  \
	    }; \
	    return de;  \
	case DxDevFnd:  \
	case DxOther:  \
	default:  \
	    return de;  \
    }  \
}

MKCOMMON_RDIR(readdir, dirent)
MKCOMMON_RDIR(readdir64, dirent64)
MKCOMMON_RDIR(readdir_r, dirent)
MKCOMMON_RDIR(readdir64_r, dirent64)

#define MKREADDIR(name, dirent)  \
struct dirent *name(DIR *dir)  \
{  \
    static struct dirent * (*orig)(DIR *) = NULL;  \
    struct dirent *de;  \
    dx_t *d;  \
  \
    d = dxtab_find(dir); \
    if(d == NULL) return NULL; \
    if(d->dx_fnd == DxDevEnd) return NULL;  \
  \
    if(orig == NULL) {  \
	orig = dlsym(RTLD_NEXT, #name);  \
	if(orig == NULL) return NULL;  \
    }  \
  \
    if(d->dx_fnd != DxMkDir) { \
	de = orig(d->dx_dir);  \
	if(de != NULL) { \
	    if(strcmp(de->d_name, vctx.vdevbase) == 0) { \
		d->dx_fnd = DxDevFnd; \
	    } \
	} \
    } else { \
	de = NULL; \
	d->dx_fnd = DxDev; \
    } \
 \
    return common_##name(d, de); \
} 

MKREADDIR(readdir, dirent)
MKREADDIR(readdir64, dirent64)
 
#define MKREADDIR_R(name, dirent) \
int name(DIR *dir, struct dirent *entry, \
struct dirent **result) \
{ \
    static int (*orig)(DIR *, struct dirent *, \
	    struct dirent **) = NULL; \
    struct dirent *de;  \
    dx_t *d;  \
    int rv; \
 \
    d = dxtab_find(dir); \
    if(d == NULL) return -1; \
    if(d->dx_fnd == DxDevEnd) { \
	*result = NULL; \
	return 0; \
    } \
  \
    if(orig == NULL) { \
	orig = dlsym(RTLD_NEXT, #name); \
	if(orig == NULL) return -1; \
    } \
    log("#1" #name "\n"); \
 \
    if(d->dx_fnd != DxMkDir) { \
	rv = orig(d->dx_dir, entry, result);  \
	de = *result; \
	if(de != NULL) { \
	    if(strcmp(de->d_name, vctx.vdevbase) == 0) { \
		d->dx_fnd = DxDevFnd; \
	    } \
	} \
    } else { \
log("#2" #name "\n"); \
	de = NULL; \
	d->dx_fnd = DxDev; \
	rv = 0; \
    } \
    de = common_##name(d, de); \
    *result = de; \
    if(d->dx_fnd == DxDevEnd) { \
	if(de != NULL) { \
	    memcpy(entry, de, sizeof *de); \
	} \
	rv = 0; \
    } \
 \
    return rv; \
}

MKREADDIR_R(readdir_r, dirent)
MKREADDIR_R(readdir64_r, dirent64)

#define FUNARGS(...) , __VA_ARGS__
#define MKDIRFUN(ret, reterr, name, argts, args) \
ret name argts \
{ \
    static ret (*orig) argts = NULL; \
    dx_t *d; \
 \
debug(#name "\n"); \
    if(orig == NULL) { \
	orig = dlsym(RTLD_NEXT, #name); \
	if(orig == NULL) return reterr; \
    } \
    d = dxtab_find(dir); \
    if(d == NULL) return reterr; \
 \
    return orig(d->dx_dir args); \
} 

MKDIRFUN(void, ,rewinddir, (DIR *dir), )
MKDIRFUN(off_t, -1, telldir, (DIR *dir), )
MKDIRFUN(void, , seekdir, (DIR *dir, off_t offset), FUNARGS(offset))
MKDIRFUN(int, -1, dirfd, (DIR *dir), )
 
int closedir(DIR *dir)
{
    static int (*orig)(DIR *) = NULL;
    dx_t *d;
    int rv;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "closedir");
	if(orig == NULL) return -1;
    }
    d = dxtab_find(dir); 
    if(d == NULL) return -1; 
    rv = orig(d->dx_dir);
    dxtab_rm(dir);
#if 0
    if(vctx.vpmmap != NULL) {
	free(vctx.vpmmap);
log("set vpmmap to NULL\n");
	vctx.vpmmap = NULL;
    }
#endif

    return rv;
}

#ifdef HAVE_XATTR
#define MKGETXATTR(name) \
ssize_t name(const char *path, const char *name, \
		void *value, size_t size) \
{ \
    static ssize_t (*orig)(const char *, const char *, \
		    void *, size_t) = NULL; \
    char buf[PATH_MAX]; \
    ssize_t rv; \
 \
    if(orig == NULL) { \
	orig = dlsym(RTLD_NEXT, #name); \
	if(orig == NULL) { \
	    err("symbol " #name " not found\n"); \
	    return -1; \
	} \
    } \
    rv = orig(path, name, value, size); \
    if(rv == -1) { \
	if(is_videodev(path)) { \
log(#name " path <%s> name <%s>\n", path, name); \
	    errno = EOPNOTSUPP; \
	} else { \
log(#name " path <%s> name <%s>\n", path, name); \
	    normalize(path, buf); \
	    if(strcmp("/dev/v4l", buf) == 0) { \
		errno = EOPNOTSUPP; \
	    } \
	} \
    } \
 \
    return rv; \
} 

MKGETXATTR(getxattr)
MKGETXATTR(lgetxattr)
#endif
