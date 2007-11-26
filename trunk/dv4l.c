/*
 * Copyright (C) 2007 Free Software Foundation, Inc.
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
 * Author: Wolfgang Beck <bewo at users.berlios.de> 2007
 */


#include <linux/videodev.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <libraw1394/raw1394.h>
#include <libiec61883/iec61883.h>
#include <libdv/dv.h>
#include "config.h"
#include "scale.h"
#include "util.h"

#define MAXW (720 * 2)
#define MAXH (576 * 2)
#define MINW 128
#define MINH 96
#define MAXFRAMES 2

#define VIDEODEV "/dev/v4l/video"
#define VIDIOCSINVALID  _IO('v',BASE_VIDIOCPRIVATE+1)
#define FRM_EMPTY (-1)
#define FRM_RDY4SYNC (-2)

#ifndef DV4LVERSION
#define DV4LVERSION "0.0"
#endif

/*
 * these constants are used to detect a vloopback input device
 * let's hope they won't change them
 */
#define VLOOP_P1 "Video loopback"
#define VLOOP_P1S ((sizeof VLOOP_P1) - 1)
#define VLOOP_P2 "input"
#define VLOOP_P2S ((sizeof VLOOP_P2) - 1)

#define NUM_BUFS 8

typedef struct {
    struct video_mbuf vmbuf;
    unsigned char *vframebuf;
    /*
     * queue of frame numbers returned from user app
     */
    unsigned char *vfreeframes[NUM_BUFS];
    int v_wr;
    int v_rd;
    int v_cnt;
    struct video_capability vcap;
    struct video_channel vchan;
    struct video_picture vpic;
    struct video_window vwin;
} vid_context_t;


static int vsync_set[MAXFRAMES];

static inline int max(int x, int y)
{
    return x > y ? x : y;
}

static unsigned char **buf_enqueue(
	vid_context_t *ctx,
	unsigned char *frame
    )
{
    unsigned char **rv;

    if(ctx->v_cnt < NUM_BUFS) {
	rv = ctx->vfreeframes + ctx->v_wr;
	ctx->v_wr = (ctx->v_wr + 1) % NUM_BUFS;
	++ctx->v_cnt;
	*rv = frame;
    } else {
	rv = NULL;
    }

    return rv;
}

static int buf_empty(const vid_context_t *ctx)
{
    return ctx->v_cnt == 0;
}

static unsigned char *buf_dequeue(vid_context_t *ctx)
{
    unsigned char *rv;

    if(ctx->v_cnt > 0) {
	rv = ctx->vfreeframes[ctx->v_rd];
	ctx->v_rd = (ctx->v_rd + 1) % NUM_BUFS;
	--ctx->v_cnt;
    } else {
	rv = NULL;
    }

    return rv;
}

static void init_ctx(vid_context_t *ctx, int w, int h)
{
    snprintf(ctx->vcap.name, sizeof ctx->vcap.name, "DV4Linux dv1394 to V4L");
    ctx->vcap.channels = 1;
    ctx->vcap.audios = 0;
    ctx->vcap.type = VID_TYPE_CAPTURE;
    ctx->vcap.maxwidth = w;
    ctx->vcap.maxheight = h;
    ctx->vcap.minwidth = MINW;
    ctx->vcap.minheight = MINH;

    ctx->vchan.channel = 0;
    snprintf(ctx->vchan.name, sizeof ctx->vchan.name, "DVCam");
    ctx->vchan.tuners = 0;
    ctx->vchan.type = VIDEO_TYPE_CAMERA;
    ctx->vchan.flags = 0;
    ctx->vchan.norm = VIDEO_MODE_AUTO;

    ctx->vpic.brightness = 0x8000;
    ctx->vpic.hue = 0x8000;
    ctx->vpic.colour = 0x8000;
    ctx->vpic.contrast = 0x8000;
    ctx->vpic.whiteness = 0x8000;
    ctx->vpic.depth = 24;
    ctx->vpic.palette = VIDEO_PALETTE_RGB24;

    ctx->vwin.x = 0;
    ctx->vwin.y = 0;
    ctx->vwin.width = ctx->vcap.maxwidth;
    ctx->vwin.height = ctx->vcap.maxheight;
    ctx->vwin.clips = NULL;
    ctx->vwin.clipcount = 0;

    ctx->v_rd = 0;
    ctx->v_wr = 0;
    ctx->v_cnt = 0;
}

