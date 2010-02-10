/*
  $Header: /cvs/src/chrony/sched.c,v 1.17 2003/09/22 21:22:30 richard Exp $

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

  This file contains the scheduling loop and the timeout queue.

  */

#include "sysincl.h"

#include "sched.h"
#include "memory.h"
#include "util.h"
#include "local.h"
#include "logging.h"

/* ================================================== */

/* Flag indicating that we are initialised */
static int initialised = 0;

/* ================================================== */

/* Variables to handle the capability to dispatch on particular file
   handles becoming readable */

/* Each bit set in this fd set corresponds to a read descriptor that
   we are watching and with which we have a handler associated in the
   file_handlers array */
static fd_set read_fds;

/* This is the number of bits that we have set in read_fds */
static unsigned int n_read_fds;

/* One more than the highest file descriptor that is registered */
static unsigned int one_highest_fd;

/* This assumes that fd_set is implemented as a fixed size array of
   bits, possibly embedded inside a record.  It might therefore
   somewhat non-portable. */

#define FD_SET_SIZE (sizeof(fd_set) * 8)

typedef struct {
  SCH_FileHandler       handler;
  SCH_ArbitraryArgument arg;
} FileHandlerEntry;

static FileHandlerEntry file_handlers[FD_SET_SIZE];

/* Last timestamp when a file descriptor became readable */
static struct timeval last_fdready;
static double last_fdready_err;

/* ================================================== */

/* Variables to handler the timer queue */

typedef struct _TimerQueueEntry
{
  struct _TimerQueueEntry *next; /* Forward and back links in the list */
  struct _TimerQueueEntry *prev;
  struct timeval tv;            /* Local system time at which the
                                   timeout is to expire.  Clearly this
                                   must be in terms of what the
                                   operating system thinks of as
                                   system time, because it will be an
                                   argument to select().  Therefore,
                                   any fudges etc that our local time
                                   driver module would apply to time
                                   that we pass to clients etc doesn't
                                   apply to this. */
  SCH_TimeoutID id;             /* ID to allow client to delete
                                   timeout */
  SCH_TimeoutClass class;       /* The class that the epoch is in */
  SCH_TimeoutHandler handler;   /* The handler routine to use */
  SCH_ArbitraryArgument arg;    /* The argument to pass to the handler */

} TimerQueueEntry;

/* The timer queue.  We only use the next and prev entries of this
   record, these chain to the real entries. */
static TimerQueueEntry timer_queue;
static unsigned long n_timer_queue_entries;
static SCH_TimeoutID next_tqe_id;

/* Pointer to head of free list */
static TimerQueueEntry *tqe_free_list = NULL;

/* ================================================== */

static int need_to_exit;

/* ================================================== */

static void
handle_slew(struct timeval *raw,
            struct timeval *cooked,
            double dfreq,
            double afreq,
            double doffset,
            int is_step_change,
            void *anything);

/* ================================================== */

void
SCH_Initialise(void)
{

  FD_ZERO(&read_fds);
  n_read_fds = 0;

  n_timer_queue_entries = 0;
  next_tqe_id = 0;

  timer_queue.next = &timer_queue;
  timer_queue.prev = &timer_queue;

  need_to_exit = 0;

  LCL_AddParameterChangeHandler(handle_slew, NULL);

  initialised = 1;

  return;
}


/* ================================================== */

void
SCH_Finalise(void) {
  initialised = 0;
  return; /* Nothing to do for now */
}

/* ================================================== */

void
SCH_AddInputFileHandler
(int fd, SCH_FileHandler handler, SCH_ArbitraryArgument arg)
{

  if (!initialised) {
    CROAK("Should be initialised");
  }
  
  /* Don't want to allow the same fd to register a handler more than
     once without deleting a previous association - this suggests
     a bug somewhere else in the program. */
  if (FD_ISSET(fd, &read_fds)) {
    CROAK("File handler already registered");
  }

  ++n_read_fds;
  
  file_handlers[fd].handler = handler;
  file_handlers[fd].arg     = arg;

  FD_SET(fd, &read_fds);

  if ((fd + 1) > one_highest_fd) {
    one_highest_fd = fd + 1;
  }

  return;
}


/* ================================================== */

void
SCH_RemoveInputFileHandler(int fd)
{
  int fds_left, fd_to_check;

  if (!initialised) {
    CROAK("Should be initialised");
  }

  /* Check that a handler was registered for the fd in question */
  if (!FD_ISSET(fd, &read_fds)) {
    CROAK("File handler not registered");
  }

  --n_read_fds;

  FD_CLR(fd, &read_fds);

  /* Find new highest file descriptor */
  fds_left = n_read_fds;
  fd_to_check = 0;
  while (fds_left > 0) {
    if (FD_ISSET(fd_to_check, &read_fds)) {
      --fds_left;
    }
    ++fd_to_check;
  }

  one_highest_fd = fd_to_check;

  return;

}

