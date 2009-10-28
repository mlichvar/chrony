/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2009
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

  Unix domain socket refclock driver.

  */

#include "refclock.h"
#include "logging.h"
#include "util.h"
#include "sched.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct sock_sample {
  struct timeval tv;
  double offset;
  int leap;
};

static void read_sample(void *anything)
{
  struct sock_sample sample;
  RCL_Instance instance;
  int sockfd;

  instance = (RCL_Instance)anything;
  sockfd = (long)RCL_GetDriverData(instance);

  if (recv(sockfd, &sample, sizeof (sample), 0) != sizeof (sample))
    return;

  RCL_AddSample(instance, &sample.tv, sample.offset, sample.leap);
}

static int sock_initialise(RCL_Instance instance)
{
  struct sockaddr_un s;
  int sockfd;
  char *path;

  path = RCL_GetDriverParameter(instance);
 
  s.sun_family = AF_UNIX;
  if (snprintf(s.sun_path, sizeof (s.sun_path), "%s", path) >= sizeof (s.sun_path)) {
    LOG_FATAL(LOGF_Refclock, "path %s is too long", path);
    return 0;
  }

  sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    LOG_FATAL(LOGF_Refclock, "socket() failed");
    return 0;
  }

  unlink(path);
  if (bind(sockfd, (struct sockaddr *)&s, sizeof (s)) < 0) {
    LOG_FATAL(LOGF_Refclock, "bind() failed");
    return 0;
  }

  RCL_SetDriverData(instance, (void *)(long)sockfd);
  SCH_AddInputFileHandler(sockfd, read_sample, instance);
  return 1;
}

static void sock_finalise(RCL_Instance instance)
{
  SCH_RemoveInputFileHandler((long)RCL_GetDriverData(instance));
}

RefclockDriver RCL_SOCK_driver = {
  sock_initialise,
  sock_finalise,
  NULL
};