/*
 * init memory mapped frame buffer shared with the vloopback driver
 */
static unsigned char *init_vmbuf(vid_context_t *ctx, int fd)
{
    unsigned char *rv;
    struct video_mbuf *p;
    int i;

    p = &ctx->vmbuf;
    p->size = MAXFRAMES * MAXW * MAXH * 3;
    p->frames = MAXFRAMES;
    rv = mmap(NULL, p->size,
		PROT_READ | PROT_WRITE,
		MAP_SHARED,
		fd, 0);
    if(rv == MAP_FAILED) {
	perror("mmap");
	return NULL;
    }
    ctx->vframebuf = rv;
    for(i = 0; i < p->frames; ++i) {
	p->offsets[i] = i * MAXW * MAXH * 3;
    }

    return rv;
}

/*
 * handle vloopback ioctls
 */
static int do_ioctl(
	int fd,
	unsigned long nr,
	const char *buf,
	vid_context_t *p
    )
{
    static int run = 1;
    unsigned char *vb;
    int fnr;
    int stop;

    const struct video_picture *spic;
    const struct video_window *swin;
    const struct video_mmap *smmap;

    stop = 0;
    if(run == 0) {
	memset(vsync_set, FRM_EMPTY, sizeof(int) * MAXFRAMES);
	run = 1;
    }
    switch(nr) {
	case VIDIOCGCAP:
log("VIDIOCGCAP\n");
            ioctl(fd, nr, &p->vcap);
            break;
        case VIDIOCGCHAN:
log("VIDIOCGCHAN\n");
            ioctl(fd, nr, &p->vchan);
            break;
        case VIDIOCSCHAN:
log("VIDIOCSCHAN\n");
            ioctl(fd, nr, buf);
            break;
        case VIDIOCGPICT:
log("VIDIOCGPICT\n");
            ioctl(fd, nr, &p->vpic);
            break;
        case VIDIOCGMBUF:
log("VIDIOCGMBUF\n");
            ioctl(fd, nr, &p->vmbuf);
            break;
        case VIDIOCGWIN:
            ioctl(fd, nr, &p->vwin);
            break;
	case VIDIOCSPICT:
log("VIDIOCSPICT\n");
	    spic = (const struct video_picture *)buf;
	    if(spic->palette != p->vpic.palette) {
		ioctl(fd, VIDIOCSINVALID);
		break;
	    }
	    ioctl(fd, nr, buf);
	    break;
	case VIDIOCSWIN:
log("VIDIOCSWIN\n");
	    swin = (const struct video_window *)buf;
	    if(swin->clips != NULL || swin->clipcount != 0) {
		ioctl(fd, VIDIOCSINVALID);
		break;
	    }
	    if(swin->width != p->vwin.width
	    || swin->height != p->vwin.height) {
		if(swin->width > MAXW || swin->height > MAXH) {
		    ioctl(fd, VIDIOCSINVALID);
		    break;
		}
		p->vwin.width = swin->width;
		p->vwin.height = swin->height;
		ioctl(fd, nr, buf);
		break;
	    } else {
		ioctl(fd, nr, buf);
	    }
	    break;
	case VIDIOCMCAPTURE:
	    smmap = (const struct video_mmap *)buf;
	    if(smmap->format != p->vpic.palette) {
		ioctl(fd, VIDIOCSINVALID);
	    }
	    p->vwin.width = smmap->width;
	    p->vwin.height = smmap->height;
debug("VIDIOCMCAPTURE ioctl %d\n", smmap->frame);
	    vb = p->vframebuf + p->vmbuf.offsets[smmap->frame];
	    buf_enqueue(p, vb);
	    ioctl(fd, nr, buf);
	    break;
	case VIDIOCSYNC:
	    fnr = *(const int *)buf;
debug("VIDIOCSYNC %d\n", fnr);
	    if(0 <= fnr && fnr < MAXFRAMES) {
		switch(vsync_set[fnr]) {
		    case FRM_EMPTY:
			if(buf_empty(p)) {
			    err("VIDIOCSYNC without VIDIOCMCAPTURE\n");
			    ioctl(fd, VIDIOCSINVALID);
			} else {
			    vsync_set[fnr] = fd;
			}
			break;
		    case FRM_RDY4SYNC:
			debug("direct VIDIOCSYNC %d\n", fnr);
			ioctl(fd, nr, buf);
			vsync_set[fnr] = FRM_EMPTY;
			break;
		    default:
			/*
			 * double SYNC on same frame, threaded user app?
			 */
			break;
		}
	    } else {
		ioctl(fd, VIDIOCSINVALID);
	    }
	    break;
	case 0:
	    ioctl(fd, nr, buf);
	    run = 0;
	    stop = 1;
	    break;
	default:
log("unsupported ioctl 0x%lx\n", nr);
	    ioctl(fd, VIDIOCSINVALID);
	    break;
    }

    return stop;
}

