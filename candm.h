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

  Definitions for the network protocol used for command and monitoring
  of the timeserver.

  */

#ifndef GOT_CANDM_H
#define GOT_CANDM_H

#include "sysincl.h"
#include "addressing.h"
#include "hash.h"

/* This is the default port to use for CANDM, if no alternative is
   defined */
#define DEFAULT_CANDM_PORT 323

/* Request codes */
#define REQ_NULL 0
#define REQ_ONLINE 1
#define REQ_OFFLINE 2
#define REQ_BURST 3
#define REQ_MODIFY_MINPOLL 4
#define REQ_MODIFY_MAXPOLL 5
#define REQ_DUMP 6
#define REQ_MODIFY_MAXDELAY 7
#define REQ_MODIFY_MAXDELAYRATIO 8
#define REQ_MODIFY_MAXUPDATESKEW 9
#define REQ_LOGON 10
#define REQ_SETTIME 11
#define REQ_LOCAL 12
#define REQ_MANUAL 13
#define REQ_N_SOURCES 14
#define REQ_SOURCE_DATA 15
#define REQ_REKEY 16
#define REQ_ALLOW 17
#define REQ_ALLOWALL 18
#define REQ_DENY 19
#define REQ_DENYALL 20
#define REQ_CMDALLOW 21
#define REQ_CMDALLOWALL 22
#define REQ_CMDDENY 23
#define REQ_CMDDENYALL 24
#define REQ_ACCHECK 25
#define REQ_CMDACCHECK 26
#define REQ_ADD_SERVER 27
#define REQ_ADD_PEER 28
#define REQ_DEL_SOURCE 29
#define REQ_WRITERTC 30
#define REQ_DFREQ 31
#define REQ_DOFFSET 32
#define REQ_TRACKING 33
#define REQ_SOURCESTATS 34
#define REQ_RTCREPORT 35
#define REQ_TRIMRTC 36
#define REQ_CYCLELOGS 37
#define REQ_SUBNETS_ACCESSED 38
#define REQ_CLIENT_ACCESSES 39
#define REQ_CLIENT_ACCESSES_BY_INDEX 40
#define REQ_MANUAL_LIST 41
#define REQ_MANUAL_DELETE 42
#define REQ_MAKESTEP 43
#define REQ_ACTIVITY 44
#define REQ_MODIFY_MINSTRATUM 45
#define REQ_MODIFY_POLLTARGET 46
#define REQ_MODIFY_MAXDELAYDEVRATIO 47
#define REQ_RESELECT 48
#define REQ_RESELECTDISTANCE 49
#define N_REQUEST_TYPES 50

/* Special utoken value used to log on with first exchange being the
   password.  (This time value has long since gone by) */
#define SPECIAL_UTOKEN 0x10101010

/* Structure used to exchange timevals independent on size of time_t */
typedef struct {
  uint32_t tv_sec_high;
  uint32_t tv_sec_low;
  uint32_t tv_nsec;
} Timeval;

/* This is used in tv_sec_high for 32-bit timestamps */
#define TV_NOHIGHSEC 0x7fffffff

/* 32-bit floating-point format consisting of 7-bit signed exponent
   and 25-bit signed coefficient without hidden bit.
   The result is calculated as: 2^(exp - 25) * coef */
typedef struct {
  int32_t f;
} Float;

/* The EOR (end of record) fields are used by the offsetof operator in
   pktlength.c, to get the number of bytes that ought to be
   transmitted for each packet type. */

typedef struct {
  IPAddr mask;
  IPAddr address;
  int32_t EOR;
} REQ_Online;

typedef struct {
  IPAddr mask;
  IPAddr address;
  int32_t EOR;
} REQ_Offline;

typedef struct {
  IPAddr mask;
  IPAddr address;
  int32_t n_good_samples;
  int32_t n_total_samples;
  int32_t EOR;
} REQ_Burst;

typedef struct {
  IPAddr address;
  int32_t new_minpoll;
  int32_t EOR;
} REQ_Modify_Minpoll;

