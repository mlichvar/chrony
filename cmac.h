/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2019
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

  Header file for CMAC.

  */

#ifndef GOT_CMAC_H
#define GOT_CMAC_H

typedef struct CMC_Instance_Record *CMC_Instance;

extern unsigned int CMC_GetKeyLength(const char *cipher);
extern CMC_Instance CMC_CreateInstance(const char *cipher, const unsigned char *key,
                                       unsigned int length);
extern unsigned int CMC_Hash(CMC_Instance inst, const unsigned char *in, unsigned int in_len,
                             unsigned char *out, unsigned int out_len);
extern void CMC_DestroyInstance(CMC_Instance inst);

#endif

