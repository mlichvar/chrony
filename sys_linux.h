/*
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
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 **********************************************************************

  =======================================================================

  The header file for the linux driver
  */

#ifndef GOT_SYS_LINUX_H
#define GOT_SYS_LINUX_H

extern void SYS_Linux_Initialise(void);

extern void SYS_Linux_Finalise(void);

extern void SYS_Linux_GetKernelVersion(int *major, int *minor, int *patchlevel);

extern void SYS_Linux_DropRoot(char *user);

extern void SYS_Linux_MemLockAll(int LockAll);

extern void SYS_Linux_SetScheduler(int SchedPriority);

#endif  /* GOT_SYS_LINUX_H */
