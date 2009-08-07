/*
  $Header: /cvs/src/chrony/addressing.h,v 1.7 2002/02/28 23:27:08 richard Exp $

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

  Types used for addressing sources etc
  */

#ifndef GOT_ADDRESSING_H
#define GOT_ADDRESSING_H

/* This type is used to represent an IPv4 address and port
   number.  Both parts are in HOST order, NOT network order. */
typedef struct {
  unsigned long ip_addr;
  unsigned long local_ip_addr;
  unsigned short port;
} NTP_Remote_Address;

#if 0
unsigned long NTP_IP_Address;
#endif

#endif /* GOT_ADDRESSING_H */

