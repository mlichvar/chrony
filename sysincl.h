/*
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
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 **********************************************************************

  =======================================================================

  This file includes all system header files that the software
  requires.  This allows us to isolate system dependencies to this file
  alone.
  */

#ifndef GOT_SYSINCL_H
#define GOT_SYSINCL_H

#if defined (SOLARIS) || defined(SUNOS) || defined(LINUX) || defined(NETBSD) || defined (MACOSX)

#if !defined(__NetBSD__) && !defined(__FreeBSD__) && !defined(MACOSX)
#include <alloca.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <glob.h>
#if !defined(__FreeBSD__) && !defined(MACOSX)
#include <malloc.h>
#endif
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pwd.h>
#include <resolv.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/shm.h>
#include <syslog.h>
#include <time.h>

#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#elif HAVE_STDINT_H
#include <stdint.h>
#else
/* Tough */
#endif

/* One or other of these to make getsid() visible */
#define __EXTENSIONS__ 1
#define __USE_XOPEN_EXTENDED 1

#include <unistd.h>

#endif

#ifdef FEAT_IPV6
/* For inet_ntop() */
#include <arpa/inet.h>
#endif

#if defined (SOLARIS) || defined(SUNOS)
/* Only needed on these platforms, and doesn't exist on some Linux
   versions. */
#include <nlist.h>
#endif

#endif /* GOT_SYSINCL_H */
