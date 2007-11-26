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

#ifndef _util_h
#define _util_h

#define TRACE_ERR 1
#define TRACE_LOG 2
#define TRACE_DEB 3

#define err(fmt, ...) _trace(TRACE_ERR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log(fmt, ...) _trace(TRACE_LOG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define debug(fmt, ...) \
_trace(TRACE_DEB, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

void _trace(int, const char *, int, const char *fmt, ...);
void set_tracelevel(int);

#endif
