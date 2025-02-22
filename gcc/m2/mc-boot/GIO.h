/* do not edit automatically generated by mc from IO.  */
/* IO.def provides Read, Write, Errors procedures mapping onto 0, 1 and 2.

Copyright (C) 2001-2025 Free Software Foundation, Inc.
Contributed by Gaius Mulley <gaius.mulley@southwales.ac.uk>.

This file is part of GNU Modula-2.

GNU Modula-2 is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GNU Modula-2 is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

Under Section 7 of GPL version 3, you are granted additional
permissions described in the GCC Runtime Library Exception, version
3.1, as published by the Free Software Foundation.

You should have received a copy of the GNU General Public License and
a copy of the GCC Runtime Library Exception along with this program;
see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
<http://www.gnu.org/licenses/>.  */


#if !defined (_IO_H)
#   define _IO_H

#include "config.h"
#include "system.h"
#   ifdef __cplusplus
extern "C" {
#   endif
#include <stdbool.h>
#   if !defined (PROC_D)
#      define PROC_D
       typedef void (*PROC_t) (void);
       typedef struct { PROC_t proc; } PROC;
#   endif


#   if defined (_IO_C)
#      define EXTERN
#   else
#      define EXTERN extern
#   endif

EXTERN void IO_Read (char *ch);
EXTERN void IO_Write (char ch);
EXTERN void IO_Error (char ch);

/*
   UnBufferedMode - places file descriptor, fd, into an unbuffered mode.
*/

EXTERN void IO_UnBufferedMode (int fd, bool input);

/*
   BufferedMode - places file descriptor, fd, into a buffered mode.
*/

EXTERN void IO_BufferedMode (int fd, bool input);

/*
   EchoOn - turns on echoing for file descriptor, fd.  This
            only really makes sence for a file descriptor opened
            for terminal input or maybe some specific file descriptor
            which is attached to a particular piece of hardware.
*/

EXTERN void IO_EchoOn (int fd, bool input);

/*
   EchoOff - turns off echoing for file descriptor, fd.  This
             only really makes sence for a file descriptor opened
             for terminal input or maybe some specific file descriptor
             which is attached to a particular piece of hardware.
*/

EXTERN void IO_EchoOff (int fd, bool input);
#   ifdef __cplusplus
}
#   endif

#   undef EXTERN
#endif
