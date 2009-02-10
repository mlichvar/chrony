/*
  $Header: /cvs/src/chrony/sys.h,v 1.7 2002/02/28 23:27:14 richard Exp $

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

  This is the header for the file that links in the operating system-
  specific parts of the software

*/

#ifndef GOT_SYS_H
#define GOT_SYS_H

/* Called at the start of the run to do initialisation */
extern void SYS_Initialise(void);

/* Called at the end of the run to do final clean-up */
extern void SYS_Finalise(void);

/* Drop root privileges to the specified user */
extern void SYS_DropRoot(char *user);

extern void SYS_SetScheduler(int SchedPriority);
extern void SYS_MemLockAll(int LockAll);

#endif /* GOT_SYS_H */
