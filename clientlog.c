/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
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

  This module keeps a count of the number of successful accesses by
  clients, and the times of the last accesses.

  This can be used for status reporting, and (in the case of a
  server), if it needs to know which clients have made use of its data
  recently.

  */

#include "config.h"

#include "sysincl.h"
#include "clientlog.h"
#include "conf.h"
#include "memory.h"
#include "reports.h"
#include "util.h"
#include "logging.h"

/* Number of bits of address per layer of the table.  This value has
   been chosen on the basis that a server will predominantly be serving
   a lot of hosts in a few subnets, rather than a few hosts scattered
   across many subnets. */

#define NBITS 8

/* Number of entries in each subtable */
#define TABLE_SIZE (1UL<<NBITS)

typedef struct _Node {
  IPAddr ip_addr;
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

/* Table for the IPv4 class A subnet */
static Subnet top_subnet4;
/* Table for IPv6 */
static Subnet top_subnet6;

/* Table containing pointers directly to all nodes that have been
   allocated. */
static Node **nodes = NULL;

/* Number of nodes actually in the table. */
static int n_nodes = 0;

/* Number of entries for which the table has been sized. */
static int max_nodes = 0;

/* Flag indicating whether facility is turned on or not */
static int active = 0;

/* Flag indicating whether memory allocation limit has been reached
   and no new nodes or subnets should be allocated */
static int alloc_limit_reached;

static unsigned long alloc_limit;
static unsigned long alloced;

/* ================================================== */

static void
split_ip6(IPAddr *ip, uint32_t *dst)
{
  int i;

  for (i = 0; i < 4; i++)
    dst[i] = ip->addr.in6[i * 4 + 0] << 24 |
             ip->addr.in6[i * 4 + 1] << 16 |
             ip->addr.in6[i * 4 + 2] << 8 |
             ip->addr.in6[i * 4 + 3];
}

/* ================================================== */

inline static uint32_t
get_subnet(uint32_t *addr, unsigned int where)
{
  int off;

  off = where / 32;
  where %= 32;

  return (addr[off] >> (32 - NBITS - where)) & ((1UL << NBITS) - 1);
}

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
  clear_subnet(&top_subnet4);
  clear_subnet(&top_subnet6);
  if (CNF_GetNoClientLog()) {
    active = 0;
  } else {
    active = 1;
  }

  nodes = NULL;
  max_nodes = 0;
  n_nodes = 0;

  alloced = 0;
  alloc_limit = CNF_GetClientLogLimit();
  alloc_limit_reached = 0;
}

/* ================================================== */

void
CLG_Finalise(void)
{
  int i;
  
  for (i = 0; i < n_nodes; i++)
    Free(nodes[i]);
  Free(nodes);
}

/* ================================================== */

static void check_alloc_limit() {
  if (alloc_limit_reached)
    return;

  if (alloced >= alloc_limit) {
    LOG(LOGS_WARN, LOGF_ClientLog, "Client log memory limit reached");
    alloc_limit_reached = 1;
  }
}

/* ================================================== */

static void
create_subnet(Subnet *parent_subnet, int the_entry)
{
  parent_subnet->entry[the_entry] = (void *) MallocNew(Subnet);
  clear_subnet((Subnet *) parent_subnet->entry[the_entry]);
  alloced += sizeof (Subnet);
  check_alloc_limit();
}

/* ================================================== */

static void
create_node(Subnet *parent_subnet, int the_entry)
{
  Node *new_node;
  new_node = MallocNew(Node);
  parent_subnet->entry[the_entry] = (void *) new_node;
  clear_node(new_node);

  alloced += sizeof (Node);

  if (n_nodes == max_nodes) {
    if (nodes) {
      assert(max_nodes > 0);
      max_nodes *= 2;
      nodes = ReallocArray(Node *, max_nodes, nodes);
    } else {
      assert(max_nodes == 0);
      max_nodes = 16;
      nodes = MallocArray(Node *, max_nodes);
    }
    alloced += sizeof (Node *) * (max_nodes - n_nodes);
  }
  nodes[n_nodes++] = (Node *) new_node;
  check_alloc_limit();
}

/* ================================================== */
/* Recursively seek out the Node entry for a particular address,
   expanding subnet tables and node entries as we go if necessary. */

static void *
find_subnet(Subnet *subnet, uint32_t *addr, int addr_len, int bits_consumed)
{
  uint32_t this_subnet;

  this_subnet = get_subnet(addr, bits_consumed);
  bits_consumed += NBITS;

  if (bits_consumed < 32 * addr_len) {
    if (!subnet->entry[this_subnet]) {
      if (alloc_limit_reached)
        return NULL;
      create_subnet(subnet, this_subnet);
    }
    return find_subnet((Subnet *) subnet->entry[this_subnet], addr, addr_len, bits_consumed);
  } else {
    if (!subnet->entry[this_subnet]) {
      if (alloc_limit_reached)
        return NULL;
      create_node(subnet, this_subnet);
    }
    return subnet->entry[this_subnet];
  }
}

/* ================================================== */

static Node *
get_node(IPAddr *ip)
{
  uint32_t ip6[4];

  switch (ip->family) {
    case IPADDR_INET4:
      return (Node *)find_subnet(&top_subnet4, &ip->addr.in4, 1, 0);
    case IPADDR_INET6:
      split_ip6(ip, ip6);
      return (Node *)find_subnet(&top_subnet6, ip6, 4, 0);
    default:
      return NULL;
  }
}

/* ================================================== */

void
CLG_LogNTPClientAccess (IPAddr *client, time_t now)
{
  Node *node;

  if (active) {
    node = get_node(client);
    if (node == NULL)
      return;

    node->ip_addr = *client;
    ++node->client_hits;
    node->last_ntp_hit = now;
  }
}

/* ================================================== */

void
CLG_LogNTPPeerAccess(IPAddr *client, time_t now)
{
  Node *node;

  if (active) {
    node = get_node(client);
    if (node == NULL)
      return;

    node->ip_addr = *client;
    ++node->peer_hits;
    node->last_ntp_hit = now;
  }
}

/* ================================================== */

void
CLG_LogCommandAccess(IPAddr *client, CLG_Command_Type type, time_t now)
{
  Node *node;

  if (active) {
    node = get_node(client);
    if (node == NULL)
      return;

    node->ip_addr = *client;
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
        assert(0);
        break;
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