typedef struct {
  IPAddr address;
  int32_t new_maxpoll;
  int32_t EOR;
} REQ_Modify_Maxpoll;

typedef struct {
  int32_t pad;
  int32_t EOR;
} REQ_Dump;

typedef struct {
  IPAddr address;
  Float new_max_delay;
  int32_t EOR;
} REQ_Modify_Maxdelay;

typedef struct {
  IPAddr address;
  Float new_max_delay_ratio;
  int32_t EOR;
} REQ_Modify_Maxdelayratio;

typedef struct {
  IPAddr address;
  Float new_max_delay_dev_ratio;
  int32_t EOR;
} REQ_Modify_Maxdelaydevratio;

typedef struct {
  IPAddr address;
  int32_t new_min_stratum;
  int32_t EOR;
} REQ_Modify_Minstratum;

typedef struct {
  IPAddr address;
  int32_t new_poll_target;
  int32_t EOR;
} REQ_Modify_Polltarget;

typedef struct {
  Float new_max_update_skew;
  int32_t EOR;
} REQ_Modify_Maxupdateskew;

typedef struct {
  Timeval ts;
  int32_t EOR;
} REQ_Logon;

typedef struct {
  Timeval ts;
  int32_t EOR;
} REQ_Settime;

typedef struct {
  int32_t on_off;
  int32_t stratum;
  int32_t EOR;
} REQ_Local;

typedef struct {
  int32_t option;
  int32_t EOR;
} REQ_Manual;

typedef struct {
  int32_t EOR;
} REQ_N_Sources;

typedef struct {
  int32_t index;
  int32_t EOR;
} REQ_Source_Data;

typedef struct {
  int32_t EOR;
} REQ_Rekey;

typedef struct {
  IPAddr ip;
  int32_t subnet_bits;
  int32_t EOR;
} REQ_Allow_Deny;

typedef struct {
  IPAddr ip;
  int32_t EOR;
} REQ_Ac_Check;

/* Flags used in NTP source requests */
#define REQ_ADDSRC_ONLINE 0x1
#define REQ_ADDSRC_AUTOOFFLINE 0x2
#define REQ_ADDSRC_IBURST 0x4
#define REQ_ADDSRC_PREFER 0x8
#define REQ_ADDSRC_NOSELECT 0x10

typedef struct {
  IPAddr ip_addr;
  uint32_t port;
  int32_t minpoll;
  int32_t maxpoll;
  int32_t presend_minpoll;
  uint32_t authkey;
  Float max_delay;
  Float max_delay_ratio;
  uint32_t flags;
  int32_t EOR;
} REQ_NTP_Source;

typedef struct {
  IPAddr ip_addr;
  int32_t EOR;
} REQ_Del_Source;

typedef struct {
  int32_t EOR;
} REQ_WriteRtc;

typedef struct {
  Float dfreq;
  int32_t EOR;
} REQ_Dfreq;

typedef struct {
  int32_t sec;
  int32_t usec;
  int32_t EOR;
} REQ_Doffset;

typedef struct {
  int32_t EOR;
} REQ_Tracking;

typedef struct {
  uint32_t index;
  int32_t EOR;
} REQ_Sourcestats;

typedef struct {
  int32_t EOR;
} REQ_RTCReport;

typedef struct {
  int32_t EOR;
} REQ_TrimRTC;

typedef struct {
  int32_t EOR;
} REQ_CycleLogs;

typedef struct {
  IPAddr ip;
  uint32_t bits_specd;
} REQ_SubnetsAccessed_Subnet;

#define MAX_SUBNETS_ACCESSED 8

typedef struct {
  uint32_t n_subnets;
  REQ_SubnetsAccessed_Subnet subnets[MAX_SUBNETS_ACCESSED];
} REQ_SubnetsAccessed;

/* This is based on the response size rather than the
   request size */
#define MAX_CLIENT_ACCESSES 8

