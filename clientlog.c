/*
  $Header: /cvs/src/chrony/clientlog.c,v 1.11 2003/09/22 21:22:30 richard Exp $

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
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 * 
 **********************************************************************

  =======================================================================

  This module keeps a count of the number of successful accesses by
  clients, and the times of the last accesses.

  This can be used for status reporting, and (in the case of a
  server), if it needs to know which clients have made use of its data
  recently.

  */

#include "sysincl.h"
#include "clientlog.h"
#include "conf.h"
#include "memory.h"
#include "reports.h"
#include "util.h"

/* Number of bits of address per layer of the table.  This value has
   been chosen on the basis that a server will predominantly be serving
   a lot of hosts in a few subnets, rather than a few hosts scattered
   across many subnets. */

#define NBITS 8

/* Number of entries in each subtable */
#define TABLE_SIZE (1UL<<NBITS)

typedef struct _Node {
  unsigned long ip_addr;
  unsigned long client_hits;
  unsigned long peer_hits;
  unsigned long cmd_hits_bad;
  unsigned long cmd_hits_normal;
  unsigned long cmd_hits_auth;
  time_t last_ntp_hit;
  time_t last_cmd_hit;
} Node;

typedef struct _Subnet {
  void *entry[TABLE_SIZE];
} Subnet;

/* ================================================== */

/* Table for the class A subnet */
static Subnet top_subnet;

/* Table containing pointers directly to all nodes that have been
   allocated. */
static Node **nodes = NULL;

/* Number of nodes actually in the table. */
static int n_nodes = 0;

/* Number of entries for which the table has been sized. */
static int max_nodes = 0;

#define NODE_TABLE_INCREMENT 4

/* Flag indicating whether facility is turned on or not */
static int active = 0;

/* ================================================== */

static void
clear_subnet(Subnet *subnet)
{
  int i;

  for (i=0; i<TABLE_SIZE; i++) {
    subnet->entry[i] = NULL;
  }
}

/* ================================================== */

static void
clear_node(Node *node)
{
  node->client_hits = 0;
  node->peer_hits = 0;
  node->cmd_hits_auth = 0;
  node->cmd_hits_normal = 0;
  node->cmd_hits_bad = 0;
  node->last_ntp_hit = (time_t) 0;
  node->last_cmd_hit = (time_t) 0;
}

/* ================================================== */

void
CLG_Initialise(void)
{
  clear_subnet(&top_subnet);
  if (CNF_GetNoClientLog()) {
    active = 0;
  } else {
    active = 1;
  }

  nodes = NULL;
  max_nodes = 0;
  n_nodes = 0;

}

/* ================================================== */

void
CLG_Finalise(void)
{
  return;
}

/* ================================================== */

static void
create_subnet(Subnet *parent_subnet, int the_entry)
{
  parent_subnet->entry[the_entry] = (void *) MallocNew(Subnet);
  clear_subnet((Subnet *) parent_subnet->entry[the_entry]);
}

/* ================================================== */

static void
create_node(Subnet *parent_subnet, int the_entry)
{
  Node *new_node;
  new_node = MallocNew(Node);
  parent_subnet->entry[the_entry] = (void *) new_node;
  clear_node(new_node);

  if (n_nodes == max_nodes) {
    if (nodes) {
      max_nodes += NODE_TABLE_INCREMENT;
      nodes = ReallocArray(Node *, max_nodes, nodes);
    } else {
      if (max_nodes != 0) {
        CROAK("max_nodes should be 0");
      }
      max_nodes = NODE_TABLE_INCREMENT;
      nodes = MallocArray(Node *, max_nodes);
    }
  }
  nodes[n_nodes++] = (Node *) new_node;
}

/* ================================================== */
/* Recursively seek out the Node entry for a particular address,
   expanding subnet tables and node entries as we go if necessary. */

static void *
find_subnet(Subnet *subnet, CLG_IP_Addr addr, int bits_left)
{
  unsigned long this_subnet, new_subnet, mask, shift;
  unsigned long new_bits_left;
  
  shift = 32 - NBITS;
  mask = (1UL<<shift) - 1;
  this_subnet = addr >> shift;
  new_subnet = (addr & mask) << NBITS;
  new_bits_left = bits_left - NBITS;

#if 0
  fprintf(stderr, "fs addr=%08lx bl=%d ma=%08lx this=%08lx newsn=%08lx nbl=%d\n",
          addr, bits_left, mask, this_subnet, new_subnet, new_bits_left);
#endif

  if (new_bits_left > 0) {
    if (!subnet->entry[this_subnet]) {
      create_subnet(subnet, this_subnet);
    }
    return find_subnet((Subnet *) subnet->entry[this_subnet], new_subnet, new_bits_left);
  } else {
    if (!subnet->entry[this_subnet]) {
      create_node(subnet, this_subnet);
    }
    return subnet->entry[this_subnet];
  }
}


/* ================================================== */
/* Search for the record for a particular subnet, but return NULL if
   one of the parents does not exist - never open a node out */

