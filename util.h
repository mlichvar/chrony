/*
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

  Various utility functions
  */

#ifndef GOT_UTIL_H
#define GOT_UTIL_H

#include "sysincl.h"

#include "addressing.h"
#include "ntp.h"
#include "candm.h"
#include "hash.h"

/* Convert a timeval into a floating point number of seconds */
extern void UTI_TimevalToDouble(struct timeval *a, double *b);

/* Convert a number of seconds expressed in floating point into a
   timeval */
extern void UTI_DoubleToTimeval(double a, struct timeval *b);

/* Returns -1 if a comes earlier than b, 0 if a is the same time as b,
   and +1 if a comes after b */
extern int UTI_CompareTimevals(struct timeval *a, struct timeval *b);

/* Normalise a struct timeval, by adding or subtracting seconds to bring
   its microseconds field into range */
extern void UTI_NormaliseTimeval(struct timeval *x);

/* Calculate result = a - b */
extern void UTI_DiffTimevals(struct timeval *result, struct timeval *a, struct timeval *b);

/* Calculate result = a - b and return as a double */
extern void UTI_DiffTimevalsToDouble(double *result, struct timeval *a, struct timeval *b);

/* Add a double increment to a timeval to get a new one. 'start' is
   the starting time, 'end' is the result that we return.  This is
   safe to use if start and end are the same */
extern void UTI_AddDoubleToTimeval(struct timeval *start, double increment, struct timeval *end);

/* Calculate the average and difference (as a double) of two timevals */
extern void UTI_AverageDiffTimevals(struct timeval *earlier, struct timeval *later, struct timeval *average, double *diff);

/* Calculate result = a - b + c */
extern void UTI_AddDiffToTimeval(struct timeval *a, struct timeval *b, struct timeval *c, struct timeval *result);

/* Convert a timeval into a temporary string, largely for diagnostic
   display */
extern char *UTI_TimevalToString(struct timeval *tv);

/* Convert an NTP timestamp into a temporary string, largely for
   diagnostic display */
extern char *UTI_TimestampToString(NTP_int64 *ts);

/* Convert ref_id into a temporary string, for diagnostics */
extern char *UTI_RefidToString(uint32_t ref_id);

/* Convert an IP address to string, for diagnostics */
extern char *UTI_IPToString(IPAddr *ip);

extern int UTI_StringToIP(const char *addr, IPAddr *ip);
extern uint32_t UTI_IPToRefid(IPAddr *ip);
extern void UTI_IPHostToNetwork(IPAddr *src, IPAddr *dest);
extern void UTI_IPNetworkToHost(IPAddr *src, IPAddr *dest);
extern int UTI_CompareIPs(IPAddr *a, IPAddr *b, IPAddr *mask);

extern void UTI_SockaddrToIPAndPort(struct sockaddr *sa, IPAddr *ip, unsigned short *port);
extern int UTI_IPAndPortToSockaddr(IPAddr *ip, unsigned short port, struct sockaddr *sa);
extern char *UTI_SockaddrToString(struct sockaddr *sa);
extern const char *UTI_SockaddrFamilyToString(int family);

extern char *UTI_TimeToLogForm(time_t t);

/* Adjust time following a frequency/offset change */
extern void UTI_AdjustTimeval(struct timeval *old_tv, struct timeval *when, struct timeval *new_tv, double *delta, double dfreq, double doffset);

/* Get a random value to fuzz an NTP timestamp in the given precision */
extern uint32_t UTI_GetNTPTsFuzz(int precision);

extern double UTI_Int32ToDouble(NTP_int32 x);
extern NTP_int32 UTI_DoubleToInt32(double x);

extern void UTI_TimevalToInt64(struct timeval *src, NTP_int64 *dest, uint32_t fuzz);

extern void UTI_Int64ToTimeval(NTP_int64 *src, struct timeval *dest);

/* Check if time + offset is sane */
extern int UTI_IsTimeOffsetSane(struct timeval *tv, double offset);

/* Get 2 raised to power of a signed integer */
extern double UTI_Log2ToDouble(int l);

extern void UTI_TimevalNetworkToHost(Timeval *src, struct timeval *dest);
extern void UTI_TimevalHostToNetwork(struct timeval *src, Timeval *dest);

extern double UTI_FloatNetworkToHost(Float x);
extern Float UTI_FloatHostToNetwork(double x);

/* Set FD_CLOEXEC on descriptor */
extern int UTI_FdSetCloexec(int fd);

extern int UTI_GenerateNTPAuth(int hash_id, const unsigned char *key, int key_len,
    const unsigned char *data, int data_len, unsigned char *auth, int auth_len);
extern int UTI_CheckNTPAuth(int hash_id, const unsigned char *key, int key_len,
    const unsigned char *data, int data_len, const unsigned char *auth, int auth_len);

/* Decode password encoded in ASCII or HEX */
extern int UTI_DecodePasswordFromText(char *key);

extern int UTI_SetQuitSignalsHandler(void (*handler)(int));

/* Get directory (as an allocated string) for a path */
extern char *UTI_PathToDir(const char *path);

/* Create a directory with a specified mode (umasked) and set its uid/gid.
   Create also any parent directories that don't exist with mode 755 and
   default uid/gid.  Returns 1 if created or already exists (even with
   different mode/uid/gid), 0 otherwise. */
extern int UTI_CreateDirAndParents(const char *path, mode_t mode, uid_t uid, gid_t gid);

/* Check if a directory is secure.  It must not have other than the specified
   permissions and its uid/gid must match the specified values. */
extern int UTI_CheckDirPermissions(const char *path, mode_t perm, uid_t uid, gid_t gid);

#endif /* GOT_UTIL_H */
