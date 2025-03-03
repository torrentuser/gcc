/* Common macros for target hook definitions.
   Copyright (C) 2001-2025 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 3, or (at your option) any
   later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING3.  If not see
   <http://www.gnu.org/licenses/>.  */

/* The following macros should be provided by the including file:

   DEFHOOK(NAME, DOC, TYPE, PARAMS, INIT): Define a function-valued hook.
   DEFHOOKPOD(NAME, DOC, TYPE, INIT): Define a piece-of-data 'hook'.  */

/* Defaults for optional macros:
   DEFHOOKPODX(NAME, TYPE, INIT): Like DEFHOOKPOD, but share documentation
   with the previous 'hook'.  */
#ifndef DEFHOOKPODX
#define DEFHOOKPODX(NAME, TYPE, INIT) DEFHOOKPOD (NAME, 0, TYPE, INIT)
#endif

/* HOOKSTRUCT(FRAGMENT): Declarator fragments to encapsulate all the
   members into a struct gcc_target, which in turn contains several
   sub-structs.  */
#ifndef HOOKSTRUCT
#define HOOKSTRUCT(FRAGMENT)
#endif
/* HOOK_VECTOR: Start a struct declaration, which then gets its own initializer.
   HOOK_VECTOR_END: Close a struct declaration, providing a member declarator
                    name for nested use.  */
#ifndef HOOK_VECTOR_1
#define HOOK_VECTOR_1(NAME, FRAGMENT) HOOKSTRUCT (FRAGMENT)
#endif
#define HOOK_VECTOR(INIT_NAME, SNAME) HOOK_VECTOR_1 (INIT_NAME, struct SNAME {)
#define HOOK_VECTOR_END(DECL_NAME) HOOK_VECTOR_1(,} DECL_NAME ;)

/* FIXME: For pre-existing hooks, we can't place the documentation in the
   documentation field here till we get permission from the FSF to include
   it in GPLed software - the target hook documentation is so far only
   available under the GFDL.  */

/* A hook should generally be documented by a string in the DOC parameter,
   which should contain texinfo markup.  If the documentation is only available
   under the GPL, but not under the GFDL, put it in a comment above the hook
   definition.  If the function declaration is available both under GPL and
   GFDL, but the documentation is only available under the GFDL, put the
   documentaton in tm.texi.in, heading with @hook <hookname> and closing
   the paragraph with @end deftypefn / deftypevr as appropriate, and marking
   the next autogenerated hook with @hook <hookname>.
   In both these cases, leave the DOC string empty, i.e. "".
   Sometimes, for some historic reason the function declaration
   has to be documented differently
   than what it is.  In that case, use DEFHOOK_UNDOC to suppress auto-generation
   of documentation.  DEFHOOK_UNDOC takes a DOC string which it ignores, so
   you can put GPLed documentation string there if you have hopes that you
   can clear the declaration & documentation for GFDL distribution later,
   in which case you can then simply change the DEFHOOK_UNDOC to DEFHOOK
   to turn on the autogeneration of the documentation.

    A documentation string of "*" means not to emit any documentation at all,
   and is mainly used internally for DEFHOOK_UNDOC.  It should generally not
   be used otherwise, but it has its use for exceptional cases where automatic
   documentation is not wanted, and the real documentation is elsewere, like
   for TARGET_ASM_{,UN}ALIGNED_INT_OP, which are hooks only for implementation
   purposes; they refer to structs, the components of which are documented as
   separate hooks TARGET_ASM_{,UN}ALIGNED_[HSDT]I_OP.
   A DOC string of 0 is for internal use of DEFHOOKPODX and special table
   entries only.  */

/* Empty macro arguments are undefined in C90, so use an empty macro
   to close top-level hook structures.  */
#define C90_EMPTY_HACK
