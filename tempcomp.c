/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2011, 2014
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

  Routines implementing temperature compensation.

  */

#include "config.h"

#include "conf.h"
#include "local.h"
#include "memory.h"
#include "util.h"
#include "logging.h"
#include "sched.h"
#include "tempcomp.h"

/* Sanity limit (in ppm) */
#define MAX_COMP 10.0

static SCH_TimeoutID timeout_id;

static LOG_FileID logfileid;

static char *filename;
static double update_interval;
static double T0, k0, k1, k2;

static void
read_timeout(void *arg)
{
  FILE *f;
  double temp, comp;

  f = fopen(filename, "r");

  if (f && fscanf(f, "%lf", &temp) == 1) {
    comp = k0 + (temp - T0) * k1 + (temp - T0) * (temp - T0) * k2;

    if (fabs(comp) <= MAX_COMP) {
      comp = LCL_SetTempComp(comp);

      if (logfileid != -1) {
        struct timeval now;

        LCL_ReadCookedTime(&now, NULL);
        LOG_FileWrite(logfileid, "%s %11.4e %11.4e",
            UTI_TimeToLogForm(now.tv_sec), temp, comp);
      }
    } else {
      LOG(LOGS_WARN, LOGF_TempComp,
          "Temperature compensation of %.3f ppm exceeds sanity limit of %.1f",
          comp, MAX_COMP);
    }
  } else {
    LOG(LOGS_WARN, LOGF_TempComp, "Could not read temperature from %s",
        filename);
  }

  if (f)
    fclose(f);

  timeout_id = SCH_AddTimeoutByDelay(update_interval, read_timeout, NULL);
}

void
TMC_Initialise(void)
{
  CNF_GetTempComp(&filename, &update_interval, &T0, &k0, &k1, &k2);

  if (filename == NULL)
    return;

  if (update_interval <= 0.0)
    update_interval = 1.0;

  logfileid = CNF_GetLogTempComp() ? LOG_FileOpen("tempcomp",
      "   Date (UTC) Time        Temp.       Comp.")
    : -1;

  read_timeout(NULL);
}

void
TMC_Finalise(void)
{
  if (filename == NULL)
    return;

  SCH_RemoveTimeout(timeout_id);
}
