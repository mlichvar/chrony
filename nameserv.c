/*
  $Header: /cvs/src/chrony/nameserv.c,v 1.14 2003/09/21 23:11:06 richard Exp $

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

  Functions to do name to IP address conversion

  */

#include "sysincl.h"

#include "nameserv.h"

/* ================================================== */

unsigned long
DNS_Name2IPAddress(const char *name)
{
  struct hostent *host;
  unsigned char *address0;
  unsigned long result;

  host = gethostbyname(name);
  if (host == NULL) {
    result = DNS_Failed_Address;
  } else {
    address0 = host->h_addr_list[0];
    result = ((((unsigned long)address0[0])<<24) |
	      (((unsigned long)address0[1])<<16) |
	      (((unsigned long)address0[2])<<8) |
	      (((unsigned long)address0[3])));
  }

  return result;

}

/* ================================================== */

const char *
DNS_IPAddress2Name(unsigned long ip_addr)
{
  struct hostent *host;
  static char buffer[16];
  unsigned int a, b, c, d;
  unsigned long addr;

  addr = htonl(ip_addr);
  if (addr == 0UL) {
    /* Catch this as a special case that will never resolve to
       anything */
    strcpy(buffer, "0.0.0.0");
    return buffer;
  } else {
    host = gethostbyaddr((const char *) &addr, sizeof(ip_addr), AF_INET);
    if (!host) {
      a = (ip_addr >> 24) & 0xff;
      b = (ip_addr >> 16) & 0xff;
      c = (ip_addr >>  8) & 0xff;
      d = (ip_addr)       & 0xff;
      snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", a, b, c, d);
      return buffer;
    } else {
      return host->h_name;
    }
  }
}

/* ================================================== */

