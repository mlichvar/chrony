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
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
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

#define SOCK_MAGIC 0x534f434b

struct sock_sample {
  struct timeval tv;
  double offset;
  int pulse;
  int leap;
  int _pad;
  int magic;
};

static void read_sample(void *anything)
{
  struct sock_sample sample;
  RCL_Instance instance;
  int sockfd, s;

  instance = (RCL_Instance)anything;
  sockfd = (long)RCL_GetDriverData(instance);

  s = recv(sockfd, &sample, sizeof (sample), 0);

  if (s < 0) {
#if 0
    LOG(LOGS_INFO, LOGF_Refclock, "Error reading from SOCK socket : %s", strerror(errno));
#endif
    return;
  }

  if (s != sizeof (sample)) {
#if 0
    LOG(LOGS_INFO, LOGF_Refclock, "Unexpected length of SOCK sample : %d != %d", s, sizeof (sample));
#endif
    return;
  }

  if (sample.magic != SOCK_MAGIC) {
#if 0
    LOG(LOGS_INFO, LOGF_Refclock, "Unexpected magic number in SOCK sample : %x != %x", sample.magic, SOCK_MAGIC);
#endif
    return;
  }

  if (sample.pulse) {
    RCL_AddPulse(instance, &sample.tv, sample.offset);
  } else {
    RCL_AddSample(instance, &sample.tv, sample.offset, sample.leap);
  }
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
  int sockfd;

  sockfd = (long)RCL_GetDriverData(instance);
  SCH_RemoveInputFileHandler(sockfd);
  close(sockfd);
}

RefclockDriver RCL_SOCK_driver = {
  sock_initialise,
  sock_finalise,
  NULL
};
