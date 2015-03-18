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

  The header file for the adjtimex wrapper
  */

#ifndef GOT_WRAP_ADJTIMEX_H
#define GOT_WRAP_ADJTIMEX_H

int TMX_ResetOffset(void);
int TMX_SetFrequency(double *freq, long tick);
int TMX_GetFrequency(double *freq, long *tick);
int TMX_SetLeap(int leap);
int TMX_GetLeap(int *leap);
int TMX_SetSync(int sync, double est_error, double max_error);
int TMX_TestStepOffset(void);
int TMX_ApplyStepOffset(double offset);

#endif  /* GOT_WRAP_ADJTIMEX_H */

