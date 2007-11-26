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


#ifndef _scale_h
#define _scale_h

typedef unsigned char pixel_t;
void set_color_correction(int rgb);
void scale(
        const pixel_t *src,
        pixel_t *dst,
        int src_w,
        int src_h,
        int dst_w,
        int dst_h
    );


#endif