static int checkdev(const char *devname)
{
    int fd;
    struct video_capability vc;
    int slen;
    int rv;

    fd = open(devname, O_RDWR);
    if(fd == -1) return -1;

    if(ioctl(fd, VIDIOCGCAP, &vc) == 0) {
        close(fd);
        slen = strlen(vc.name);
        if(strncmp(VLOOP_P1, vc.name, VLOOP_P1S) == 0) {
            return !strncmp(VLOOP_P2,
                        vc.name + slen - VLOOP_P2S,
                        VLOOP_P2S) == 0;
        }
        rv = -1;
    } else {
        rv = -2;
    }
    close(fd);

    return rv;
}

static int scan_dev(char *devname, int sz)
{
    int i;

    for(i = 0; i < 10; ++i) {
        snprintf(devname, sz, VIDEODEV "%d", i);
        if(checkdev(devname) == 0) return i;
    }

    return -1;
}

#if 0
static int selport(raw1394handle_t h)
{
    int num;
    int i;
    struct raw1394_portinfo pinf[16];
    int rv;

    num = raw1394_get_port_info(h, pinf, 16);
    rv = -1;
    for(i = 0; i < num; ++i) {
	printf("nodes %d name <%s>\n",
	    pinf[i].nodes, pinf[i].name);
	rv = pinf[i].nodes;
    }

    return 0;
}
#endif

typedef struct {
    dv_decoder_t *dvdec;
    uint8_t **frame;
    uint8_t *dst;
    int pitches[3];
    vid_context_t *vctx;
} fcb_arg_t;

static int frame_recv(
	    unsigned char *data,
	    int len,
	    int complete,
	    void *arg
	)
{
    fcb_arg_t *p;
    vid_context_t *ctx;
    int w;
    int h;
    uint8_t *frame;
    unsigned char *vb;
    int snr;

    p = (fcb_arg_t *)arg;
    ctx = p->vctx;
    if(complete) {
	dv_parse_header(p->dvdec, data);
	if(p->frame == NULL) {
	    /*
	     * on-demand initialization, must be called before
	     * vloopback ioctl handling has started
	     *
	     */
	    w = p->dvdec->width;
	    h = p->dvdec->height;
	    debug("w %d h %d\n", p->dvdec->width, p->dvdec->height);
	    frame = malloc(w * h * 3);
	    p->frame = malloc(3 * sizeof frame);
	    p->frame[0] = frame;
	    p->frame[1] = NULL;
	    p->frame[2] = NULL;

	    p->pitches[0] = w * 3;
	    p->pitches[1] = 0;
	    p->pitches[2] = 0;

	    init_ctx(ctx, w, h);
	    ctx->vwin.width = w;
	    ctx->vwin.height = h;
	} else {
	    w = ctx->vcap.maxwidth;
	    h = ctx->vcap.maxheight;
	}
	vb = buf_dequeue(p->vctx);
	if(vb != NULL) {
	    snr = (vb - ctx->vframebuf) / (MAXW * MAXH * 3);
	    p->dst = vb;
	    dv_decode_full_frame(p->dvdec, data,
				    e_dv_color_rgb,
				    p->frame,
				    p->pitches);
	    scale(p->frame[0], p->dst, w, h, ctx->vwin.width, ctx->vwin.height);
	    switch(vsync_set[snr]) {
		case FRM_EMPTY:
		    vsync_set[snr] = FRM_RDY4SYNC;
		    break;
		case FRM_RDY4SYNC:
		    /*
		     * already marked as syncable
		     */
		    break;
		default:
debug("do SYNC %d\n", snr);
		    ioctl(vsync_set[snr], VIDIOCSYNC, &snr);
		    vsync_set[snr] = FRM_EMPTY;
		    break;
	    };
	}
    } else {
	log("incomplete frame\n");
    }

    return 0;
}

