/*
  $Header: /cvs/src/chrony/broadcast.h,v 1.2 2002/02/28 23:27:08 richard Exp $

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

  Deal with broadcast server functions.
  */

#ifndef GOT_BROADCAST_H
#define GOT_BROADCAST_H

extern void BRD_Initialise(void);
extern void BRD_Finalise(void);
extern void BRD_AddDestination(unsigned long addr, unsigned short port, int interval);

#endif /* GOT_BROADCAST_H */

