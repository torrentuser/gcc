/* Functions used by the Windows port of libgccjit.
   Copyright (C) 2020-2025 Free Software Foundation, Inc.
   Contributed by Nicolas Bertolo <nicolasbertolo@gmail.com>.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace gcc {
namespace jit {
extern void print_last_error (void);
extern char * win_mkdtemp(void);
}
}
