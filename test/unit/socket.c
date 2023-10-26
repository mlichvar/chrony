/*
 **********************************************************************
 * Copyright (C) Luke Valenta  2023
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

#include <socket.c>
#include "test.h"

static void
test_preinitialise(void)
{
#ifdef LINUX
  /* Test LISTEN_FDS environment variable parsing */

  /* normal */
  putenv("LISTEN_FDS=2");
  SCK_PreInitialise();
  TEST_CHECK(reusable_fds == 2);

  /* negative */
  putenv("LISTEN_FDS=-2");
  SCK_PreInitialise();
  TEST_CHECK(reusable_fds == 0);

  /* trailing characters */
  putenv("LISTEN_FDS=2a");
  SCK_PreInitialise();
  TEST_CHECK(reusable_fds == 0);

  /* non-integer */
  putenv("LISTEN_FDS=a2");
  SCK_PreInitialise();
  TEST_CHECK(reusable_fds == 0);

  /* not set */
  unsetenv("LISTEN_FDS");
  SCK_PreInitialise();
  TEST_CHECK(reusable_fds == 0);
#endif
}

void
test_unit(void)
{
  test_preinitialise();
}