static void handle_sigio(int sig, siginfo_t *info, void *uc)
{
    /*
     * just let the signal happen
     */
}

static struct sigaction old_sigio;
static int init_sig()
{
    static struct sigaction act;

    act.sa_handler = NULL;
    act.sa_sigaction = handle_sigio;
    act.sa_flags = SA_SIGINFO;
    sigemptyset(&act.sa_mask);
    sigaddset(&act.sa_mask, SIGIO);
    act.sa_restorer = NULL;
    if(sigaction(SIGIO, &act, &old_sigio) == -1) {
	perror("sigaction");
	return -1;
    }

    return 0;
}

/*
 * wait for the first frame that will cause frame_recv
 * to detect the frame size as reported from the
 * camera
 */
static void get_camsize(
	int fd,
	raw1394handle_t raw,
	vid_context_t *ctx
    )
{
    fd_set rfds;
    int rv;

    FD_ZERO(&rfds);
    while(ctx->vcap.maxwidth == 0) {
	FD_SET(fd, &rfds);
	rv = select(fd + 1, &rfds, NULL, NULL, NULL);
	if(rv > 0) {
	    if(FD_ISSET(fd, &rfds)) {
		raw1394_loop_iterate(raw);
	    }
	}
    }
}

static struct option long_options[] = {
    { "color-correction", 0, NULL, 'c' },
    { "device", 1, NULL, 'd' },
    { "help", 0, NULL, 'h' },
    { "verbose", 1, NULL, 'h' },
    { NULL, 0, NULL, 0 }
};

static const char *short_options = "cd:h?v:";

static void usage(const char *cmd, const char *txt)
{
    if(*txt != '\0') {
        printf("%s\n", txt);
    }
    printf(
"Usage: %s [OPTION] ...\n\
\n\
Version %s\n\
\n\
Push video stream of a dv1394 camera to a vloopback input device\n\
to provide a virtual video4linux camera.\n\
\n\
-c, --color-correction\n\
		 use this option if red objects look blue\n\
-d, --device     vloopback-input-device (eg /dev/v4l/video0)\n\
-v, --verbose    verbosity-level (0 - no output; 3 - all messages)\n\
-h, --help       display this help and exit\n",
cmd, DV4LVERSION);

}

static void cmdline(
	int argc,
	char **argv
    )
{
    int c;
    int option_index;
    unsigned long vv;
    char *ep;
    char buf[80];

    for(c = getopt_long(argc, argv, short_options,
                            long_options, &option_index);
        c != -1;
        c = getopt_long(argc, argv, short_options,
                            long_options, &option_index)) {
        switch(c) {
	    case 'c':
		set_color_correction(1);
		break;
            case 'd':
                if(optarg != NULL) {
                    strncpy(buf, optarg, sizeof buf);
                }
                break;
            case 'v':
                if(optarg != NULL) {
                    vv = strtol(optarg, &ep, 0);
                    if(*optarg != '\0' && *ep == '\0') {
                        set_tracelevel(vv);
                    } else {
                        usage(basename(argv[0]), "invalid verbosity level:");
                        exit(-1);
                    }
                }
                break;
            case 'h':
            case '?':
                usage(basename(argv[0]), "");
                exit(0);
            default:
                usage(basename(argv[0]), "error:");
                exit(-1);
        }
    }

}

