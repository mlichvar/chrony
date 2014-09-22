/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2014
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 **********************************************************************

  =======================================================================

  Utility functions for memory allocation.

  */

#include "config.h"

#include "logging.h"
#include "memory.h"

void *
Malloc(size_t size)
{
  void *r;

  r = malloc(size);
  if (!r && size)
    LOG_FATAL(LOGF_Memory, "Could not allocate memory");

  return r;
}

void *
Realloc(void *ptr, size_t size)
{
  void *r;

  r = realloc(ptr, size);
  if (!r && size)
    LOG_FATAL(LOGF_Memory, "Could not allocate memory");

  return r;
}

char *
Strdup(const char *s)
{
  void *r;

  r = strdup(s);
  if (!r)
    LOG_FATAL(LOGF_Memory, "Could not allocate memory");

  return r;
}
