/*
  $Header: /cvs/src/chrony/sys.c,v 1.11 2002/02/28 23:27:14 richard Exp $

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

  This file contains all the conditionally compiled bits that pull
  in the various operating-system specific modules
  */

#include "sys.h"

#if defined (LINUX)
#include "sys_linux.h"
#endif

#if defined (SOLARIS)
#include "sys_solaris.h"
#endif

#if defined (SUNOS)
#include "sys_sunos.h"
#endif

#if defined (__NetBSD__)
#include "sys_netbsd.h"
#endif

/* ================================================== */

void
SYS_Initialise(void)
{

#if defined(LINUX)
  SYS_Linux_Initialise();
#endif

#if defined(SOLARIS)
  SYS_Solaris_Initialise();
#endif

#if defined(SUNOS)
  SYS_SunOS_Initialise();
#endif

#if defined(__NetBSD__)
  SYS_NetBSD_Initialise();
#endif

}

/* ================================================== */

void
SYS_Finalise(void)
{
  
#if defined(LINUX)
  SYS_Linux_Finalise();
#endif

#if defined(SOLARIS)
  SYS_Solaris_Finalise();
#endif

#if defined(SUNOS)
  SYS_SunOS_Finalise();
#endif

#if defined(__NetBSD__)
  SYS_NetBSD_Finalise();
#endif

  return;
}

/* ================================================== */
/* ================================================== */
/* ================================================== */



