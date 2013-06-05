/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2011
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

#include "config.h"

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

/* Timestamp when last select() returned */
static struct timeval last_select_ts, last_select_ts_raw;
static double last_select_ts_err;

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

/* Timestamp when was last timeout dispatched for each class */
static struct timeval last_class_dispatch[SCH_NumberOfClasses];

/* ================================================== */

static int need_to_exit;

/* ================================================== */

static void
handle_slew(struct timeval *raw,
            struct timeval *cooked,
            double dfreq,
            double doffset,
            int is_step_change,
            void *anything);

/* ================================================== */

void
SCH_Initialise(void)
{
  struct timeval tv;

  FD_ZERO(&read_fds);
  n_read_fds = 0;

  n_timer_queue_entries = 0;
  next_tqe_id = 0;

  timer_queue.next = &timer_queue;
  timer_queue.prev = &timer_queue;

  need_to_exit = 0;

  LCL_AddParameterChangeHandler(handle_slew, NULL);

  LCL_ReadRawTime(&tv);
  srandom(tv.tv_sec << 16 ^ tv.tv_usec);

  initialised = 1;
}


/* ================================================== */

void
SCH_Finalise(void) {
  initialised = 0;
}

/* ================================================== */

void
SCH_AddInputFileHandler
(int fd, SCH_FileHandler handler, SCH_ArbitraryArgument arg)
{

  assert(initialised);
  
  /* Don't want to allow the same fd to register a handler more than
     once without deleting a previous association - this suggests
     a bug somewhere else in the program. */
  assert(!FD_ISSET(fd, &read_fds));

  ++n_read_fds;
  
  file_handlers[fd].handler = handler;
  file_handlers[fd].arg     = arg;

  FD_SET(fd, &read_fds);

  if ((fd + 1) > one_highest_fd) {
    one_highest_fd = fd + 1;
  }
}


/* ================================================== */

void
SCH_RemoveInputFileHandler(int fd)
{
  int fds_left, fd_to_check;

  assert(initialised);

  /* Check that a handler was registered for the fd in question */
  assert(FD_ISSET(fd, &read_fds));

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
}

/* ================================================== */

void
SCH_GetLastEventTime(struct timeval *cooked, double *err, struct timeval *raw)
{
  if (cooked) {
    *cooked = last_select_ts;
    if (err)
      *err = last_select_ts_err;
  }
  if (raw)
    *raw = last_select_ts_raw;
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
}

/* ================================================== */

SCH_TimeoutID
SCH_AddTimeout(struct timeval *tv, SCH_TimeoutHandler handler, SCH_ArbitraryArgument arg)
{
  TimerQueueEntry *new_tqe;
  TimerQueueEntry *ptr;

  assert(initialised);

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

  assert(initialised);
  assert(delay >= 0.0);

  LCL_ReadRawTime(&now);
  UTI_AddDoubleToTimeval(&now, delay, &then);
  return SCH_AddTimeout(&then, handler, arg);

}

/* ================================================== */

