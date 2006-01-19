/*
  $Header: /cvs/src/chrony/mkdirpp.c,v 1.10 2002/11/03 22:49:17 richard Exp $

  =======================================================================

  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2002
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

  A function for creating a directory and any parent directories that
  don't exist.

  */

#include "sysincl.h"

#include "mkdirpp.h"

static int
do_dir(char *p)
{
  int status;
  struct stat buf;

#if defined(TEST)
  fprintf(stderr, "do_dir(%s)\n", p);
#endif

  /* See if directory exists */
  status = stat(p, &buf);

  if (status < 0) {
    if (errno == ENOENT) {
      /* Try to create directory */
      status = mkdir(p, 0755);
      return status;
    } else {
      return status;
    }
  }

  if (!S_ISDIR(buf.st_mode)) {
    return -1;
  }

  return 0;
}

/* ================================================== */
/* Return 0 if the directory couldn't be created, 1 if it could (or
   already existed) */

int
mkdir_and_parents(const char *path)
{
  char *p;
  int len;
  int i, j, k, last;
  len = strlen(path);

  p = (char *) malloc(1 + len);

  i = k = 0;
  while (1) {
    p[i++] = path[k++];
    
    if (path[k] == '/' || !path[k]) {
      p[i] = 0;

      if (do_dir(p) < 0) {
        return 0;
      }

      if (!path[k]) {
        /* End of the string */
        break;
      }

      /* check whether its a trailing / or group of / */
      last = 1;
      j = k+1;
      while (path[j]) {
        if (path[j] != '/') {
          k = j - 1; /* Pick up a / into p[] thru the assignment at the top of the loop */
          last = 0;
          break;
        }
        j++;
      }

      if (last) {
        break;
      }
    }

    if (!path[k]) break;

  }  

  free(p);
  return 1;

}

/* ================================================== */

#if defined(TEST)
int main(int argc, char **argv) {
  if (argc > 1) {
    /* Invert sense of result */
    return mkdir_and_parents(argv[1]) ? 0 : 1;
  } else {
    return 1;
  }
}
#endif
                                  
