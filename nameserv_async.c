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

  Functions to asynchronously convert name to IP address

  */

#include "config.h"
#include "sysincl.h"

#include "nameserv_async.h"
#include "logging.h"
#include "util.h"

/* ================================================== */

/* This is a blocking implementation used when nothing else is available */

void
DNS_Name2IPAddressAsync(const char *name, DNS_NameResolveHandler handler, void *anything)
{
  IPAddr addr;
  DNS_Status status;

  status = DNS_Name2IPAddress(name, &addr);
  (handler)(status, &addr, anything);
}

/* ================================================== */
