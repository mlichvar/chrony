/*
  $Header: /cvs/src/chrony/srcparams.h,v 1.10 2002/02/28 23:27:14 richard Exp $

  =======================================================================

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
  unsigned long authkey;
  double max_delay;
  double max_delay_ratio;
  SRC_SelectOption sel_option;
} SourceParameters;

#define INACTIVE_AUTHKEY 0UL

#endif /* GOT_SRCPARAMS_H */