typedef struct {
  uint32_t n_clients;
  IPAddr client_ips[MAX_CLIENT_ACCESSES];
} REQ_ClientAccesses;  

typedef struct {
  uint32_t first_index;
  uint32_t n_indices;
  int32_t EOR;
} REQ_ClientAccessesByIndex;

typedef struct {
  int32_t EOR;
} REQ_ManualList;

typedef struct {
  int32_t index;
  int32_t EOR;
} REQ_ManualDelete;

typedef struct {
  int32_t EOR;
} REQ_MakeStep;

typedef struct {
  int32_t EOR;
} REQ_Activity;

typedef struct {
  int32_t EOR;
} REQ_Reselect;

typedef struct {
  Float distance;
  int32_t EOR;
} REQ_ReselectDistance;

/* ================================================== */

#define PKT_TYPE_CMD_REQUEST 1
#define PKT_TYPE_CMD_REPLY 2

/* This version number needs to be incremented whenever the packet
   size and/or the format of any of the existing messages is changed.
   Other changes, e.g. new command types, should be handled cleanly by
   client.c and cmdmon.c anyway, so the version can stay the same.
   
   Version 1 : original version with fixed size packets

   Version 2 : both command and reply packet sizes made capable of
   being variable length.

   Version 3 : NTP_Source message lengthened (auto_offline)

   Version 4 : IPv6 addressing added, 64-bit time values, sourcestats 
   and tracking reports extended, added flags to NTP source request,
   trimmed source report, replaced fixed-point format with floating-point
   and used also instead of integer microseconds, new commands: modify stratum,
   modify polltarget, modify maxdelaydevratio, reselect, reselectdistance

   Version 5 : auth data moved to the end of the packet to allow hashes with
   different sizes, extended sources, tracking and activity reports
 */

#define PROTO_VERSION_NUMBER 5

/* The oldest protocol version that is compatible enough with
   the current version to report a version mismatch */
#define PROTO_VERSION_MISMATCH_COMPAT 4

/* ================================================== */

typedef struct {
  uint8_t version; /* Protocol version */
  uint8_t pkt_type; /* What sort of packet this is */
  uint8_t res1;
  uint8_t res2;
  uint16_t command; /* Which command is being issued */
  uint16_t attempt; /* How many resends the client has done
                             (count up from zero for same sequence
                             number) */
  uint32_t sequence; /* Client's sequence number */
  uint32_t utoken; /* Unique token per incarnation of daemon */
  uint32_t token; /* Command token (to prevent replay attack) */

  union {
    REQ_Online online;
    REQ_Offline offline;
    REQ_Burst burst;
    REQ_Modify_Minpoll modify_minpoll;
    REQ_Modify_Maxpoll modify_maxpoll;
    REQ_Dump dump;
    REQ_Modify_Maxdelay modify_maxdelay;
    REQ_Modify_Maxdelayratio modify_maxdelayratio;
    REQ_Modify_Maxdelaydevratio modify_maxdelaydevratio;
    REQ_Modify_Minstratum modify_minstratum;
    REQ_Modify_Polltarget modify_polltarget;
    REQ_Modify_Maxupdateskew modify_maxupdateskew;
    REQ_Logon logon;
    REQ_Settime settime;
    REQ_Local local;
    REQ_Manual manual;
    REQ_N_Sources n_sources;
    REQ_Source_Data source_data;
    REQ_Rekey rekey;
    REQ_Allow_Deny allow_deny;
    REQ_Ac_Check ac_check;
    REQ_NTP_Source ntp_source;
    REQ_Del_Source del_source;
    REQ_WriteRtc writertc;
    REQ_Dfreq dfreq;
    REQ_Doffset doffset;
    REQ_Tracking tracking;
    REQ_Sourcestats sourcestats;
    REQ_RTCReport rtcreport;
    REQ_TrimRTC trimrtc;
    REQ_CycleLogs cyclelogs;
    REQ_SubnetsAccessed subnets_accessed;
    REQ_ClientAccesses client_accesses;
    REQ_ClientAccessesByIndex client_accesses_by_index;
    REQ_ManualList manual_list;
    REQ_ManualDelete manual_delete;
    REQ_MakeStep make_step;
    REQ_Activity activity;
    REQ_Reselect reselect;
    REQ_ReselectDistance reselect_distance;
  } data; /* Command specific parameters */

  /* authentication of the packet, there is no hole after the actual data
     from the data union, this field only sets the maximum auth size */
  uint8_t auth[MAX_HASH_LENGTH];

} CMD_Request;

