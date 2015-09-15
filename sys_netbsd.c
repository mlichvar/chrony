/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2001
 * Copyright (C) J. Hannken-Illjes  2001
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

  Driver file for the NetBSD operating system.
  */

#include "config.h"

#include "sysincl.h"

#include "sys_netbsd.h"
#include "sys_timex.h"
#include "logging.h"

/* ================================================== */

void
SYS_NetBSD_Initialise(void)
{
  SYS_Timex_Initialise();
}

/* ================================================== */

void
SYS_NetBSD_Finalise(void)
{
  SYS_Timex_Finalise();
}

/* ================================================== */

#ifdef FEAT_PRIVDROP
void
SYS_NetBSD_DropRoot(uid_t uid, gid_t gid)
{
  int fd;

  if (setgroups(0, NULL))
    LOG_FATAL(LOGF_SysNetBSD, "setgroups() failed : %s", strerror(errno));

  if (setgid(gid))
    LOG_FATAL(LOGF_SysNetBSD, "setgid(%d) failed : %s", gid, strerror(errno));

  if (setuid(uid))
    LOG_FATAL(LOGF_SysNetBSD, "setuid(%d) failed : %s", uid, strerror(errno));

  DEBUG_LOG(LOGF_SysNetBSD, "Root dropped to uid %d gid %d", uid, gid);

  /* Check if we have write access to /dev/clockctl */
  fd = open("/dev/clockctl", O_WRONLY);
  if (fd < 0)
    LOG_FATAL(LOGF_SysNetBSD, "Can't write to /dev/clockctl");
  close(fd);
}
#endif
