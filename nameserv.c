/*
  $Header: /cvs/src/chrony/nameserv.c,v 1.15 2003/09/22 21:22:30 richard Exp $

  =======================================================================

  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
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
#include <resolv.h>

/* ================================================== */

static unsigned int retries = 0;

static unsigned long
Name2IPAddress(const char *name, int retry)
{
  struct hostent *host;
  unsigned char *address0;
  unsigned long result;

try_again:
  host = gethostbyname(name);
  if (host == NULL) {
    if (retry && h_errno == TRY_AGAIN && retries < 10) {
      sleep(2 << retries);
      retries++;
      res_init();
      goto try_again;
    }
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

unsigned long
DNS_Name2IPAddress(const char *name)
{
  return Name2IPAddress(name, 0);
}

/* ================================================== */

unsigned long
DNS_Name2IPAddressRetry(const char *name)
{
  return Name2IPAddress(name, 1);
}

/* ================================================== */

const char *
DNS_IPAddress2Name(unsigned long ip_addr)
{
  struct hostent *host;
  static char buffer[16];
  unsigned int a, b, c, d;
  uint32_t addr;

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

