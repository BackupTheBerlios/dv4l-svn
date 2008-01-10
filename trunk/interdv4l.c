#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>

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
#include "scale.h"
#include "palettes.h"

#define VIDEOGROUP "video"
#define VIDEODEV "/dev/video0"
static int fake_fd;

typedef struct {
    int vfd;
    unsigned char *vrbuf;
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

    return (strcmp(VIDEODEV, resolved) == 0);
}

#define MKSTAT(name, stat) \
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
    int rv; \
 \
    if(orig##name == NULL) { \
	orig##name = dlsym(RTLD_NEXT, #name); \
	if(orig##name == NULL) return -1; \
    } \
    if(fd != fake_fd) { \
	va_start(argp, cmd); \
	rv = orig##name(fd, cmd, argp); \
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
}

#define MKOPEN(name) \
static int (*orig##name)(const char *, int flags, ...) = NULL; \
int name(const char *path, int flags, ...) \
{ \
    va_list argp; \
    static int libdv_inited = 0; \
    int rv; \
     \
    if(orig##name == NULL) { \
	orig##name = dlsym(RTLD_NEXT, #name); \
	if(orig##name == NULL) return -1; \
    } \
    if(is_videodev(path)) { \
printf("#1 dv4l open\n"); \
	rv = orig##name("/dev/null", O_RDONLY); \
	fake_fd = rv; \
	if(!libdv_inited) { \
	    vctx.vraw = raw1394_new_handle_on_port(0); \
	    if(vctx.vraw == NULL) { \
		return -1; \
	    } \
printf("#1 dv4l open libdv_init\n"); \
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
	    vctx.vpic.depth = 24; \
	    libdv_inited = 1; \
	} \
	vctx.vpitches[0] = 0; \
	vctx.vfd = raw1394_get_fd(vctx.vraw); \
	vctx.vrbuf = NULL; \
	vctx.vcap.maxwidth = 0; \
	vctx.vcap.maxheight = 0; \
	vctx.vwin.width = 320; \
	vctx.vwin.height = 240; \
printf("#2 dv4l open vfd %d\n", vctx.vfd); \
	get_camsize(&vctx); \
printf("#3 dv4l open\n"); \
	if(iec61883_dv_fb_start(vctx.viec, 63) < 0) { \
	    return -1; \
	} \
printf("#4 dv4l open\n"); \
    } else { \
	va_start(argp, flags); \
	rv = orig##name(path, flags, argp); \
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
	    cnt -= vctx.vcomplete;
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
printf("dv4l write\n");

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
    char *p;
    int rv;

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
	    vcap->type = VID_TYPE_CAPTURE | VIDEO_TYPE_CAMERA;
            vcap->audios = 0;
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
            strncpy(vchan->name, "dv1394", sizeof vchan->name);
            vchan->tuners = 0;
            vchan->flags = VIDEO_TYPE_CAMERA;
            vchan->norm = VIDEO_MODE_AUTO;
printf("#2dv4l ioctl\n");
            return 0;
        case VIDIOCSCHAN:
printf("#3dv4l ioctl\n");
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
            vpic->depth = 3;
            vpic->palette = vctx.vpic.palette;
            return 0;
        case VIDIOCSPICT:
            va_start(argp, request);
            vpic = va_arg(argp, struct video_picture *);
            va_end(argp);
printf("#5dv4l ioctl\n");
            return 0;
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
	    vctx.vwin.width = vwin->width;
	    vctx.vwin.height = vwin->height;
            return 0;
	case VIDIOCGFBUF:
	case VIDIOCGCAPTURE:
	case VIDIOCGMBUF:
	default:
printf("#8dv4l ioctl\n");
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
    }
    if(fd == fake_fd) {
	fake_fd = -1;
    }

    return orig(fd);
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
