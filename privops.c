/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Bryan Christianson 2015
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

  Perform privileged operations over a unix socket to a privileged fork.
  */

#include "config.h"

#include "sysincl.h"

#include "conf.h"
#include "logging.h"
#include "privops.h"
#include "util.h"

#define op_ADJTIME        1024
#define op_SETTIMEOFDAY   1025
#define op_BINDSOCKET     1026
#define op_QUIT           1099

union sockaddr_in46 {
  struct sockaddr_in in4;
#ifdef FEAT_IPV6
  struct sockaddr_in6 in6;
#endif
  struct sockaddr u;
};

/* daemon request structs */

typedef struct {
  struct timeval tv;
} ReqAdjustTime;

typedef struct {
  struct timeval tv;
} ReqSetTime;

typedef struct {
  int sock;
  socklen_t sa_len;
  union sockaddr_in46 sa;
} ReqBindSocket;

typedef struct {
  int op;
  union {
    ReqAdjustTime adj_tv;
    ReqSetTime settime_tv;
    ReqBindSocket bind_sock;
  } u;
} PrvRequest;

/* helper response structs */

typedef struct {
  struct timeval tv;
} ResAdjustTime;

typedef struct {
  char msg[256];
} ResFatalMsg;

typedef struct {
  int fatal_error;
  int rc;
  int res_errno;
  union {
    ResFatalMsg fatal_msg;
    ResAdjustTime adj_tv;
  } u;
} PrvResponse;

static int helper_fd;
static pid_t helper_pid;

static int
have_helper(void)
{
  return helper_fd >= 0;
}

/* ======================================================================= */

/* HELPER - prepare fatal error for daemon */
static void
res_fatal(PrvResponse *res, const char *fmt, ...)
{
  va_list ap;

  res->fatal_error = 1;
  va_start(ap, fmt);
  vsnprintf(res->u.fatal_msg.msg, sizeof (res->u.fatal_msg.msg), fmt, ap);
  va_end(ap);
}

/* ======================================================================= */

/* HELPER - send response to the fd */

static int
send_response(int fd, const PrvResponse *res)
{
  if (send(fd, res, sizeof (*res), 0) != sizeof (*res))
    return 0;

  return 1;
}

/* ======================================================================= */
/* receive daemon request plus optional file descriptor over a unix socket */

static int
receive_from_daemon(int fd, PrvRequest *req)
{
  struct msghdr msg;
  struct cmsghdr *cmsg;
  struct iovec iov;
  char cmsgbuf[256];

  iov.iov_base = req;
  iov.iov_len = sizeof (*req);

  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = (void *)cmsgbuf;
  msg.msg_controllen = sizeof (cmsgbuf);
  msg.msg_flags = MSG_WAITALL;

  /* read the data */
  if (recvmsg(fd, &msg, 0) != sizeof (*req))
    return 0;

  if (req->op == op_BINDSOCKET) {
    /* extract transferred descriptor */
    req->u.bind_sock.sock = -1;
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
        memcpy(&req->u.bind_sock.sock, CMSG_DATA(cmsg), sizeof (int));
    }

    /* return error if valid descriptor not found */
    if (req->u.bind_sock.sock < 0)
      return 0;
  }

  return 1;
}

/* ======================================================================= */

/* HELPER - perform adjtime() */

#ifdef PRIVOPS_ADJUSTTIME
static void
do_adjtime(const ReqAdjustTime *req, PrvResponse *res)
{
  res->rc = adjtime(&req->tv, &res->u.adj_tv.tv);
  if (res->rc)
    res->res_errno = errno;
}
#endif

/* ======================================================================= */

/* HELPER - perform settimeofday() */

static void
do_settimeofday(const ReqSetTime *req, PrvResponse *res)
{
  res->rc = settimeofday(&req->tv, NULL);
  if (res->rc)
    res->res_errno = errno;
}

/* ======================================================================= */

/* HELPER - bind port to a socket */

