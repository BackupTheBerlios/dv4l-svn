#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/mman.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <libraw1394/raw1394.h>
#include <libiec61883/iec61883.h>
#include <libdv/dv.h>
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
#include "config.h"
#include "scale.h"
#include "palettes.h"
#include "util.h"

#define VIDEOGROUP "video"
#define VIDEODEV "/dev/video0"
#define VIDEOV4LDEV "/dev/v4l/video0"
static int fake_fd = -1;

typedef enum { DvIdle = 1, DvRead, DvMmap } dv4l_run_t;

typedef struct {
    int vfd;
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
} vid_context_t;

static vid_context_t vctx = { 0 };

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

    return (strcmp(VIDEODEV, resolved) == 0)
    || (strcmp(VIDEOV4LDEV, resolved) == 0);
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

#define PRELOAD "LD_PRELOAD="
static void strip_environ()
{
#if 0
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
    const char *deb;
    char *err;
    int lvl;

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "getenv");
	if(orig == NULL) return NULL;
	strip_environ();
	deb = getenv("DV4L_VERBOSE");
	if(deb != NULL) {
	    lvl = strtol(deb, &err, 0);
	    if(*deb != '\0' && *err == '\0') {
		set_tracelevel(lvl);
		log("set tracelevel to %d\n", lvl);
	    }
	}
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
    if(is_videodev(path)) { \
	memset(buf, 0, sizeof *buf); \
	buf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP; \
	buf->st_rdev = makedev(81, 10); \
	buf->st_gid = vidgid; \
	rv = 0; \
    } \
 \
    return rv; \
}

MKXSTAT(__xstat, stat)
MKXSTAT(__xstat64, stat64)
MKXSTAT(__lxstat, stat)
MKXSTAT(__lxstat64, stat64)

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
    if(fd != fake_fd || (flags & MAP_ANONYMOUS)) { \
	rv = orig(start, len, prot, flags, fd, off); \
    } else if(fake_fd != -1) { \
	vctx.vpmmap = malloc(MAXBUF * vctx.vimgsz); \
	if(vctx.vpmmap == NULL) return NULL; \
	rv = vctx.vpmmap; \
    } else { \
	err("invalid fd in " #name "\n"); \
	rv = NULL; \
    } \
    log("rv 0x%lx\n", rv); \
 \
    return rv; \
}

MKMMAP(mmap);
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
    if(vctx.vstate == DvIdle) {
	if(vctx.vpmmap != NULL) {
	    free(vctx.vpmmap);
	    vctx.vpmmap = NULL;
	}
	rv = 0;
    } else {
	rv = -1;
    }

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
    struct timeval now;
    unsigned long td;

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
	gettimeofday(&now, NULL);
	td = timediff(&now, &vc->vlastcomplete);
	gettimeofday(&vc->vlastcomplete, NULL);
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
}

#define MKOPEN(name) \
static int (*orig##name)(const char *, int flags, ...) = NULL; \
int name(const char *path, int flags, ...) \
{ \
    va_list argp; \
    static int libdv_inited = 0; \
    char *p; \
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
	    return -1; \
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
	vctx.vpitches[0] = 0; \
	vctx.vfd = raw1394_get_fd(vctx.vraw); \
	vctx.vrbuf = NULL; \
	vctx.vpmmap = NULL; \
	memset(&vctx.vlastcomplete, 0, sizeof vctx.vlastcomplete); \
	vctx.vcap.maxwidth = 0; \
	vctx.vcap.maxheight = 0; \
debug("#2 dv4l open vfd %d\n", vctx.vfd); \
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
	vctx.vstate = DvRead; \
    } else { \
	va_start(argp, flags); \
	p = va_arg(argp, char *); \
	rv = orig##name(path, flags, p); \
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

    if(orig == NULL) {
	orig = dlsym(RTLD_NEXT, "read");
	if(orig == NULL) return -1;
    }
    if(fd != fake_fd) {
	cnt = orig(fd, buf, count);
	return cnt;
    }
    cnt = 1;
    FD_ZERO(&rfds);
    while(cnt > 0) {
	FD_SET(vctx.vfd, &rfds);
	vctx.vrbuf = buf;
	if(select(vctx.vfd + 1, &rfds, NULL, NULL, NULL) > 0) {
	    raw1394_loop_iterate(vctx.vraw);
	    if(vctx.vcomplete) {
		--cnt;
	    }
	}
    }

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
	    || vpic->palette == VIDEO_PALETTE_YUV420P) {
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
		vctx.vwin.width = vmmap->width;
		vctx.vwin.height = vmmap->height;
		vctx.vpic.palette = vmmap->format;
		read(fake_fd,
			vctx.vpmmap + vctx.vimgsz * (vmmap->frame & 1),
			vctx.vimgsz);
		vctx.vstate = DvMmap;
		rv = 0;
	    } else {
log("VIDIOCSYNC no mem mapped\n");
		rv = -1;
	    }
	    return rv;
	case VIDIOCGCAPTURE:
	case VIDIOCGFBUF:
	default:
log("unsupported ioctl %d\n", request);
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
    }
    if(fd == fake_fd) {
log("close fake_fd");
	iec61883_dv_fb_stop(vctx.viec);
	fake_fd = -1;
    }

    return orig(fd);
}

typedef struct dx_s {
    DIR *dx_dir;
    enum { DxOther = 1, DxDev, DxDevFnd, DxDevEnd } dx_fnd;
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
    }
    dir = orig(path);
    if(dir == NULL) return dir;

    if(realpath(path, resolved) == NULL) return NULL;
    d = malloc(sizeof *d);
    if(d == NULL) return NULL;
    d->dx_dir = dir;
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
    de = orig(d->dx_dir);  \
    switch(d->dx_fnd) {  \
	case DxDev:  \
	    if(de != NULL) {  \
		if(strcmp("video0", de->d_name) == 0) {  \
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
    vctx.vstate = DvIdle;

    return rv;
}