/* ================================================== */
/* Authority codes for command types */

#define PERMIT_OPEN 0
#define PERMIT_LOCAL 1
#define PERMIT_AUTH 2

/* ================================================== */

/* Reply codes */
#define RPY_NULL 1
#define RPY_N_SOURCES 2
#define RPY_SOURCE_DATA 3
#define RPY_MANUAL_TIMESTAMP 4
#define RPY_TRACKING 5
#define RPY_SOURCESTATS 6
#define RPY_RTC 7
#define RPY_SUBNETS_ACCESSED 8
#define RPY_CLIENT_ACCESSES 9
#define RPY_CLIENT_ACCESSES_BY_INDEX 10
#define RPY_MANUAL_LIST 11
#define RPY_ACTIVITY 12
#define N_REPLY_TYPES 13

/* Status codes */
#define STT_SUCCESS 0
#define STT_FAILED 1
#define STT_UNAUTH 2
#define STT_INVALID 3
#define STT_NOSUCHSOURCE 4
#define STT_INVALIDTS 5
#define STT_NOTENABLED 6
#define STT_BADSUBNET 7
#define STT_ACCESSALLOWED 8
#define STT_ACCESSDENIED 9
#define STT_NOHOSTACCESS 10
#define STT_SOURCEALREADYKNOWN 11
#define STT_TOOMANYSOURCES 12
#define STT_NORTC 13
#define STT_BADRTCFILE 14
#define STT_INACTIVE 15
#define STT_BADSAMPLE 16
#define STT_INVALIDAF 17
#define STT_BADPKTVERSION 18
#define STT_BADPKTLENGTH 19

typedef struct {
  int32_t EOR;
} RPY_Null;

typedef struct {
  uint32_t n_sources;
  int32_t EOR;
} RPY_N_Sources;

#define RPY_SD_MD_CLIENT 0
#define RPY_SD_MD_PEER   1
#define RPY_SD_MD_REF    2

#define RPY_SD_ST_SYNC 0
#define RPY_SD_ST_UNREACH 1
#define RPY_SD_ST_FALSETICKER 2
#define RPY_SD_ST_JITTERY 3
#define RPY_SD_ST_CANDIDATE 4
#define RPY_SD_ST_OUTLYER 5

#define RPY_SD_FLAG_NOSELECT 0x1
#define RPY_SD_FLAG_PREFER 0x2

typedef struct {
  IPAddr ip_addr;
  uint16_t poll;
  uint16_t stratum;
  uint16_t state;
  uint16_t mode;
  uint16_t flags;
  uint16_t reachability;
  uint32_t  since_sample;
  Float orig_latest_meas;
  Float latest_meas;
  Float latest_meas_err;
  int32_t EOR;
} RPY_Source_Data;

typedef struct {
  uint32_t ref_id;
  IPAddr ip_addr;
  uint32_t stratum;
  Timeval ref_time;
  Float current_correction;
  Float last_offset;
  Float rms_offset;
  Float freq_ppm;
  Float resid_freq_ppm;
  Float skew_ppm;
  Float root_delay;
  Float root_dispersion;
  Float last_update_interval;
  int32_t EOR;
} RPY_Tracking;

typedef struct {
  uint32_t ref_id;
  IPAddr ip_addr;
  uint32_t n_samples;
  uint32_t n_runs;
  uint32_t span_seconds;
  Float sd;
  Float resid_freq_ppm;
  Float skew_ppm;
  Float est_offset;
  Float est_offset_err;
  int32_t EOR;
} RPY_Sourcestats;