static void
do_bindsocket(ReqBindSocket *req, PrvResponse *res)
{
  unsigned short port;
  IPAddr ip;
  int sock_fd;
  struct sockaddr *sa;
  socklen_t sa_len;

  sa = &req->sa.u;
  sa_len = req->sa_len;
  sock_fd = req->sock;

  UTI_SockaddrToIPAndPort(sa, &ip, &port);
  if (port && port != CNF_GetNTPPort()) {
    close(sock_fd);
    res_fatal(res, "Invalid port %d", port);
    return;
  }

  res->rc = bind(sock_fd, sa, sa_len);
  if (res->rc)
    res->res_errno = errno;

  /* sock is still open on daemon side, but we're done with it in the helper */
  close(sock_fd);
}

/* ======================================================================= */

/* HELPER - main loop - action requests from the daemon */

static void
helper_main(int fd)
{
  PrvRequest req;
  PrvResponse res;
  int quit = 0;

  while (!quit) {
    if (!receive_from_daemon(fd, &req))
      /* read error or closed input - we cannot recover - give up */
      break;

    memset(&res, 0, sizeof (res));

    switch (req.op) {
#ifdef PRIVOPS_ADJUSTTIME
      case op_ADJTIME:
        do_adjtime(&req.u.adj_tv, &res);
        break;
#endif
      case op_SETTIMEOFDAY:
        do_settimeofday(&req.u.settime_tv, &res);
        break;

      case op_BINDSOCKET:
        do_bindsocket(&req.u.bind_sock, &res);
        break;

      case op_QUIT:
        quit = 1;
        continue;

      default:
        res_fatal(&res, "Unexpected operator %d", req.op);
        break;
    }

    send_response(fd, &res);
  }

  close(fd);
  exit(0);
}

/* ======================================================================= */

/* DAEMON - read helper response */

static int
read_response(PrvResponse *res)
{
  int resp_len;

  resp_len = recv(helper_fd, res, sizeof (*res), 0);
  if (resp_len < 0)
    LOG_FATAL(LOGF_PrivOps, "Could not read from helper : %s", strerror(errno));
  if (resp_len != sizeof (*res))
    LOG_FATAL(LOGF_PrivOps, "Invalid helper response");

  if (res->fatal_error)
    LOG_FATAL(LOGF_PrivOps, "Error in helper : %s", res->u.fatal_msg.msg);

  DEBUG_LOG(LOGF_PrivOps, "Received response rc=%d", res->rc);

  /* if operation failed in the helper, set errno so daemon can print log message */
  if (res->rc) {
    errno = res->res_errno;
    return 0;
  }

  return 1;
}

/* ======================================================================= */

/* DAEMON - send daemon request to the helper */

static void
send_request(PrvRequest *req)
{
  struct msghdr msg;
  struct iovec iov;
  char cmsgbuf[256];

  iov.iov_base = req;
  iov.iov_len = sizeof (*req);

  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;

  if (req->op == op_BINDSOCKET) {
    /* send file descriptor as a control message */
    struct cmsghdr *cmsg;
    int *ptr_send_fd;

    msg.msg_control = cmsgbuf;
    msg.msg_controllen = CMSG_SPACE(sizeof (int));

    cmsg = CMSG_FIRSTHDR(&msg);
    memset(cmsg, 0, CMSG_SPACE(sizeof (int)));

    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof (int));

    ptr_send_fd = (int *)CMSG_DATA(cmsg);
    *ptr_send_fd = req->u.bind_sock.sock;
  }

  if (sendmsg(helper_fd, &msg, 0) < 0) {
    /* don't try to send another request from exit() */
    helper_fd = -1;
    LOG_FATAL(LOGF_PrivOps, "Could not send to helper : %s", strerror(errno));
  }

  DEBUG_LOG(LOGF_PrivOps, "Sent request op=%d", req->op);
}

/* ======================================================================= */

/* DAEMON - send daemon request and wait for response */

static int
submit_request(PrvRequest *req, PrvResponse *res)
{
  send_request(req);
  return read_response(res);
}

/* ======================================================================= */

/* DAEMON - send the helper a request to exit and wait until it exits */

