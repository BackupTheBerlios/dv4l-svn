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
#ifndef _palettes_h
#define _palettes_h

int rgb24toyuv420p(
	const unsigned char * const rgb,
	unsigned char *dst,
	int w,
	int h
    );
static inline int palette_conv(
	const unsigned char * const rgb,
	unsigned char *dst,
	int palette,
	int w,
	int h
    )
{
    switch(palette) {
	case VIDEO_PALETTE_YUV420P:
	    return rgb24toyuv420p(rgb, dst, w, h);
	case VIDEO_PALETTE_RGB24:
	default:
	    return w * h * 3;
    }
}

#endif