int main(int argc, char **argv)
{
    raw1394handle_t raw;
    vid_context_t ctx;
    fd_set rfds;
    fcb_arg_t arg;
    int fd;
    int vfd;
    int rv;
    int devi;
    int run;
    int sz;
    char buf[1024];

    iec61883_dv_fb_t iec;
    dv_decoder_t *dvdec;

    cmdline(argc, argv);
    /*
     * get a vloopback dev
     */
    devi = scan_dev(buf, sizeof buf);
    if(devi < 0) {
           err("no vloopback input device found. "
	       "vloopback module in kernel?\n");
	   exit(-1);
    }
    printf("use " VIDEODEV "%d in your webcam application\n", devi + 1);
    vfd = open(buf, O_RDWR);
    if(vfd < 0) {
	perror("open vloopback");
	exit(-2);
    }
    init_vmbuf(&ctx, vfd);
    signal(SIGIO, SIG_IGN);
    memset(&vsync_set, -1, sizeof(int) * MAXFRAMES);
    /*
     * init libraw1394
     */
    raw = raw1394_new_handle_on_port(0);
    if(raw == NULL) {
	printf("raw1394_new_handle failed\n");
	exit(-1);
    }
    /*
     * init libdv
     */
    dv_init(0, 0);
    dvdec = dv_decoder_new(FALSE, FALSE, FALSE);
    if(dvdec == NULL) {
	perror("dv_decoder_new");
	exit(-1);
    }
    dv_set_quality(dvdec, DV_QUALITY_BEST);
    arg.dvdec = dvdec;
    arg.frame = NULL;
    arg.vctx = &ctx;
    /*
     * init iec61883
     */
    iec = iec61883_dv_fb_init(raw, frame_recv, &arg);
    if(iec == NULL) {
	printf("iec61883_dv_fb_init failed\n");
	exit(-1);
    }

    if(iec61883_dv_fb_start(iec, 63) < 0) {
	printf("iec61883_dv_fb_start failed\n");
	exit(-1);
    }
    fd = raw1394_get_fd(raw);
    get_camsize(fd, raw, &ctx);
    FD_ZERO(&rfds);
    run = 1;
    while(run) {
	FD_SET(vfd, &rfds);
	FD_SET(fd, &rfds);
	rv = select(max(vfd, fd) + 1, &rfds, NULL, NULL, NULL);
	if(rv > 0) {
	    if(FD_ISSET(vfd, &rfds)) {
		sz = read(vfd, buf, sizeof buf);
		if(sz > 0) {
		    if(do_ioctl(vfd, *(unsigned long *)buf,
				buf + sizeof(unsigned long), &ctx)) {
			iec61883_dv_fb_stop(iec);
			while(buf_dequeue(&ctx) != NULL) ;
			log("stopped\n");
			init_sig();
			FD_ZERO(&rfds);
			FD_SET(vfd, &rfds);
			rv = select(vfd + 1, &rfds, NULL, NULL, NULL);
			log("sleep inter\n");
			signal(SIGIO, SIG_IGN);
			if(iec61883_dv_fb_start(iec, 63) < 0) {
			    printf("iec61883_dv_fb_start failed\n");
			    exit(-1);
			}
		    }
		}
	    }
	    if(FD_ISSET(fd, &rfds)) {
		raw1394_loop_iterate(raw);
	    }
	} else {
	    perror("select");
	}
    }
    iec61883_dv_fb_close(iec);
    /*
     * cleanup libdv
     */
    dv_decoder_free(dvdec);
    dv_cleanup();

    /*
     * cleanup libraw1394
     */
    raw1394_destroy_handle(raw);

    return 0;
}
