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

  Header file for the command parser
  */

#ifndef GOT_CMDPARSE_H
#define GOT_CMDPARSE_H

#include "srcparams.h"
#include "addressing.h"

typedef enum {
  CPS_Success,
  CPS_InvalidValue,
  CPS_InvalidOption,
  CPS_MissingArgument,
} CPS_Status;

typedef struct {
  char *name;
  int family;
  int port;
  SourceParameters params;
} CPS_NTP_Source;

/* Parse a command to add an NTP server or peer */
extern CPS_Status CPS_ParseNTPSourceAdd(char *line, CPS_NTP_Source *src);
  
/* Get an NTP/refclock select option */
extern int CPS_GetSelectOption(char *option);

/* Parse a command to allow/deny access */
extern int CPS_ParseAllowDeny(char *line, int *all, IPAddr *ip, int *subnet_bits);

/* Parse a command to enable local reference */
extern CPS_Status CPS_ParseLocal(char *line, int *stratum, int *orphan, double *distance,
                                 double *activate, double *wait_synced, double *wait_unsynced);

/* Remove extra white-space and comments */
extern void CPS_NormalizeLine(char *line);

/* Terminate first word and return pointer to the next word */
extern char *CPS_SplitWord(char *line);

/* Parse a key from keyfile */
extern int CPS_ParseKey(char *line, uint32_t *id, const char **type, char **key);

/* Parse a refclock reference ID (returns number of characters) */
extern int CPS_ParseRefid(char *line, uint32_t *ref_id);

#endif /* GOT_CMDPARSE_H */