typedef struct {
  Timeval ref_time;
  uint16_t n_samples;
  uint16_t n_runs;
  uint32_t span_seconds;
  Float rtc_seconds_fast;
  Float rtc_gain_rate_ppm;
  int32_t EOR;
} RPY_Rtc;

typedef struct {
  uint32_t centiseconds;
  Float dfreq_ppm;
  Float new_afreq_ppm;
  int32_t EOR;
} RPY_ManualTimestamp;

typedef struct {
  IPAddr ip;
  uint32_t bits_specd;
  uint32_t bitmap[8];
} RPY_SubnetsAccessed_Subnet;

typedef struct {
  uint32_t n_subnets;
  RPY_SubnetsAccessed_Subnet subnets[MAX_SUBNETS_ACCESSED];
} RPY_SubnetsAccessed;

typedef struct {
  IPAddr ip;
  uint32_t client_hits;
  uint32_t peer_hits;
  uint32_t cmd_hits_auth;
  uint32_t cmd_hits_normal;
  uint32_t cmd_hits_bad;
  uint32_t last_ntp_hit_ago;
  uint32_t last_cmd_hit_ago;
} RPY_ClientAccesses_Client;

typedef struct {
  uint32_t n_clients;
  RPY_ClientAccesses_Client clients[MAX_CLIENT_ACCESSES];
} RPY_ClientAccesses;

typedef struct {
  uint32_t n_indices;      /* how many indices there are in the server's table */
  uint32_t next_index;     /* the index 1 beyond those processed on this call */
  uint32_t n_clients;      /* the number of valid entries in the following array */
  RPY_ClientAccesses_Client clients[MAX_CLIENT_ACCESSES];
} RPY_ClientAccessesByIndex;

#define MAX_MANUAL_LIST_SAMPLES 32

typedef struct {
  Timeval when;
  Float slewed_offset;
  Float orig_offset;
  Float residual;
} RPY_ManualListSample;

typedef struct {
  uint32_t n_samples;
  RPY_ManualListSample samples[MAX_MANUAL_LIST_SAMPLES];
} RPY_ManualList;

typedef struct {
  int32_t online;
  int32_t offline;
  int32_t burst_online;
  int32_t burst_offline;
  int32_t unresolved;
  int32_t EOR;
} RPY_Activity;

typedef struct {
  uint8_t version;
  uint8_t pkt_type;
  uint8_t res1;
  uint8_t res2;
  uint16_t command; /* Which command is being replied to */
  uint16_t reply; /* Which format of reply this is */
  uint16_t status; /* Status of command processing */
  uint16_t number; /* Which packet this is in reply sequence */
  uint16_t total; /* Number of replies to expect in this sequence */
  uint16_t pad1; /* Get up to 4 byte alignment */
  uint32_t sequence; /* Echo of client's sequence number */
  uint32_t utoken; /* Unique token per incarnation of daemon */
  uint32_t token; /* New command token (only if command was successfully
                          authenticated) */
  union {
    RPY_Null null;
    RPY_N_Sources n_sources;
    RPY_Source_Data source_data;
    RPY_ManualTimestamp manual_timestamp;
    RPY_Tracking tracking;
    RPY_Sourcestats sourcestats;
    RPY_Rtc rtc;
    RPY_SubnetsAccessed subnets_accessed;
    RPY_ClientAccesses client_accesses;
    RPY_ClientAccessesByIndex client_accesses_by_index;
    RPY_ManualList manual_list;
    RPY_Activity activity;
  } data; /* Reply specific parameters */

  /* authentication of the packet, there is no hole after the actual data
     from the data union, this field only sets the maximum auth size */
  uint8_t auth[MAX_HASH_LENGTH];

} CMD_Reply;

/* ================================================== */

#endif /* GOT_CANDM_H */
