/*
  $Header: /cvs/src/chrony/addrfilt.c,v 1.8 2002/02/28 23:27:08 richard Exp $

  =======================================================================

  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997,1998,1999,2000,2001,2002,2005
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

  This module provides a set of routines for checking IP addresses
  against a set of rules and deciding whether they are allowed or
  disallowed.

  */

#include "sysincl.h"

#include "addrfilt.h"
#include "memory.h"

/* Define the number of bits which are stripped off per level of
   indirection in the tables */
#define NBITS 4

/* Define the table size */
#define TABLE_SIZE (1UL<<NBITS)

typedef enum {DENY, ALLOW, AS_PARENT} State;

typedef struct _TableNode {
  State state;
  struct _TableNode *extended;
} TableNode;

struct ADF_AuthTableInst {
  TableNode base;
};

/* ================================================== */

inline static unsigned long
get_subnet(unsigned long addr)
{
  return (addr >> (32-NBITS)) & ((1UL<<NBITS) - 1);
}

/* ================================================== */

inline static unsigned long
get_residual(unsigned long addr)
{
  return (addr << NBITS);
}

/* ================================================== */

ADF_AuthTable
ADF_CreateTable(void)
{
  ADF_AuthTable result;
  result = MallocNew(struct ADF_AuthTableInst);

  /* Default is that nothing is allowed */
  result->base.state = DENY;
  result->base.extended = NULL;

  return result;
}

/* ================================================== */
/* This function deletes all definitions of child nodes, in effect
   pruning a whole subnet definition back to a single parent
   record. */
static void
close_node(TableNode *node)
{
  int i;
  TableNode *child_node;

  if (node->extended != NULL) {
    for (i=0; i<TABLE_SIZE; i++) {
      child_node = &(node->extended[i]);
      close_node(child_node);
    }
    Free(node->extended);
    node->extended = NULL;
  }

  return;
}


/* ================================================== */
/* Allocate the extension field in a node, and set all the children's
   states to default to that of the node being extended */

static void
open_node(TableNode *node)
{
  int i;
  TableNode *child_node;

  if (node->extended == NULL) {

    node->extended = MallocArray(struct _TableNode, TABLE_SIZE);

    for (i=0; i<TABLE_SIZE; i++) {
      child_node = &(node->extended[i]);
      child_node->state = AS_PARENT;
      child_node->extended = NULL;
    }
  }
  return;
}

/* ================================================== */

static ADF_Status
set_subnet(TableNode *start_node,
           unsigned long ip,
           int subnet_bits,
           State new_state,
           int delete_children)
{
  int bits_to_go;
  unsigned long residual;
  unsigned long subnet;
  TableNode *node;

  bits_to_go = subnet_bits;
  residual = ip;
  node = start_node;

  if ((subnet_bits < 0) ||
      (subnet_bits > 32)) {

    return ADF_BADSUBNET;

  } else {

    if ((bits_to_go & (NBITS-1)) == 0) {
    
      while (bits_to_go > 0) {
        subnet = get_subnet(residual);
        residual = get_residual(residual);
        if (!(node->extended)) {
          open_node(node);
        }
        node = &(node->extended[subnet]);
        bits_to_go -= NBITS;
      }

      if (delete_children) {
        close_node(node);
      }
      node->state = new_state;

    } else { /* Have to set multiple entries */
      int N, i, j;
      TableNode *this_node;

      while (bits_to_go >= NBITS) {
        subnet = get_subnet(residual);
        residual = get_residual(residual);
        if (!(node->extended)) {
          open_node(node);
        }
        node = &(node->extended[subnet]);
        bits_to_go -= NBITS;
      }

      /* How many subnet entries to set : 1->8, 2->4, 3->2 */
      N = 1 << (NBITS-bits_to_go);
      subnet = get_subnet(residual);
      if (!(node->extended)) {
        open_node(node);
      }
      
      for (i=subnet, j=0; j<N; i++, j++) {
        this_node = &(node->extended[i]);
        if (delete_children) {
          close_node(this_node);
        }
        this_node->state = new_state;
      }
    }
    
    return ADF_SUCCESS;
  }
  
}

