/*
  $Header: /cvs/src/chrony/nameserv.h,v 1.8 2002/02/28 23:27:11 richard Exp $

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

  Module header for nameserver functions
  */


#ifndef GOT_NAMESERV_H
#define GOT_NAMESERV_H

#include "addressing.h"

/* Resolve names only to selected address family */
extern void DNS_SetAddressFamily(int family);

extern int DNS_Name2IPAddress(const char *name, IPAddr *addr, int retry);

extern void DNS_IPAddress2Name(IPAddr *ip_addr, char *name, int len);

#endif /* GOT_NAMESERV_H */

