/*
  $Header: /cvs/src/chrony/conf.h,v 1.25 2003/09/22 21:22:30 richard Exp $

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
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 **********************************************************************

  =======================================================================

  Header file for configuration module
  */

#ifndef GOT_CONF_H
#define GOT_CONF_H

#include "addressing.h"

extern char *CNF_GetRtcDevice(void);

extern void CNF_ReadFile(const char *filename);

extern void CNF_AddSources(void);
extern void CNF_AddBroadcasts(void);
extern void CNF_AddRefclocks(void);

extern void CNF_ProcessInitStepSlew(void (*after_hook)(void *), void *anything);

extern unsigned short CNF_GetAcquisitionPort(void);
extern unsigned short CNF_GetNTPPort(void);
extern char *CNF_GetDriftFile(void);
extern char *CNF_GetLogDir(void);
extern char *CNF_GetDumpDir(void);
extern int CNF_GetLogMeasurements(void);
extern int CNF_GetLogStatistics(void);
extern int CNF_GetLogTracking(void);
extern int CNF_GetLogRtc(void);
extern char *CNF_GetKeysFile(void);
extern char *CNF_GetRtcFile(void);
extern unsigned long CNF_GetCommandKey(void);
extern int CNF_GetDumpOnExit(void);
extern int CNF_GetManualEnabled(void);
extern int CNF_GetCommandPort(void);
extern int CNF_GetRTCOnUTC(void);
extern void CNF_GetLogChange(int *enabled, double *threshold);
extern void CNF_GetMailOnChange(int *enabled, double *threshold, char **user);
extern int CNF_GetNoClientLog(void);
extern unsigned long CNF_GetClientLogLimit(void);
extern void CNF_GetBindAddress(int family, IPAddr *addr);
extern void CNF_GetBindCommandAddress(int family, IPAddr *addr);
extern char *CNF_GetPidFile(void);
extern void CNF_GetLinuxHz(int *set, int *hz);
extern void CNF_GetLinuxFreqScale(int *set, double *freq_scale);

/* Value returned in ppm, as read from file */
extern double CNF_GetMaxUpdateSkew(void);
extern int CNF_AllowLocalReference(int *stratum);

extern void CNF_SetupAccessRestrictions(void);

#if defined(HAVE_SCHED_SETSCHEDULER)
extern int SchedPriority;
#endif

#if defined(HAVE_MLOCKALL)
extern int LockAll;
#endif

#endif /* GOT_CONF_H */