/* ================================================== */

void
SCH_GetFileReadyTime(struct timeval *tv, double *err)
{
  *tv = last_fdready;
  if (err)
    *err = last_fdready_err;
}

/* ================================================== */

#define TQE_ALLOC_QUANTUM 32

static TimerQueueEntry *
allocate_tqe(void)
{
  TimerQueueEntry *new_block;
  TimerQueueEntry *result;
  int i;
  if (tqe_free_list == NULL) {
    new_block = MallocArray(TimerQueueEntry, TQE_ALLOC_QUANTUM);
    for (i=1; i<TQE_ALLOC_QUANTUM; i++) {
      new_block[i].next = &(new_block[i-1]);
    }
    new_block[0].next = NULL;
    tqe_free_list = &(new_block[TQE_ALLOC_QUANTUM - 1]);
  }

  result = tqe_free_list;
  tqe_free_list = tqe_free_list->next;
  return result;
}

/* ================================================== */

static void
release_tqe(TimerQueueEntry *node)
{
  node->next = tqe_free_list;
  tqe_free_list = node;
  return;
}

/* ================================================== */

SCH_TimeoutID
SCH_AddTimeout(struct timeval *tv, SCH_TimeoutHandler handler, SCH_ArbitraryArgument arg)
{
  TimerQueueEntry *new_tqe;
  TimerQueueEntry *ptr;

  if (!initialised) {
    CROAK("Should be initialised");
  }

  new_tqe = allocate_tqe();

  new_tqe->id = next_tqe_id++;
  new_tqe->handler = handler;
  new_tqe->arg = arg;
  new_tqe->tv = *tv;
  new_tqe->class = SCH_ReservedTimeoutValue;

  /* Now work out where to insert the new entry in the list */
  for (ptr = timer_queue.next; ptr != &timer_queue; ptr = ptr->next) {
    if (UTI_CompareTimevals(&new_tqe->tv, &ptr->tv) == -1) {
      /* If the new entry comes before the current pointer location in
         the list, we want to insert the new entry just before ptr. */
      break;
    }
  }

  /* At this stage, we want to insert the new entry immediately before
     the entry identified by 'ptr' */

  new_tqe->next = ptr;
  new_tqe->prev = ptr->prev;
  ptr->prev->next = new_tqe;
  ptr->prev = new_tqe;

  n_timer_queue_entries++;

  return new_tqe->id;
}

/* ================================================== */
/* This queues a timeout to elapse at a given delta time relative to
   the current (raw) time */

SCH_TimeoutID
SCH_AddTimeoutByDelay(double delay, SCH_TimeoutHandler handler, SCH_ArbitraryArgument arg)
{
  struct timeval now, then;

  if (!initialised) {
    CROAK("Should be initialised");
  }

  LCL_ReadRawTime(&now);
  UTI_AddDoubleToTimeval(&now, delay, &then);
  return SCH_AddTimeout(&then, handler, arg);

}

/* ================================================== */

SCH_TimeoutID
SCH_AddTimeoutInClass(double min_delay, double separation,
                      SCH_TimeoutClass class,
                      SCH_TimeoutHandler handler, SCH_ArbitraryArgument arg)
{
  TimerQueueEntry *new_tqe;
  TimerQueueEntry *ptr;
  struct timeval now;
  double diff;
  double new_min_delay;

  if (!initialised) {
    CROAK("Should be initialised");
  }
  
  LCL_ReadRawTime(&now);
  new_min_delay = min_delay;

  /* Scan through list for entries in the same class and increase min_delay
     if necessary to keep at least the separation away */
  for (ptr = timer_queue.next; ptr != &timer_queue; ptr = ptr->next) {
    if (ptr->class == class) {
      UTI_DiffTimevalsToDouble(&diff, &ptr->tv, &now);
      if (new_min_delay > diff) {
        if (new_min_delay - diff < separation) {
          new_min_delay = diff + separation;
        }
      }
      if (new_min_delay < diff) {
        if (diff - new_min_delay < separation) {
          new_min_delay = diff + separation;
        }
      }
    }
  }

  for (ptr = timer_queue.next; ptr != &timer_queue; ptr = ptr->next) {
    UTI_DiffTimevalsToDouble(&diff, &ptr->tv, &now);
    if (diff > new_min_delay) {
      break;
    }
  }

  /* We have located the insertion point */
  new_tqe = allocate_tqe();

  new_tqe->id = next_tqe_id++;
  new_tqe->handler = handler;
  new_tqe->arg = arg;
  UTI_AddDoubleToTimeval(&now, new_min_delay, &new_tqe->tv);
  new_tqe->class = class;

  new_tqe->next = ptr;
  new_tqe->prev = ptr->prev;
  ptr->prev->next = new_tqe;
  ptr->prev = new_tqe;
  n_timer_queue_entries++;

  return new_tqe->id;
}

