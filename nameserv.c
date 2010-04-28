/*
  $Header: /cvs/src/chrony/nameserv.c,v 1.15 2003/09/22 21:22:30 richard Exp $

  =======================================================================

  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2009
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

  Functions to do name to IP address conversion

  */

#include "sysincl.h"

#include "nameserv.h"
#include "util.h"
#include <resolv.h>

/* ================================================== */

static int address_family = IPADDR_UNSPEC;

void
DNS_SetAddressFamily(int family)
{
  address_family = family;
}

DNS_Status 
DNS_Name2IPAddress(const char *name, IPAddr *addr)
{
#ifdef HAVE_IPV6
  struct addrinfo hints, *res, *ai;
  int result;
  
  memset(&hints, 0, sizeof (hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  result = getaddrinfo(name, NULL, &hints, &res);

  if (result) {
    return result == EAI_AGAIN ? DNS_TryAgain : DNS_Failure;
  }

  for (ai = res; !result && ai != NULL; ai = ai->ai_next) {
    switch (ai->ai_family) {
      case AF_INET:
        addr->family = IPADDR_INET4;
        addr->addr.in4 = ntohl(((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr);
        result = 1;
        break;
#ifdef HAVE_IPV6
      case AF_INET6:
        addr->family = IPADDR_INET6;
        memcpy(&addr->addr.in6, &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr.s6_addr, sizeof (addr->addr.in6));
        result = 1;
        break;
#endif
    }
    if (result && address_family != IPADDR_UNSPEC && address_family != addr->family)
      result = 0;
  }

  freeaddrinfo(res);
  return result ? DNS_Success : DNS_Failure;
#else
  struct hostent *host;
  
  host = gethostbyname(name);

  if (host == NULL) {
    if (h_errno == TRY_AGAIN)
      return DNS_TryAgain;
  } else {
    addr->family = IPADDR_INET4;
    addr->addr.in4 = ntohl(*(uint32_t *)host->h_addr_list[0]);
    return DNS_Success;
  }

  return DNS_Failure;
#endif
}

/* ================================================== */

int
DNS_IPAddress2Name(IPAddr *ip_addr, char *name, int len)
{
  char *result = NULL;

#ifdef HAVE_IPV6
  struct sockaddr_in in4;
  struct sockaddr_in6 in6;
  char hbuf[NI_MAXHOST];

  switch (ip_addr->family) {
    case IPADDR_INET4:
      memset(&in4, 0, sizeof (in4));
#ifdef SIN6_LEN
      in4.sin_len = sizeof (in4);
#endif
      in4.sin_family = AF_INET;
      in4.sin_addr.s_addr = htonl(ip_addr->addr.in4);
      if (!getnameinfo((const struct sockaddr *)&in4, sizeof (in4), hbuf, sizeof (hbuf), NULL, 0, 0))
        result = hbuf;
      break;
    case IPADDR_INET6:
      memset(&in6, 0, sizeof (in6));
#ifdef SIN6_LEN
      in6.sin6_len = sizeof (in6);
#endif
      in6.sin6_family = AF_INET6;
      memcpy(&in6.sin6_addr.s6_addr, ip_addr->addr.in6, sizeof (in6.sin6_addr.s6_addr));
      if (!getnameinfo((const struct sockaddr *)&in6, sizeof (in6), hbuf, sizeof (hbuf), NULL, 0, 0))
        result = hbuf;
      break;
  }
#else
  struct hostent *host;
  uint32_t addr;

  switch (ip_addr->family) {
    case IPADDR_INET4:
      addr = htonl(ip_addr->addr.in4);
      host = gethostbyaddr((const char *) &addr, sizeof (ip_addr), AF_INET);
      break;
#ifdef HAVE_IPV6
    case IPADDR_INET6:
      host = gethostbyaddr((const void *) ip_addr->addr.in6, sizeof (ip_addr->addr.in6), AF_INET6);
      break;
#endif
    default:
      host = NULL;
  }
  if (host)
    result = host->h_name;
#endif

  if (result == NULL)
    result = UTI_IPToString(ip_addr);
  if (snprintf(name, len, "%s", result) >= len)
    return 0;

  return 1;
}

/* ================================================== */

void
DNS_Reload(void)
{
  res_init();
}

/* ================================================== */

