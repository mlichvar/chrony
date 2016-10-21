/*
 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2016
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
 */

#include <hwclock.c>
#include "test.h"

void
test_unit(void)
{
  struct timespec start_hw_ts, start_local_ts, hw_ts, local_ts, ts;
  HCL_Instance clock;
  double freq, jitter, interval, d;
  int i, j;

  LCL_Initialise();

  clock = HCL_CreateInstance();

  for (i = 0; i < 2000; i++) {
    UTI_ZeroTimespec(&start_hw_ts);
    UTI_ZeroTimespec(&start_local_ts);
    UTI_AddDoubleToTimespec(&start_hw_ts, TST_GetRandomDouble(0.0, 1e9), &start_hw_ts);
    UTI_AddDoubleToTimespec(&start_local_ts, TST_GetRandomDouble(0.0, 1e9), &start_local_ts);

    DEBUG_LOG(0, "iteration %d", i);

    freq = TST_GetRandomDouble(0.9, 1.1);
    jitter = TST_GetRandomDouble(10.0e-9, 1000.0e-9);
    interval = TST_GetRandomDouble(MIN_SAMPLE_SEPARATION / 10, MIN_SAMPLE_SEPARATION * 10.0);

    clock->n_samples = 0;
    clock->valid_coefs = 0;

    for (j = 0; j < 100; j++) {
      UTI_AddDoubleToTimespec(&start_hw_ts, j * interval * freq + TST_GetRandomDouble(-jitter, jitter), &hw_ts);
      UTI_AddDoubleToTimespec(&start_local_ts, j * interval, &local_ts);
      if (HCL_CookTime(clock, &hw_ts, &ts, NULL)) {
        d = UTI_DiffTimespecsToDouble(&ts, &local_ts);
        TEST_CHECK(fabs(d) <= 5.0 * jitter);
      }

      if (HCL_NeedsNewSample(clock, &local_ts))
        HCL_AccumulateSample(clock, &hw_ts, &local_ts, 2.0 * jitter);

      TEST_CHECK(j < 20 || clock->valid_coefs);

      if (!clock->valid_coefs)
        continue;

      TEST_CHECK(fabs(clock->offset) <= 2.0 * jitter);
    }
  }

  LCL_Finalise();
}
