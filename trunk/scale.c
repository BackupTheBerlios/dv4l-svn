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


#include <stdlib.h>
#include <string.h>
#include "scale.h"

static void write_px_rgb(const pixel_t * const v, pixel_t * const p)
{
    // memcpy(p, v, 3);
    p[0] = v[2];
    p[1] = v[1];
    p[2] = v[0];
}

static void write_px_bgr(const pixel_t * const v, pixel_t * const p)
{
    // memcpy(p, v, 3);
    p[0] = v[0];
    p[1] = v[1];
    p[2] = v[2];
}

static inline void average(const pixel_t *x, const pixel_t *y, pixel_t *p)
{
    p[0] = (x[0] + y[0]) / 2;
    p[1] = (x[1] + y[1]) / 2;
    p[2] = (x[2] + y[2]) / 2;
}

static void (*write_px)(const pixel_t * const v, pixel_t * const p)
    = write_px_rgb;

void set_color_correction(int rgb)
{
    if(rgb) {
	write_px = write_px_bgr;
    } else {
	write_px = write_px_rgb;
    }
}

/*
 * Bresenham image scaling
 * http://www.ddj.com/showArticle.jhtml?articleID=184405045
 */
void scale_line(
	const pixel_t *src,
	pixel_t *dst, 
	int src_w,
	int dst_w
    )
{
    int num;
    int ip;
    int fp;
    int e;

    num = dst_w;
    ip = src_w / dst_w;
    fp = src_w % dst_w;
    e = 0;
    while(num > 0) {
	write_px(src, dst);
	dst += 3;
	src += 3 * ip;
	e += fp;
	if(e >= dst_w) {
	    e -= dst_w;
	    src += 3;
	}
	--num;
    }
}

void scale(
	const pixel_t *src,
	pixel_t *dst,
	int src_w,
	int src_h,
	int dst_w,
	int dst_h
    )
{
    int num;
    int ip;
    int fp;
    int e;
    const pixel_t *prev_src;

    num = dst_h;
    ip = (src_h / dst_h) * src_w;
    fp = src_h % dst_h;
    e = 0;
    prev_src = NULL;

    while(num > 0) {
	if(prev_src == src) {
	    /*
	     * re-use last line
	     */
	    memcpy(dst, dst - dst_w * 3, dst_w * 3);
	} else {
	    scale_line(src, dst, src_w, dst_w);
	    prev_src = src;
	}
	dst += dst_w * 3;
	src += ip * 3;
	e += fp;
	if(e >= dst_h) {
	    e -= dst_h;
	    src += src_w * 3;
	}
	--num;
    }
}
