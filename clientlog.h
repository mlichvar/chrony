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

  This module contains facilities for logging access by clients.

  */

#ifndef GOT_CLIENTLOG_H
#define GOT_CLIENTLOG_H

#include "sysincl.h"
#include "reports.h"

extern void CLG_Initialise(void);
extern void CLG_Finalise(void);
extern void CLG_LogNTPAccess(IPAddr *client, time_t now);
extern void CLG_LogCommandAccess(IPAddr *client, time_t now);

/* And some reporting functions, for use by chronyc. */
/* TBD */

typedef enum {
  CLG_SUCCESS,                  /* All is well */
  CLG_EMPTYSUBNET,              /* No hosts logged in requested subnet */
  CLG_BADSUBNET,                /* Subnet requested is not 0, 8, 16 or 24 bits */
  CLG_INACTIVE,                 /* Facility not active */
  CLG_INDEXTOOLARGE             /* Node index is higher than number of nodes present */
} CLG_Status;

CLG_Status
CLG_GetClientAccessReportByIndex(int index, RPT_ClientAccessByIndex_Report *report,
                                 time_t now, unsigned long *n_indices);

#endif /* GOT_CLIENTLOG_H */