/* ================================================== */

void
SCH_RemoveTimeout(SCH_TimeoutID id)
{
  TimerQueueEntry *ptr;
  int ok;

  if (!initialised) {
    CROAK("Should be initialised");
  }

  ok = 0;

  for (ptr = timer_queue.next; ptr != &timer_queue; ptr = ptr->next) {

    if (ptr->id == id) {
      /* Found the required entry */
      
      /* Unlink from the queue */
      ptr->next->prev = ptr->prev;
      ptr->prev->next = ptr->next;
      
      /* Decrement entry count */
      --n_timer_queue_entries;
      
      /* Release memory back to the operating system */
      release_tqe(ptr);

      ok = 1;

      break;
    }
  }

  assert(ok);

}

/* ================================================== */
/* The current time (now) has to be passed in from the
   caller to avoid race conditions */

static int
dispatch_timeouts(struct timeval *now) {
  TimerQueueEntry *ptr;
  int n_done = 0;

  if ((n_timer_queue_entries > 0) &&
         (UTI_CompareTimevals(now, &(timer_queue.next->tv)) >= 0)) {
    ptr = timer_queue.next;

    /* Dispatch the handler */
    (ptr->handler)(ptr->arg);

    /* Increment count of timeouts handled */
    ++n_done;

    /* Unlink entry from the queue */
    ptr->prev->next = ptr->next;
    ptr->next->prev = ptr->prev;

    /* Decrement count of entries in queue */
    --n_timer_queue_entries;

    /* Delete entry */
    release_tqe(ptr);
  }

  return n_done;

}

/* ================================================== */

/* nfh is the number of bits set in fhs */

static void
dispatch_filehandlers(int nfh, fd_set *fhs)
{
  int fh = 0;
  
  while (nfh > 0) {
    if (FD_ISSET(fh, fhs)) {

      /* This descriptor can be read from, dispatch its handler */
      (file_handlers[fh].handler)(file_handlers[fh].arg);

      /* Decrement number of readable files still to find */
      --nfh;
    }

    ++fh;
  }

}

/* ================================================== */

static void
handle_slew(struct timeval *raw,
            struct timeval *cooked,
            double dfreq,
            double afreq,
            double doffset,
            int is_step_change,
            void *anything)
{
  TimerQueueEntry *ptr;
  struct timeval T1;

  if (is_step_change) {
    /* We're not interested in anything else - it won't affect the
       functionality of timer event dispatching.  If a step change
       occurs, just shift all the timeouts by the offset */
    
    for (ptr = timer_queue.next; ptr != &timer_queue; ptr = ptr->next) {
      UTI_AddDoubleToTimeval(&ptr->tv, -doffset, &T1);
      ptr->tv = T1;
    }

  }
}

/* ================================================== */

void
SCH_MainLoop(void)
{
  fd_set rd;
  int status;
  struct timeval tv, *ptv;
  struct timeval now;

  if (!initialised) {
    CROAK("Should be initialised");
  }

  while (!need_to_exit) {

    /* Copy current set of read file descriptors */
    memcpy((void *) &rd, (void *) &read_fds, sizeof(fd_set));
    
    /* Try to dispatch any timeouts that have already gone by, and
       keep going until all are done.  (The earlier ones may take so
       long to do that the later ones come around by the time they are
       completed). */

    do {
      LCL_ReadRawTime(&now);
    } while (dispatch_timeouts(&now) > 0);
    
    /* Check whether there is a timeout and set it up */
    if (n_timer_queue_entries > 0) {

      UTI_DiffTimevals(&tv, &(timer_queue.next->tv), &now);
      ptv = &tv;

    } else {
      ptv = NULL;
    }

    /* if there are no file descriptors being waited on and no
       timeout set, this is clearly ridiculous, so stop the run */

    if (!ptv && (n_read_fds == 0)) {
      LOG_FATAL(LOGF_Scheduler, "No descriptors or timeout to wait for");
    }

    status = select(one_highest_fd, &rd, NULL, NULL, ptv);

    if (status < 0) {
      if (!need_to_exit)
        CROAK("Status < 0 after select");
    } else if (status > 0) {
      /* A file descriptor is ready to read */

      LCL_ReadCookedTime(&last_fdready, &last_fdready_err);
      dispatch_filehandlers(status, &rd);

    } else {
      if (status != 0) {
        CROAK("Unexpected value from select");
      }

      /* No descriptors readable, timeout must have elapsed.
       Therefore, tv must be non-null */
      if (!ptv) {
        CROAK("No descriptors or timeout?");
      }

      /* There's nothing to do here, since the timeouts
         will be dispatched at the top of the next loop
         cycle */

    }
  }         

  return;

}

/* ================================================== */

void
SCH_QuitProgram(void)
{
  if (!initialised) {
    CROAK("Should be initialised");
  }
  need_to_exit = 1;
}

/* ================================================== */

