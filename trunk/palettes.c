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


/*
 * RGB to YUV420P conversion taken from
 * en.wikipedia.org/wiki/YUV
 */
static inline unsigned char get_u(const unsigned char * const rgb)
{
    return ((-38 * rgb[0] - 74 * rgb[1] + 112 * rgb[2] + 128) >> 8) + 128;
}

static inline unsigned char get_v(const unsigned char * const rgb)
{
    return ((112 * rgb[0] - 94 * rgb[1] - 18 * rgb[2] + 128) >> 8) + 128;
}

void rgb24toyuv420p(
	const unsigned char *rgb,
	unsigned char *dst,
	int w,
	int h
    )
{
    int i;
    int j;
    const unsigned char *s;
    const unsigned char *t;
    unsigned char *d;
    unsigned char *ud;
    unsigned char *vd;
    unsigned char y;
    unsigned char u;
    unsigned char v;

    for(i = 0, s = rgb, t = rgb + 3 * w,
	d = dst, vd = dst + (w * h), ud = vd + (w * h) / 4;
	i < h;
	++i) {
	for(j = 0; j < w; ++j) {
	    y = ((66 * s[0] + 129 * s[1] + 25 * s[2] + 128) >> 8) + 16;
	    *d = y;
	    ++d;
	    if (((i | j) & 0x1) == 0) {
		u = (get_u(s) + get_u(s + 3) + get_u(t) + get_u(t + 3)) / 4;
		*ud = u;
		++ud;
		v = (get_v(s) + get_v(s + 3) + get_v(t) + get_v(t + 3)) / 4;
		*vd = v;
		++vd;
	    }
	    s += 3;
	    t += 3;
	}
    }
}