static void *
find_subnet_dont_open(Subnet *subnet, CLG_IP_Addr addr, int bits_left)
{
  unsigned long this_subnet, new_subnet, mask, shift;
  unsigned long new_bits_left;

  if (bits_left == 0) {
    return subnet;
  } else {
    
    shift = 32 - NBITS;
    mask = (1UL<<shift) - 1;
    this_subnet = addr >> shift;
    new_subnet = (addr & mask) << NBITS;
    new_bits_left = bits_left - NBITS;

#if 0
    fprintf(stderr, "fsdo addr=%08lx bl=%d this=%08lx newsn=%08lx nbl=%d\n",
            addr, bits_left, this_subnet, new_subnet, new_bits_left);
#endif
    
    if (!subnet->entry[this_subnet]) {
      return NULL;
    } else {
      return find_subnet_dont_open((Subnet *) subnet->entry[this_subnet], new_subnet, new_bits_left);
    }
  }
}

/* ================================================== */

void
CLG_LogNTPClientAccess (CLG_IP_Addr client, time_t now)
{
  Node *node;
  if (active) {
    node = (Node *) find_subnet(&top_subnet, client, 32);
    node->ip_addr = client;
    ++node->client_hits;
    node->last_ntp_hit = now;
  }
}

/* ================================================== */

void
CLG_LogNTPPeerAccess(CLG_IP_Addr client, time_t now)
{
  Node *node;
  if (active) {
    node = (Node *) find_subnet(&top_subnet, client, 32);
    node->ip_addr = client;
    ++node->peer_hits;
    node->last_ntp_hit = now;
  }
}

/* ================================================== */

void
CLG_LogCommandAccess(CLG_IP_Addr client, CLG_Command_Type type, time_t now)
{
  Node *node;
  if (active) {
    node = (Node *) find_subnet(&top_subnet, client, 32);
    node->ip_addr = client;
    node->last_cmd_hit = now;
    switch (type) {
      case CLG_CMD_AUTH:
        ++node->cmd_hits_auth;
        break;
      case CLG_CMD_NORMAL:
        ++node->cmd_hits_normal;
        break;
      case CLG_CMD_BAD_PKT:
        ++node->cmd_hits_bad;
        break;
      default:
        CROAK("Impossible");
        break;
    }
  }
}

/* ================================================== */

CLG_Status
CLG_GetSubnetBitmap(CLG_IP_Addr subnet, int bits, CLG_Bitmap result)
{
  Subnet *s;
  unsigned long i;
  unsigned long word, bit, mask;

  if ((bits == 0) || (bits == 8) || (bits == 16) || (bits == 24)) {
    memset (result, 0, TABLE_SIZE/8);
    if (active) {
      s = find_subnet_dont_open(&top_subnet, subnet, bits);
      if (s) {
        for (i=0; i<256; i++) {
          if (s->entry[i]) {
            word = i / 32;
            bit =  i % 32;
            mask = 1UL << bit;
            result[word] |= mask;
          }
        }
        return CLG_SUCCESS;
      } else {
        return CLG_EMPTYSUBNET;
      }
    } else {
      return CLG_INACTIVE;
    }
  } else {
    return CLG_BADSUBNET;
  }
}

/* ================================================== */

CLG_Status
CLG_GetClientAccessReportByIP(unsigned long ip, RPT_ClientAccess_Report *report, time_t now)
{
  Node *node;

  if (!active) {
    return CLG_INACTIVE;
  } else {
    node = (Node *) find_subnet_dont_open(&top_subnet, ip, 32);
  
    if (!node) {
      return CLG_EMPTYSUBNET;
    } else {
      report->client_hits = node->client_hits;
      report->peer_hits = node->peer_hits;
      report->cmd_hits_auth = node->cmd_hits_auth;
      report->cmd_hits_normal = node->cmd_hits_normal;
      report->cmd_hits_bad = node->cmd_hits_bad;
      report->last_ntp_hit_ago = now - node->last_ntp_hit;
      report->last_cmd_hit_ago = now - node->last_cmd_hit;

      return CLG_SUCCESS;
    }
  }
}

/* ================================================== */

CLG_Status
CLG_GetClientAccessReportByIndex(int index, RPT_ClientAccessByIndex_Report *report,
                                 time_t now, unsigned long *n_indices)
{
  Node *node;

  *n_indices = n_nodes;

  if (!active) {
    return CLG_INACTIVE;
  } else {

    if ((index < 0) || (index >= n_nodes)) {
      return CLG_INDEXTOOLARGE;
    }
    
    node = nodes[index];
    
    report->ip_addr = node->ip_addr;
    report->client_hits = node->client_hits;
    report->peer_hits = node->peer_hits;
    report->cmd_hits_auth = node->cmd_hits_auth;
    report->cmd_hits_normal = node->cmd_hits_normal;
    report->cmd_hits_bad = node->cmd_hits_bad;
    report->last_ntp_hit_ago = now - node->last_ntp_hit;
    report->last_cmd_hit_ago = now - node->last_cmd_hit;
    
    return CLG_SUCCESS;
  }

}