/* ================================================== */

ADF_Status
ADF_Allow(ADF_AuthTable table,
          unsigned long ip,
          int subnet_bits)
{
  return set_subnet(&(table->base), ip, subnet_bits, ALLOW, 0);
}

/* ================================================== */


ADF_Status
ADF_AllowAll(ADF_AuthTable table,
             unsigned long ip,
             int subnet_bits)
{
  return set_subnet(&(table->base), ip, subnet_bits, ALLOW, 1);
}

/* ================================================== */

ADF_Status
ADF_Deny(ADF_AuthTable table,
         unsigned long ip,
         int subnet_bits)
{
  return set_subnet(&(table->base), ip, subnet_bits, DENY, 0);
}

/* ================================================== */

ADF_Status
ADF_DenyAll(ADF_AuthTable table,
            unsigned long ip,
            int subnet_bits)
{
  return set_subnet(&(table->base), ip, subnet_bits, DENY, 1);
}

/* ================================================== */

void
ADF_DestroyTable(ADF_AuthTable table)
{
  close_node(&(table->base));
  Free(table);
}

/* ================================================== */

static int
check_ip_in_node(TableNode *start_node, unsigned long ip)
{
  unsigned long residual, subnet;
  int result = 0;
  int finished = 0;
  TableNode *node;
  State state=DENY;

  node = start_node;
  residual = ip;

  do {
    if (node->state != AS_PARENT) {
      state = node->state;
    }
    if (node->extended) {
      subnet = get_subnet(residual);
      residual = get_residual(residual);
      node = &(node->extended[subnet]);
    } else {
      /* Make decision on this node */
      finished = 1;
    }
  } while (!finished);

  switch (state) {
    case ALLOW:
      result = 1;
      break;
    case DENY:
      result = 0;
      break;
    case AS_PARENT:
      assert(0);
      break;
  }

  return result;
}


/* ================================================== */

int
ADF_IsAllowed(ADF_AuthTable table,
              unsigned long ip)
{

  return check_ip_in_node(&(table->base), ip);

}

/* ================================================== */

#if defined TEST

static void print_node(TableNode *node, unsigned long addr, int shift, int subnet_bits)
{
  unsigned long new_addr;
  int i;
  TableNode *sub_node;

  for (i=0; i<subnet_bits; i++) putchar(' ');

  printf("%d.%d.%d.%d/%d : %s\n",
         ((addr >> 24) & 255),
         ((addr >> 16) & 255),
         ((addr >>  8) & 255),
         ((addr      ) & 255),
         subnet_bits,
         (node->state == ALLOW) ? "allow" :
         (node->state == DENY)  ? "deny" : "as parent");
  if (node->extended) {
    for (i=0; i<16; i++) {
      sub_node = &((*(node->extended))[i]);
      new_addr = addr | ((unsigned long) i << shift);
      print_node(sub_node, new_addr, shift - 4, subnet_bits + 4);
    }
  }
  return;
}


static void print_table(ADF_AuthTable table)
{
  unsigned long addr = 0;
  int shift = 28;
  int subnet_bits = 0;

  print_node(&table->base, addr, shift, subnet_bits);
  return;
}

/* ================================================== */

int main (int argc, char **argv)
{
  ADF_AuthTable table;
  table = ADF_CreateTable();

  ADF_Allow(table, 0x7e800000, 9);
  ADF_Deny(table, 0x7ecc0000, 14);
  /* ADF_Deny(table, 0x7f000001, 32); */
  /* ADF_Allow(table, 0x7f000000, 8); */

  print_table(table);

  ADF_DestroyTable(table);
  return 0;
}



#endif /* defined TEST */




