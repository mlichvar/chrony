/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2002
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

  Header file defining parameters that can be set on a per source basis
  */

#ifndef GOT_SRCPARAMS_H
#define GOT_SRCPARAMS_H

#include "sources.h"

typedef struct {
  int minpoll;
  int maxpoll;
  int online;
  int auto_offline;
  int presend_minpoll;
  int iburst;
  int min_stratum;
  int poll_target;
  int version;
  int max_sources;
  uint32_t authkey;
  double max_delay;
  double max_delay_ratio;
  double max_delay_dev_ratio;
  SRC_SelectOption sel_option;
} SourceParameters;

#define SRC_DEFAULT_PORT 123
#define SRC_DEFAULT_MINPOLL 6
#define SRC_DEFAULT_MAXPOLL 10
#define SRC_DEFAULT_PRESEND_MINPOLL 0
#define SRC_DEFAULT_MAXDELAY 16.0
#define SRC_DEFAULT_MAXDELAYRATIO 0.0
#define SRC_DEFAULT_MAXDELAYDEVRATIO 10.0
#define SRC_DEFAULT_MINSTRATUM 0
#define SRC_DEFAULT_POLLTARGET 6
#define SRC_DEFAULT_MAXSOURCES 4
#define INACTIVE_AUTHKEY 0

#endif /* GOT_SRCPARAMS_H */