static void
stop_helper(void)
{
  PrvRequest req;
  int status;

  if (!have_helper())
    return;

  memset(&req, 0, sizeof (req));
  req.op = op_QUIT;
  send_request(&req);

  waitpid(helper_pid, &status, 0);
}

/* ======================================================================= */

/* DAEMON - request adjtime() */

#ifdef PRIVOPS_ADJUSTTIME
int
PRV_AdjustTime(const struct timeval *delta, struct timeval *olddelta)
{
  PrvRequest req;
  PrvResponse res;

  if (!have_helper() || delta == NULL)
    /* helper is not running or read adjustment call */
    return adjtime(delta, olddelta);

  memset(&req, 0, sizeof (req));
  req.op = op_ADJTIME;
  req.u.adj_tv.tv = *delta;

  if (!submit_request(&req, &res))
    return -1;

  if (olddelta)
    *olddelta = res.u.adj_tv.tv;

  return 0;
}
#endif

/* ======================================================================= */

/* DAEMON - request settimeofday() */

#ifdef PRIVOPS_SETTIME
int
PRV_SetTime(const struct timeval *tp, const struct timezone *tzp)
{
  PrvRequest req;
  PrvResponse res;

  /* only support setting the time */
  assert(tp != NULL);
  assert(tzp == NULL);

  if (!have_helper())
    return settimeofday(tp, NULL);

  memset(&req, 0, sizeof (req));
  req.op = op_SETTIMEOFDAY;
  req.u.settime_tv.tv = *tp;

  if (!submit_request(&req, &res))
    return -1;

  return 0;
}
#endif

/* ======================================================================= */

/* DAEMON - bind socket to reserved port */

#ifdef PRIVOPS_BINDSOCKET
int
PRV_BindSocket(int sock, struct sockaddr *address, socklen_t address_len)
{
  PrvRequest req;
  PrvResponse res;
  IPAddr ip;
  unsigned short port;

  UTI_SockaddrToIPAndPort(address, &ip, &port);
  assert(!port || port == CNF_GetNTPPort());

  if (!have_helper())
    return bind(sock, address, address_len);

  memset(&req, 0, sizeof (req));
  req.op = op_BINDSOCKET;
  req.u.bind_sock.sock = sock;
  req.u.bind_sock.sa_len = address_len;
  memcpy(&req.u.bind_sock.sa.u, address, address_len);

  if (!submit_request(&req, &res))
    return -1;

  return 0;
}
#endif

/* ======================================================================= */

void
PRV_Initialise(void)
{
  helper_fd = -1;
}

/* ======================================================================= */

/* DAEMON - setup socket(s) then fork to run the helper */
/* must be called before privileges are dropped */

void
PRV_StartHelper(void)
{
  pid_t pid;
  int fd, sock_pair[2];

  if (have_helper())
    LOG_FATAL(LOGF_PrivOps, "Helper already running");

  if (
#ifdef SOCK_SEQPACKET
      socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sock_pair) &&
#endif
      socketpair(AF_UNIX, SOCK_DGRAM, 0, sock_pair))
    LOG_FATAL(LOGF_PrivOps, "socketpair() failed : %s", strerror(errno));

  UTI_FdSetCloexec(sock_pair[0]);
  UTI_FdSetCloexec(sock_pair[1]);

  pid = fork();
  if (pid < 0)
    LOG_FATAL(LOGF_PrivOps, "fork() failed : %s", strerror(errno));

  if (pid == 0) {
    /* child process */
    close(sock_pair[0]);

    /* close other descriptors inherited from the parent process */
    for (fd = 0; fd < 1024; fd++) {
      if (fd != sock_pair[1])
        close(fd);
    }

    helper_main(sock_pair[1]);

  } else {
    /* parent process */
    close(sock_pair[1]);
    helper_fd = sock_pair[0];
    helper_pid = pid;

    /* stop the helper even when not exiting cleanly from the main function */
    atexit(stop_helper);
  }
}

/* ======================================================================= */

/* DAEMON - graceful shutdown of the helper */

void
PRV_Finalise(void)
{
  if (!have_helper())
    return;

  stop_helper();
  close(helper_fd);
  helper_fd = -1;
}
