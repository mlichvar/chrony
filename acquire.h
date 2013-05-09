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

  Header file for acquisition module
  */

#ifndef GOT_ACQUIRE_H
#define GOT_ACQUIRE_H

#include "addressing.h"

typedef struct ACQ_SourceRecord *ACQ_Source;

extern void ACQ_Initialise(void);

extern void ACQ_Finalise(void);

extern void ACQ_StartAcquisition(int n, IPAddr *ip_addrs, double init_slew_threshold,
                                 void (*after_hook)(void *), void *anything);

extern void ACQ_AccumulateSample(ACQ_Source acq_source, double offset, double root_distance);

extern void ACQ_MissedSample(ACQ_Source acq_source);

#endif /* GOT_ACQUIRE_H */
