/*
  $Header: /cvs/src/chrony/cmdparse.h,v 1.7 2002/02/28 23:27:09 richard Exp $

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
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 * 
 **********************************************************************

  =======================================================================

  Header file for the command parser
  */

#ifndef GOT_CMDPARSE_H
#define GOT_CMDPARSE_H

#include "srcparams.h"

typedef enum {
  CPS_Success,
  CPS_BadOption,
  CPS_BadHost,
  CPS_BadPort,
  CPS_BadMinpoll,
  CPS_BadMaxpoll,
  CPS_BadPresend,
  CPS_BadMaxdelayratio,
  CPS_BadMaxdelay,
  CPS_BadKey
} CPS_Status;

typedef struct {
  unsigned long ip_addr;
  unsigned short port;
  SourceParameters params;
} CPS_NTP_Source;

/* Parse a command to add an NTP server or peer */
extern CPS_Status CPS_ParseNTPSourceAdd(const char *line, CPS_NTP_Source *src);
  


#endif /* GOT_CMDPARSE_H */