SCH_TimeoutID
SCH_AddTimeoutInClass(double min_delay, double separation, double randomness,
                      SCH_TimeoutClass class,
                      SCH_TimeoutHandler handler, SCH_ArbitraryArgument arg)
{
  TimerQueueEntry *new_tqe;
  TimerQueueEntry *ptr;
  struct timeval now;
  double diff, r;
  double new_min_delay;

  assert(initialised);
  assert(min_delay >= 0.0);
  assert(class < SCH_NumberOfClasses);

  if (randomness > 0.0) {
    r = random() % 0xffff / (0xffff - 1.0) * randomness + 1.0;
    min_delay *= r;
    separation *= r;
  }
  
  LCL_ReadRawTime(&now);
  new_min_delay = min_delay;

  /* Check the separation from the last dispatched timeout */
  UTI_DiffTimevalsToDouble(&diff, &now, &last_class_dispatch[class]);
  if (diff < separation && diff >= 0.0 && diff + new_min_delay < separation) {
    new_min_delay = separation - diff;
  }

  /* Scan through list for entries in the same class and increase min_delay
     if necessary to keep at least the separation away */
  for (ptr = timer_queue.next; ptr != &timer_queue; ptr = ptr->next) {
    if (ptr->class == class) {
      UTI_DiffTimevalsToDouble(&diff, &ptr->tv, &now);
      if (new_min_delay > diff) {
        if (new_min_delay - diff < separation) {
          new_min_delay = diff + separation;
        }
      } else {
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

  assert(initialised);

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

      break;
    }
  }
}

/* ================================================== */
/* Try to dispatch any timeouts that have already gone by, and
   keep going until all are done.  (The earlier ones may take so
   long to do that the later ones come around by the time they are
   completed). */

static void
dispatch_timeouts(struct timeval *now) {
  TimerQueueEntry *ptr;
  SCH_TimeoutHandler handler;
  SCH_ArbitraryArgument arg;
  int n_done = 0, n_entries_on_start = n_timer_queue_entries;

  while (1) {
    LCL_ReadRawTime(now);

    if (!(n_timer_queue_entries > 0 &&
          UTI_CompareTimevals(now, &(timer_queue.next->tv)) >= 0)) {
      break;
    }

    ptr = timer_queue.next;

    last_class_dispatch[ptr->class] = *now;

    handler = ptr->handler;
    arg = ptr->arg;

    SCH_RemoveTimeout(ptr->id);

    /* Dispatch the handler */
    (handler)(arg);

    /* Increment count of timeouts handled */
    ++n_done;

    /* If more timeouts were handled than there were in the timer queue on
       start and there are now, assume some code is scheduling timeouts with
       negative delays and abort.  Make the actual limit higher in case the
       machine is temporarily overloaded and dispatching the handlers takes
       more time than was delay of a scheduled timeout. */
    if (n_done > n_timer_queue_entries * 4 &&
        n_done > n_entries_on_start * 4) {
      LOG_FATAL(LOGF_Scheduler, "Possible infinite loop in scheduling");
    }
  }
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
            double doffset,
            int is_step_change,
            void *anything)
{
  TimerQueueEntry *ptr;
  int i;

  if (is_step_change) {
    /* We're not interested in anything else - it won't affect the
       functionality of timer event dispatching.  If a step change
       occurs, just shift all the timeouts by the offset */
    
    for (ptr = timer_queue.next; ptr != &timer_queue; ptr = ptr->next) {
      UTI_AddDoubleToTimeval(&ptr->tv, -doffset, &ptr->tv);
    }

    for (i = 0; i < SCH_NumberOfClasses; i++) {
      UTI_AddDoubleToTimeval(&last_class_dispatch[i], -doffset, &last_class_dispatch[i]);
    }

    UTI_AddDoubleToTimeval(&last_select_ts_raw, -doffset, &last_select_ts_raw);
    UTI_AddDoubleToTimeval(&last_select_ts, -doffset, &last_select_ts);
  }
}

/* ================================================== */

/* Try to handle unexpected backward time jump */

static void
recover_backjump(struct timeval *raw, struct timeval *cooked, int timeout)
{
      double diff, err;

      UTI_DiffTimevalsToDouble(&diff, &last_select_ts_raw, raw);

      if (n_timer_queue_entries > 0) {
        UTI_DiffTimevalsToDouble(&err, &(timer_queue.next->tv), &last_select_ts_raw);
      } else {
        err = 0.0;
      }

      diff += err;

      if (timeout) {
        err = 1.0;
      }

      LOG(LOGS_WARN, LOGF_Scheduler, "Backward time jump detected! (correction %.1f +- %.1f seconds)", diff, err);

      LCL_NotifyExternalTimeStep(raw, cooked, diff, err);
}

/* ================================================== */

void
SCH_MainLoop(void)
{
  fd_set rd;
  int status;
  struct timeval tv, *ptv;
  struct timeval now, cooked;
  double err;

  assert(initialised);

  while (!need_to_exit) {

    /* Copy current set of read file descriptors */
    memcpy((void *) &rd, (void *) &read_fds, sizeof(fd_set));
    
    /* Dispatch timeouts and fill now with current raw time */
    dispatch_timeouts(&now);
    
    /* Check whether there is a timeout and set it up */
    if (n_timer_queue_entries > 0) {

      UTI_DiffTimevals(&tv, &(timer_queue.next->tv), &now);
      ptv = &tv;
      assert(tv.tv_sec > 0 || tv.tv_usec > 0);

    } else {
      ptv = NULL;
    }

    /* if there are no file descriptors being waited on and no
       timeout set, this is clearly ridiculous, so stop the run */

    if (!ptv && (n_read_fds == 0)) {
      LOG_FATAL(LOGF_Scheduler, "No descriptors or timeout to wait for");
    }

    status = select(one_highest_fd, &rd, NULL, NULL, ptv);

    LCL_ReadRawTime(&now);
    LCL_CookTime(&now, &cooked, &err);

    /* Check if time didn't jump backwards */
    if (last_select_ts_raw.tv_sec > now.tv_sec + 1) {
      recover_backjump(&now, &cooked, status == 0);
    }

    last_select_ts_raw = now;
    last_select_ts = cooked;
    last_select_ts_err = err;

    if (status < 0) {
      assert(need_to_exit);
    } else if (status > 0) {
      /* A file descriptor is ready to read */

      dispatch_filehandlers(status, &rd);

    } else {
      assert(status == 0);

      /* No descriptors readable, timeout must have elapsed.
       Therefore, tv must be non-null */
      assert(ptv);

      /* There's nothing to do here, since the timeouts
         will be dispatched at the top of the next loop
         cycle */

    }
  }         
}

/* ================================================== */

void
SCH_QuitProgram(void)
{
  assert(initialised);
  need_to_exit = 1;
}

/* ================================================== */

