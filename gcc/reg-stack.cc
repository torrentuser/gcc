/* Register to Stack convert for GNU compiler.
   Copyright (C) 1992-2025 Free Software Foundation, Inc.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   GCC is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING3.  If not see
   <http://www.gnu.org/licenses/>.  */

/* This pass converts stack-like registers from the "flat register
   file" model that gcc uses, to a stack convention that the 387 uses.

   * The form of the input:

   On input, the function consists of insn that have had their
   registers fully allocated to a set of "virtual" registers.  Note that
   the word "virtual" is used differently here than elsewhere in gcc: for
   each virtual stack reg, there is a hard reg, but the mapping between
   them is not known until this pass is run.  On output, hard register
   numbers have been substituted, and various pop and exchange insns have
   been emitted.  The hard register numbers and the virtual register
   numbers completely overlap - before this pass, all stack register
   numbers are virtual, and afterward they are all hard.

   The virtual registers can be manipulated normally by gcc, and their
   semantics are the same as for normal registers.  After the hard
   register numbers are substituted, the semantics of an insn containing
   stack-like regs are not the same as for an insn with normal regs: for
   instance, it is not safe to delete an insn that appears to be a no-op
   move.  In general, no insn containing hard regs should be changed
   after this pass is done.

   * The form of the output:

   After this pass, hard register numbers represent the distance from
   the current top of stack to the desired register.  A reference to
   FIRST_STACK_REG references the top of stack, FIRST_STACK_REG + 1,
   represents the register just below that, and so forth.  Also, REG_DEAD
   notes indicate whether or not a stack register should be popped.

   A "swap" insn looks like a parallel of two patterns, where each
   pattern is a SET: one sets A to B, the other B to A.

   A "push" or "load" insn is a SET whose SET_DEST is FIRST_STACK_REG
   and whose SET_DEST is REG or MEM.  Any other SET_DEST, such as PLUS,
   will replace the existing stack top, not push a new value.

   A store insn is a SET whose SET_DEST is FIRST_STACK_REG, and whose
   SET_SRC is REG or MEM.

   The case where the SET_SRC and SET_DEST are both FIRST_STACK_REG
   appears ambiguous.  As a special case, the presence of a REG_DEAD note
   for FIRST_STACK_REG differentiates between a load insn and a pop.

   If a REG_DEAD is present, the insn represents a "pop" that discards
   the top of the register stack.  If there is no REG_DEAD note, then the
   insn represents a "dup" or a push of the current top of stack onto the
   stack.

   * Methodology:

   Existing REG_DEAD and REG_UNUSED notes for stack registers are
   deleted and recreated from scratch.  REG_DEAD is never created for a
   SET_DEST, only REG_UNUSED.

   * asm_operands:

   There are several rules on the usage of stack-like regs in
   asm_operands insns.  These rules apply only to the operands that are
   stack-like regs:

   1. Given a set of input regs that die in an asm_operands, it is
      necessary to know which are implicitly popped by the asm, and
      which must be explicitly popped by gcc.

	An input reg that is implicitly popped by the asm must be
	explicitly clobbered, unless it is constrained to match an
	output operand.

   2. For any input reg that is implicitly popped by an asm, it is
      necessary to know how to adjust the stack to compensate for the pop.
      If any non-popped input is closer to the top of the reg-stack than
      the implicitly popped reg, it would not be possible to know what the
      stack looked like - it's not clear how the rest of the stack "slides
      up".

	All implicitly popped input regs must be closer to the top of
	the reg-stack than any input that is not implicitly popped.

	All explicitly referenced input operands may not "skip" a reg.
	Otherwise we can have holes in the stack.

   3. It is possible that if an input dies in an insn, reload might
      use the input reg for an output reload.  Consider this example:

		asm ("foo" : "=t" (a) : "f" (b));

      This asm says that input B is not popped by the asm, and that
      the asm pushes a result onto the reg-stack, i.e., the stack is one
      deeper after the asm than it was before.  But, it is possible that
      reload will think that it can use the same reg for both the input and
      the output, if input B dies in this insn.

	If any input operand uses the "f" constraint, all output reg
	constraints must use the "&" earlyclobber.

      The asm above would be written as

		asm ("foo" : "=&t" (a) : "f" (b));

   4. Some operands need to be in particular places on the stack.  All
      output operands fall in this category - there is no other way to
      know which regs the outputs appear in unless the user indicates
      this in the constraints.

	Output operands must specifically indicate which reg an output
	appears in after an asm.  "=f" is not allowed: the operand
	constraints must select a class with a single reg.

   5. Output operands may not be "inserted" between existing stack regs.
      Since no 387 opcode uses a read/write operand, all output operands
      are dead before the asm_operands, and are pushed by the asm_operands.
      It makes no sense to push anywhere but the top of the reg-stack.

	Output operands must start at the top of the reg-stack: output
	operands may not "skip" a reg.

   6. Some asm statements may need extra stack space for internal
      calculations.  This can be guaranteed by clobbering stack registers
      unrelated to the inputs and outputs.

   Here are a couple of reasonable asms to want to write.  This asm
   takes one input, which is internally popped, and produces two outputs.

	asm ("fsincos" : "=t" (cos), "=u" (sin) : "0" (inp));

   This asm takes two inputs, which are popped by the fyl2xp1 opcode,
   and replaces them with one output.  The user must code the "st(1)"
   clobber for reg-stack.cc to know that fyl2xp1 pops both inputs.

	asm ("fyl2xp1" : "=t" (result) : "0" (x), "u" (y) : "st(1)");

*/

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "target.h"
#include "rtl.h"
#include "tree.h"
#include "df.h"
#include "insn-config.h"
#include "memmodel.h"
#include "regs.h"
#include "emit-rtl.h"  /* FIXME: Can go away once crtl is moved to rtl.h.  */
#include "recog.h"
#include "varasm.h"
#include "rtl-error.h"
#include "cfgrtl.h"
#include "cfganal.h"
#include "cfgbuild.h"
#include "cfgcleanup.h"
#include "reload.h"
#include "tree-pass.h"
#include "rtl-iter.h"
#include "function-abi.h"

#ifdef STACK_REGS

/* We use this array to cache info about insns, because otherwise we
   spend too much time in stack_regs_mentioned_p.

   Indexed by insn UIDs.  A value of zero is uninitialized, one indicates
   the insn uses stack registers, two indicates the insn does not use
   stack registers.  */
static vec<char> stack_regs_mentioned_data;

#define REG_STACK_SIZE (LAST_STACK_REG - FIRST_STACK_REG + 1)

int regstack_completed = 0;

/* This is the basic stack record.  TOP is an index into REG[] such
   that REG[TOP] is the top of stack.  If TOP is -1 the stack is empty.

   If TOP is -2, REG[] is not yet initialized.  Stack initialization
   consists of placing each live reg in array `reg' and setting `top'
   appropriately.

   REG_SET indicates which registers are live.  */

typedef struct stack_def
{
  int top;			/* index to top stack element */
  HARD_REG_SET reg_set;		/* set of live registers */
  unsigned char reg[REG_STACK_SIZE];/* register - stack mapping */
} *stack_ptr;

/* This is used to carry information about basic blocks.  It is
   attached to the AUX field of the standard CFG block.  */

typedef struct block_info_def
{
  struct stack_def stack_in;	/* Input stack configuration.  */
  struct stack_def stack_out;	/* Output stack configuration.  */
  HARD_REG_SET out_reg_set;	/* Stack regs live on output.  */
  bool done;			/* True if block already converted.  */
  int predecessors;		/* Number of predecessors that need
				   to be visited.  */
} *block_info;

#define BLOCK_INFO(B)	((block_info) (B)->aux)

/* Passed to change_stack to indicate where to emit insns.  */
enum emit_where
{
  EMIT_AFTER,
  EMIT_BEFORE
};

/* The block we're currently working on.  */
static basic_block current_block;

/* In the current_block, whether we're processing the first register
   stack or call instruction, i.e. the regstack is currently the
   same as BLOCK_INFO(current_block)->stack_in.  */
static bool starting_stack_p;

/* This is the register file for all register after conversion.  */
static rtx
  FP_mode_reg[LAST_STACK_REG+1-FIRST_STACK_REG][(int) MAX_MACHINE_MODE];

#define FP_MODE_REG(regno,mode)	\
  (FP_mode_reg[(regno)-FIRST_STACK_REG][(int) (mode)])

/* Used to initialize uninitialized registers.  */
static rtx not_a_num;

/* Forward declarations */

static bool stack_regs_mentioned_p (const_rtx pat);
static void pop_stack (stack_ptr, int);
static rtx *get_true_reg (rtx *);

static bool check_asm_stack_operands (rtx_insn *);
static void get_asm_operands_in_out (rtx, int *, int *);
static rtx stack_result (tree);
static void replace_reg (rtx *, int);
static void remove_regno_note (rtx_insn *, enum reg_note, unsigned int);
static int get_hard_regnum (stack_ptr, rtx);
static rtx_insn *emit_pop_insn (rtx_insn *, stack_ptr, rtx, enum emit_where);
static void swap_to_top (rtx_insn *, stack_ptr, rtx, rtx);
static bool move_for_stack_reg (rtx_insn *, stack_ptr, rtx);
static bool move_nan_for_stack_reg (rtx_insn *, stack_ptr, rtx);
static bool swap_rtx_condition_1 (rtx);
static bool swap_rtx_condition (rtx_insn *, int &);
static void compare_for_stack_reg (rtx_insn *, stack_ptr, rtx, bool);
static bool subst_stack_regs_pat (rtx_insn *, stack_ptr, rtx);
static void subst_asm_stack_regs (rtx_insn *, stack_ptr);
static bool subst_stack_regs (rtx_insn *, stack_ptr);
static void change_stack (rtx_insn *, stack_ptr, stack_ptr, enum emit_where);
static void print_stack (FILE *, stack_ptr);
static rtx_insn *next_flags_user (rtx_insn *, int &);

/* Return true if any stack register is mentioned somewhere within PAT.  */

static bool
stack_regs_mentioned_p (const_rtx pat)
{
  const char *fmt;
  int i;

  if (STACK_REG_P (pat))
    return true;

  fmt = GET_RTX_FORMAT (GET_CODE (pat));
  for (i = GET_RTX_LENGTH (GET_CODE (pat)) - 1; i >= 0; i--)
    {
      if (fmt[i] == 'E')
	{
	  int j;

	  for (j = XVECLEN (pat, i) - 1; j >= 0; j--)
	    if (stack_regs_mentioned_p (XVECEXP (pat, i, j)))
	      return true;
	}
      else if (fmt[i] == 'e' && stack_regs_mentioned_p (XEXP (pat, i)))
	return true;
    }

  return false;
}

/* Return true if INSN mentions stacked registers, else return zero.  */

bool
stack_regs_mentioned (const_rtx insn)
{
  unsigned int uid, max;
  int test;

  if (! INSN_P (insn) || !stack_regs_mentioned_data.exists ())
    return false;

  uid = INSN_UID (insn);
  max = stack_regs_mentioned_data.length ();
  if (uid >= max)
    {
      /* Allocate some extra size to avoid too many reallocs, but
	 do not grow too quickly.  */
      max = uid + uid / 20 + 1;
      stack_regs_mentioned_data.safe_grow_cleared (max, true);
    }

  test = stack_regs_mentioned_data[uid];
  if (test == 0)
    {
      /* This insn has yet to be examined.  Do so now.  */
      test = stack_regs_mentioned_p (PATTERN (insn)) ? 1 : 2;
      stack_regs_mentioned_data[uid] = test;
    }

  return test == 1;
}

static rtx ix86_flags_rtx;

static rtx_insn *
next_flags_user (rtx_insn *insn, int &debug_seen)
{
  /* Search forward looking for the first use of this value.
     Stop at block boundaries.  */

  while (insn != BB_END (current_block))
    {
      insn = NEXT_INSN (insn);

      if (INSN_P (insn) && reg_mentioned_p (ix86_flags_rtx, PATTERN (insn)))
	{
	  if (DEBUG_INSN_P (insn) && debug_seen >= 0)
	    {
	      debug_seen = 1;
	      continue;
	    }
	  return insn;
	}

      if (CALL_P (insn))
	return NULL;
    }
  return NULL;
}

/* Reorganize the stack into ascending numbers, before this insn.  */

static void
straighten_stack (rtx_insn *insn, stack_ptr regstack)
{
  struct stack_def temp_stack;
  int top;

  /* If there is only a single register on the stack, then the stack is
     already in increasing order and no reorganization is needed.

     Similarly if the stack is empty.  */
  if (regstack->top <= 0)
    return;

  temp_stack.reg_set = regstack->reg_set;

  for (top = temp_stack.top = regstack->top; top >= 0; top--)
    temp_stack.reg[top] = FIRST_STACK_REG + temp_stack.top - top;

  change_stack (insn, regstack, &temp_stack, EMIT_BEFORE);
}

/* Pop a register from the stack.  */

static void
pop_stack (stack_ptr regstack, int regno)
{
  int top = regstack->top;

  CLEAR_HARD_REG_BIT (regstack->reg_set, regno);
  regstack->top--;
  /* If regno was not at the top of stack then adjust stack.  */
  if (regstack->reg [top] != regno)
    {
      int i;
      for (i = regstack->top; i >= 0; i--)
	if (regstack->reg [i] == regno)
	  {
	    int j;
	    for (j = i; j < top; j++)
	      regstack->reg [j] = regstack->reg [j + 1];
	    break;
	  }
    }
}

/* Return a pointer to the REG expression within PAT.  If PAT is not a
   REG, possible enclosed by a conversion rtx, return the inner part of
   PAT that stopped the search.  */

static rtx *
get_true_reg (rtx *pat)
{
  for (;;)
    switch (GET_CODE (*pat))
      {
      case SUBREG:
	/* Eliminate FP subregister accesses in favor of the
	   actual FP register in use.  */
	{
	  rtx subreg = SUBREG_REG (*pat);

	  if (STACK_REG_P (subreg))
	    {
	      int regno_off = subreg_regno_offset (REGNO (subreg),
						   GET_MODE (subreg),
						   SUBREG_BYTE (*pat),
						   GET_MODE (*pat));
	      *pat = FP_MODE_REG (REGNO (subreg) + regno_off,
				  GET_MODE (subreg));
	      return pat;
	    }
	  pat = &XEXP (*pat, 0);
	  break;
	}

      case FLOAT_TRUNCATE:
	if (!flag_unsafe_math_optimizations)
	  return pat;
	/* FALLTHRU */

      case FLOAT:
      case FIX:
      case FLOAT_EXTEND:
	pat = &XEXP (*pat, 0);
	break;

      case UNSPEC:
	if (XINT (*pat, 1) == UNSPEC_TRUNC_NOOP
	    || XINT (*pat, 1) == UNSPEC_FILD_ATOMIC)
	  pat = &XVECEXP (*pat, 0, 0);
	return pat;

      default:
	return pat;
      }
}

/* Set if we find any malformed asms in a function.  */
static bool any_malformed_asm;

/* There are many rules that an asm statement for stack-like regs must
   follow.  Those rules are explained at the top of this file: the rule
   numbers below refer to that explanation.  */

static bool
check_asm_stack_operands (rtx_insn *insn)
{
  int i;
  int n_clobbers;
  bool malformed_asm = false;
  rtx body = PATTERN (insn);

  char reg_used_as_output[FIRST_PSEUDO_REGISTER];
  char implicitly_dies[FIRST_PSEUDO_REGISTER];
  char explicitly_used[FIRST_PSEUDO_REGISTER];

  rtx *clobber_reg = 0;
  int n_inputs, n_outputs;

  /* Find out what the constraints require.  If no constraint
     alternative matches, this asm is malformed.  */
  extract_constrain_insn (insn);

  preprocess_constraints (insn);

  get_asm_operands_in_out (body, &n_outputs, &n_inputs);

  if (which_alternative < 0)
    {
      /* Avoid further trouble with this insn.  */
      PATTERN (insn) = gen_rtx_USE (VOIDmode, const0_rtx);
      return 0;
    }
  const operand_alternative *op_alt = which_op_alt ();

  /* Strip SUBREGs here to make the following code simpler.  */
  for (i = 0; i < recog_data.n_operands; i++)
    if (GET_CODE (recog_data.operand[i]) == SUBREG
	&& REG_P (SUBREG_REG (recog_data.operand[i])))
      recog_data.operand[i] = SUBREG_REG (recog_data.operand[i]);

  /* Set up CLOBBER_REG.  */

  n_clobbers = 0;

  if (GET_CODE (body) == PARALLEL)
    {
      clobber_reg = XALLOCAVEC (rtx, XVECLEN (body, 0));

      for (i = 0; i < XVECLEN (body, 0); i++)
	if (GET_CODE (XVECEXP (body, 0, i)) == CLOBBER)
	  {
	    rtx clobber = XVECEXP (body, 0, i);
	    rtx reg = XEXP (clobber, 0);

	    if (GET_CODE (reg) == SUBREG && REG_P (SUBREG_REG (reg)))
	      reg = SUBREG_REG (reg);

	    if (STACK_REG_P (reg))
	      {
		clobber_reg[n_clobbers] = reg;
		n_clobbers++;
	      }
	  }
    }

  /* Enforce rule #4: Output operands must specifically indicate which
     reg an output appears in after an asm.  "=f" is not allowed: the
     operand constraints must select a class with a single reg.

     Also enforce rule #5: Output operands must start at the top of
     the reg-stack: output operands may not "skip" a reg.  */

  memset (reg_used_as_output, 0, sizeof (reg_used_as_output));
  for (i = 0; i < n_outputs; i++)
    if (STACK_REG_P (recog_data.operand[i]))
      {
	if (reg_class_size[(int) op_alt[i].cl] != 1)
	  {
	    error_for_asm (insn, "output constraint %d must specify a single register", i);
	    malformed_asm = true;
	  }
	else
	  {
	    int j;

	    for (j = 0; j < n_clobbers; j++)
	      if (REGNO (recog_data.operand[i]) == REGNO (clobber_reg[j]))
		{
		  error_for_asm (insn, "output constraint %d cannot be "
				 "specified together with %qs clobber",
				 i, reg_names [REGNO (clobber_reg[j])]);
		  malformed_asm = true;
		  break;
		}
	    if (j == n_clobbers)
	      reg_used_as_output[REGNO (recog_data.operand[i])] = 1;
	  }
      }


  /* Search for first non-popped reg.  */
  for (i = FIRST_STACK_REG; i < LAST_STACK_REG + 1; i++)
    if (! reg_used_as_output[i])
      break;

  /* If there are any other popped regs, that's an error.  */
  for (; i < LAST_STACK_REG + 1; i++)
    if (reg_used_as_output[i])
      break;

  if (i != LAST_STACK_REG + 1)
    {
      error_for_asm (insn, "output registers must be grouped at top of stack");
      malformed_asm = true;
    }

  /* Enforce rule #2: All implicitly popped input regs must be closer
     to the top of the reg-stack than any input that is not implicitly
     popped.  */

  memset (implicitly_dies, 0, sizeof (implicitly_dies));
  memset (explicitly_used, 0, sizeof (explicitly_used));
  for (i = n_outputs; i < n_outputs + n_inputs; i++)
    if (STACK_REG_P (recog_data.operand[i]))
      {
	/* An input reg is implicitly popped if it is tied to an
	   output, or if there is a CLOBBER for it.  */
	int j;

	for (j = 0; j < n_clobbers; j++)
	  if (operands_match_p (clobber_reg[j], recog_data.operand[i]))
	    break;

	if (j < n_clobbers || op_alt[i].matches >= 0)
	  implicitly_dies[REGNO (recog_data.operand[i])] = 1;
	else if (reg_class_size[(int) op_alt[i].cl] == 1)
	  explicitly_used[REGNO (recog_data.operand[i])] = 1;
      }

  /* Search for first non-popped reg.  */
  for (i = FIRST_STACK_REG; i < LAST_STACK_REG + 1; i++)
    if (! implicitly_dies[i])
      break;

  /* If there are any other popped regs, that's an error.  */
  for (; i < LAST_STACK_REG + 1; i++)
    if (implicitly_dies[i])
      break;

  if (i != LAST_STACK_REG + 1)
    {
      error_for_asm (insn,
		     "implicitly popped registers must be grouped "
		     "at top of stack");
      malformed_asm = true;
    }

  /* Search for first not-explicitly used reg.  */
  for (i = FIRST_STACK_REG; i < LAST_STACK_REG + 1; i++)
    if (! implicitly_dies[i] && ! explicitly_used[i])
      break;

  /* If there are any other explicitly used regs, that's an error.  */
  for (; i < LAST_STACK_REG + 1; i++)
    if (explicitly_used[i])
      break;

  if (i != LAST_STACK_REG + 1)
    {
      error_for_asm (insn,
		     "explicitly used registers must be grouped "
		     "at top of stack");
      malformed_asm = true;
    }

  /* Enforce rule #3: If any input operand uses the "f" constraint, all
     output constraints must use the "&" earlyclobber.

     ??? Detect this more deterministically by having constrain_asm_operands
     record any earlyclobber.  */

  for (i = n_outputs; i < n_outputs + n_inputs; i++)
    if (STACK_REG_P (recog_data.operand[i]) && op_alt[i].matches == -1)
      {
	int j;

	for (j = 0; j < n_outputs; j++)
	  if (operands_match_p (recog_data.operand[j], recog_data.operand[i]))
	    {
	      error_for_asm (insn,
			     "output operand %d must use %<&%> constraint", j);
	      malformed_asm = true;
	    }
      }

  if (malformed_asm)
    {
      /* Avoid further trouble with this insn.  */
      PATTERN (insn) = gen_rtx_USE (VOIDmode, const0_rtx);
      any_malformed_asm = true;
      return false;
    }

  return true;
}

/* Calculate the number of inputs and outputs in BODY, an
   asm_operands.  N_OPERANDS is the total number of operands, and
   N_INPUTS and N_OUTPUTS are pointers to ints into which the results are
   placed.  */

static void
get_asm_operands_in_out (rtx body, int *pout, int *pin)
{
  rtx asmop = extract_asm_operands (body);

  *pin = ASM_OPERANDS_INPUT_LENGTH (asmop);
  *pout = (recog_data.n_operands
	   - ASM_OPERANDS_INPUT_LENGTH (asmop)
	   - ASM_OPERANDS_LABEL_LENGTH (asmop));
}

/* If current function returns its result in an fp stack register,
   return the REG.  Otherwise, return 0.  */

static rtx
stack_result (tree decl)
{
  rtx result;

  /* If the value is supposed to be returned in memory, then clearly
     it is not returned in a stack register.  */
  if (aggregate_value_p (DECL_RESULT (decl), decl))
    return 0;

  result = DECL_RTL_IF_SET (DECL_RESULT (decl));
  if (result != 0)
    result = targetm.calls.function_value (TREE_TYPE (DECL_RESULT (decl)),
					   decl, true);

  return result != 0 && STACK_REG_P (result) ? result : 0;
}


/*
 * This section deals with stack register substitution, and forms the second
 * pass over the RTL.
 */

/* Replace REG, which is a pointer to a stack reg RTX, with an RTX for
   the desired hard REGNO.  */

static void
replace_reg (rtx *reg, int regno)
{
  gcc_assert (IN_RANGE (regno, FIRST_STACK_REG, LAST_STACK_REG));
  gcc_assert (STACK_REG_P (*reg));

  gcc_assert (GET_MODE_CLASS (GET_MODE (*reg)) == MODE_FLOAT
	      || GET_MODE_CLASS (GET_MODE (*reg)) == MODE_COMPLEX_FLOAT);

  *reg = FP_MODE_REG (regno, GET_MODE (*reg));
}

/* Remove a note of type NOTE, which must be found, for register
   number REGNO from INSN.  Remove only one such note.  */

static void
remove_regno_note (rtx_insn *insn, enum reg_note note, unsigned int regno)
{
  rtx *note_link, this_rtx;

  note_link = &REG_NOTES (insn);
  for (this_rtx = *note_link; this_rtx; this_rtx = XEXP (this_rtx, 1))
    if (REG_NOTE_KIND (this_rtx) == note
	&& REG_P (XEXP (this_rtx, 0)) && REGNO (XEXP (this_rtx, 0)) == regno)
      {
	*note_link = XEXP (this_rtx, 1);
	return;
      }
    else
      note_link = &XEXP (this_rtx, 1);

  gcc_unreachable ();
}

/* Find the hard register number of virtual register REG in REGSTACK.
   The hard register number is relative to the top of the stack.  -1 is
   returned if the register is not found.  */

static int
get_hard_regnum (stack_ptr regstack, rtx reg)
{
  int i;

  gcc_assert (STACK_REG_P (reg));

  for (i = regstack->top; i >= 0; i--)
    if (regstack->reg[i] == REGNO (reg))
      break;

  return i >= 0 ? (FIRST_STACK_REG + regstack->top - i) : -1;
}

/* Emit an insn to pop virtual register REG before or after INSN.
   REGSTACK is the stack state after INSN and is updated to reflect this
   pop.  WHEN is either emit_insn_before or emit_insn_after.  A pop insn
   is represented as a SET whose destination is the register to be popped
   and source is the top of stack.  A death note for the top of stack
   cases the movdf pattern to pop.  */

static rtx_insn *
emit_pop_insn (rtx_insn *insn, stack_ptr regstack, rtx reg,
	       enum emit_where where)
{
  machine_mode raw_mode = reg_raw_mode[FIRST_STACK_REG];
  rtx_insn *pop_insn;
  rtx pop_rtx;
  int hard_regno;

  /* For complex types take care to pop both halves.  These may survive in
     CLOBBER and USE expressions.  */
  if (COMPLEX_MODE_P (GET_MODE (reg)))
    {
      rtx reg1 = FP_MODE_REG (REGNO (reg), raw_mode);
      rtx reg2 = FP_MODE_REG (REGNO (reg) + 1, raw_mode);

      pop_insn = NULL;
      if (get_hard_regnum (regstack, reg1) >= 0)
	pop_insn = emit_pop_insn (insn, regstack, reg1, where);
      if (get_hard_regnum (regstack, reg2) >= 0)
	pop_insn = emit_pop_insn (insn, regstack, reg2, where);
      gcc_assert (pop_insn);
      return pop_insn;
    }

  hard_regno = get_hard_regnum (regstack, reg);

  gcc_assert (hard_regno >= FIRST_STACK_REG);

  pop_rtx = gen_rtx_SET (FP_MODE_REG (hard_regno, raw_mode),
			 FP_MODE_REG (FIRST_STACK_REG, raw_mode));

  if (where == EMIT_AFTER)
    pop_insn = emit_insn_after (pop_rtx, insn);
  else
    pop_insn = emit_insn_before (pop_rtx, insn);

  add_reg_note (pop_insn, REG_DEAD, FP_MODE_REG (FIRST_STACK_REG, raw_mode));

  regstack->reg[regstack->top - (hard_regno - FIRST_STACK_REG)]
    = regstack->reg[regstack->top];
  regstack->top -= 1;
  CLEAR_HARD_REG_BIT (regstack->reg_set, REGNO (reg));

  return pop_insn;
}

/* Emit an insn before or after INSN to swap virtual register REG with
   the top of stack.  REGSTACK is the stack state before the swap, and
   is updated to reflect the swap.  A swap insn is represented as a
   PARALLEL of two patterns: each pattern moves one reg to the other.

   If REG is already at the top of the stack, no insn is emitted.  */

static void
emit_swap_insn (rtx_insn *insn, stack_ptr regstack, rtx reg)
{
  int hard_regno;
  int other_reg;		/* swap regno temps */
  rtx_insn *i1;			/* the stack-reg insn prior to INSN */
  rtx i1set = NULL_RTX;		/* the SET rtx within I1 */

  hard_regno = get_hard_regnum (regstack, reg);

  if (hard_regno == FIRST_STACK_REG)
    return;
  if (hard_regno == -1)
    {
      /* Something failed if the register wasn't on the stack.  If we had
	 malformed asms, we zapped the instruction itself, but that didn't
	 produce the same pattern of register sets as before.  To prevent
	 further failure, adjust REGSTACK to include REG at TOP.  */
      gcc_assert (any_malformed_asm);
      regstack->reg[++regstack->top] = REGNO (reg);
      return;
    }
  gcc_assert (hard_regno >= FIRST_STACK_REG);

  other_reg = regstack->top - (hard_regno - FIRST_STACK_REG);
  std::swap (regstack->reg[regstack->top], regstack->reg[other_reg]);

  /* Find the previous insn involving stack regs, but don't pass a
     block boundary.  */
  i1 = NULL;
  if (current_block && insn != BB_HEAD (current_block))
    {
      rtx_insn *tmp = PREV_INSN (insn);
      rtx_insn *limit = PREV_INSN (BB_HEAD (current_block));
      while (tmp != limit)
	{
	  if (LABEL_P (tmp)
	      || CALL_P (tmp)
	      || NOTE_INSN_BASIC_BLOCK_P (tmp)
	      || (NONJUMP_INSN_P (tmp)
		  && stack_regs_mentioned (tmp)))
	    {
	      i1 = tmp;
	      break;
	    }
	  tmp = PREV_INSN (tmp);
	}
    }

  if (i1 != NULL_RTX
      && (i1set = single_set (i1)) != NULL_RTX)
    {
      rtx i1src = *get_true_reg (&SET_SRC (i1set));
      rtx i1dest = *get_true_reg (&SET_DEST (i1set));

      /* If the previous register stack push was from the reg we are to
	 swap with, omit the swap.  */

      if (REG_P (i1dest) && REGNO (i1dest) == FIRST_STACK_REG
	  && REG_P (i1src)
	  && REGNO (i1src) == (unsigned) hard_regno - 1
	  && find_regno_note (i1, REG_DEAD, FIRST_STACK_REG) == NULL_RTX)
	return;

      /* If the previous insn wrote to the reg we are to swap with,
	 omit the swap.  */

      if (REG_P (i1dest) && REGNO (i1dest) == (unsigned) hard_regno
	  && REG_P (i1src) && REGNO (i1src) == FIRST_STACK_REG
	  && find_regno_note (i1, REG_DEAD, FIRST_STACK_REG) == NULL_RTX)
	return;

      /* Instead of
	   fld a
	   fld b
	   fxch %st(1)
	 just use
	   fld b
	   fld a
	 if possible.  Similarly for fld1, fldz, fldpi etc. instead of any
	 of the loads or for float extension from memory.  */

      i1src = SET_SRC (i1set);
      if (GET_CODE (i1src) == FLOAT_EXTEND)
	i1src = XEXP (i1src, 0);
      if (REG_P (i1dest)
	  && REGNO (i1dest) == FIRST_STACK_REG
	  && (MEM_P (i1src) || GET_CODE (i1src) == CONST_DOUBLE)
	  && !side_effects_p (i1src)
	  && hard_regno == FIRST_STACK_REG + 1
	  && i1 != BB_HEAD (current_block))
	{
	  /* i1 is the last insn that involves stack regs before insn, and
	     is known to be a load without other side-effects, i.e. fld b
	     in the above comment.  */
	  rtx_insn *i2 = NULL;
	  rtx i2set;
	  rtx_insn *tmp = PREV_INSN (i1);
	  rtx_insn *limit = PREV_INSN (BB_HEAD (current_block));
	  /* Find the previous insn involving stack regs, but don't pass a
	     block boundary.  */
	  while (tmp != limit)
	    {
	      if (LABEL_P (tmp)
		  || CALL_P (tmp)
		  || NOTE_INSN_BASIC_BLOCK_P (tmp)
		  || (NONJUMP_INSN_P (tmp)
		      && stack_regs_mentioned (tmp)))
		{
		  i2 = tmp;
		  break;
		}
	      tmp = PREV_INSN (tmp);
	    }
	  if (i2 != NULL_RTX
	      && (i2set = single_set (i2)) != NULL_RTX)
	    {
	      rtx i2dest = *get_true_reg (&SET_DEST (i2set));
	      rtx i2src = SET_SRC (i2set);
	      if (GET_CODE (i2src) == FLOAT_EXTEND)
		i2src = XEXP (i2src, 0);
	      /* If the last two insns before insn that involve
		 stack regs are loads, where the latter (i1)
		 pushes onto the register stack and thus
		 moves the value from the first load (i2) from
		 %st to %st(1), consider swapping them.  */
	      if (REG_P (i2dest)
		  && REGNO (i2dest) == FIRST_STACK_REG
		  && (MEM_P (i2src) || GET_CODE (i2src) == CONST_DOUBLE)
		  /* Ensure i2 doesn't have other side-effects.  */
		  && !side_effects_p (i2src)
		  /* And that the two instructions can actually be
		     swapped, i.e. there shouldn't be any stores
		     in between i2 and i1 that might alias with
		     the i1 memory, and the memory address can't
		     use registers set in between i2 and i1.  */
		  && !modified_between_p (SET_SRC (i1set), i2, i1))
		{
		  /* Move i1 (fld b above) right before i2 (fld a
		     above.  */
		  remove_insn (i1);
		  SET_PREV_INSN (i1) = NULL_RTX;
		  SET_NEXT_INSN (i1) = NULL_RTX;
		  set_block_for_insn (i1, NULL);
		  emit_insn_before (i1, i2);
		  return;
		}
	    }
	}
    }

  /* Avoid emitting the swap if this is the first register stack insn
     of the current_block.  Instead update the current_block's stack_in
     and let compensate edges take care of this for us.  */
  if (current_block && starting_stack_p)
    {
      BLOCK_INFO (current_block)->stack_in = *regstack;
      starting_stack_p = false;
      return;
    }

  machine_mode raw_mode = reg_raw_mode[FIRST_STACK_REG];
  rtx op1 = FP_MODE_REG (hard_regno, raw_mode);
  rtx op2 = FP_MODE_REG (FIRST_STACK_REG, raw_mode);
  rtx swap_rtx
    = gen_rtx_PARALLEL (VOIDmode,
			gen_rtvec (2, gen_rtx_SET (op1, op2),
				   gen_rtx_SET (op2, op1)));
  if (i1)
    emit_insn_after (swap_rtx, i1);
  else if (current_block)
    emit_insn_before (swap_rtx, BB_HEAD (current_block));
  else
    emit_insn_before (swap_rtx, insn);
}

/* Emit an insns before INSN to swap virtual register SRC1 with
   the top of stack and virtual register SRC2 with second stack
   slot. REGSTACK is the stack state before the swaps, and
   is updated to reflect the swaps.  A swap insn is represented as a
   PARALLEL of two patterns: each pattern moves one reg to the other.

   If SRC1 and/or SRC2 are already at the right place, no swap insn
   is emitted.  */

static void
swap_to_top (rtx_insn *insn, stack_ptr regstack, rtx src1, rtx src2)
{
  struct stack_def temp_stack;
  int regno, j, k;

  temp_stack = *regstack;

  /* Place operand 1 at the top of stack.  */
  regno = get_hard_regnum (&temp_stack, src1);
  gcc_assert (regno >= 0);
  if (regno != FIRST_STACK_REG)
    {
      k = temp_stack.top - (regno - FIRST_STACK_REG);
      j = temp_stack.top;

      std::swap (temp_stack.reg[j], temp_stack.reg[k]);
    }

  /* Place operand 2 next on the stack.  */
  regno = get_hard_regnum (&temp_stack, src2);
  gcc_assert (regno >= 0);
  if (regno != FIRST_STACK_REG + 1)
    {
      k = temp_stack.top - (regno - FIRST_STACK_REG);
      j = temp_stack.top - 1;

      std::swap (temp_stack.reg[j], temp_stack.reg[k]);
    }

  change_stack (insn, regstack, &temp_stack, EMIT_BEFORE);
}

/* Handle a move to or from a stack register in PAT, which is in INSN.
   REGSTACK is the current stack.  Return whether a control flow insn
   was deleted in the process.  */

static bool
move_for_stack_reg (rtx_insn *insn, stack_ptr regstack, rtx pat)
{
  rtx *psrc =  get_true_reg (&SET_SRC (pat));
  rtx *pdest = get_true_reg (&SET_DEST (pat));
  rtx src, dest;
  rtx note;
  bool control_flow_insn_deleted = false;

  src = *psrc; dest = *pdest;

  if (STACK_REG_P (src) && STACK_REG_P (dest))
    {
      /* Write from one stack reg to another.  If SRC dies here, then
	 just change the register mapping and delete the insn.  */

      note = find_regno_note (insn, REG_DEAD, REGNO (src));
      if (note)
	{
	  int i;

	  /* If this is a no-op move, there must not be a REG_DEAD note.  */
	  gcc_assert (REGNO (src) != REGNO (dest));

	  for (i = regstack->top; i >= 0; i--)
	    if (regstack->reg[i] == REGNO (src))
	      break;

	  /* The destination must be dead, or life analysis is borked.  */
	  gcc_assert (get_hard_regnum (regstack, dest) < FIRST_STACK_REG
		      || any_malformed_asm);

	  /* If the source is not live, this is yet another case of
	     uninitialized variables.  Load up a NaN instead.  */
	  if (i < 0)
	    return move_nan_for_stack_reg (insn, regstack, dest);

	  /* It is possible that the dest is unused after this insn.
	     If so, just pop the src.  */

	  if (find_regno_note (insn, REG_UNUSED, REGNO (dest)))
	    emit_pop_insn (insn, regstack, src, EMIT_AFTER);
	  else
	    {
	      regstack->reg[i] = REGNO (dest);
	      SET_HARD_REG_BIT (regstack->reg_set, REGNO (dest));
	      CLEAR_HARD_REG_BIT (regstack->reg_set, REGNO (src));
	    }

	  if (control_flow_insn_p (insn))
	    control_flow_insn_deleted = true;
	  delete_insn (insn);
	  return control_flow_insn_deleted;
	}

      /* The source reg does not die.  */

      /* If this appears to be a no-op move, delete it, or else it
	 will confuse the machine description output patterns. But if
	 it is REG_UNUSED, we must pop the reg now, as per-insn processing
	 for REG_UNUSED will not work for deleted insns.  */

      if (REGNO (src) == REGNO (dest))
	{
	  if (find_regno_note (insn, REG_UNUSED, REGNO (dest)))
	    emit_pop_insn (insn, regstack, dest, EMIT_AFTER);

	  if (control_flow_insn_p (insn))
	    control_flow_insn_deleted = true;
	  delete_insn (insn);
	  return control_flow_insn_deleted;
	}

      /* The destination ought to be dead.  */
      if (get_hard_regnum (regstack, dest) >= FIRST_STACK_REG)
	gcc_assert (any_malformed_asm);
      else
	{
	  replace_reg (psrc, get_hard_regnum (regstack, src));

	  regstack->reg[++regstack->top] = REGNO (dest);
	  SET_HARD_REG_BIT (regstack->reg_set, REGNO (dest));
	  replace_reg (pdest, FIRST_STACK_REG);
	}
    }
  else if (STACK_REG_P (src))
    {
      /* Save from a stack reg to MEM, or possibly integer reg.  Since
	 only top of stack may be saved, emit an exchange first if
	 needs be.  */

      emit_swap_insn (insn, regstack, src);

      note = find_regno_note (insn, REG_DEAD, REGNO (src));
      if (note)
	{
	  replace_reg (&XEXP (note, 0), FIRST_STACK_REG);
	  regstack->top--;
	  CLEAR_HARD_REG_BIT (regstack->reg_set, REGNO (src));
	}
      else if ((GET_MODE (src) == XFmode)
	       && regstack->top < REG_STACK_SIZE - 1)
	{
	  /* A 387 cannot write an XFmode value to a MEM without
	     clobbering the source reg.  The output code can handle
	     this by reading back the value from the MEM.
	     But it is more efficient to use a temp register if one is
	     available.  Push the source value here if the register
	     stack is not full, and then write the value to memory via
	     a pop.  */
	  rtx push_rtx;
	  rtx top_stack_reg = FP_MODE_REG (FIRST_STACK_REG, GET_MODE (src));

	  push_rtx = gen_movxf (top_stack_reg, top_stack_reg);
	  emit_insn_before (push_rtx, insn);
	  add_reg_note (insn, REG_DEAD, top_stack_reg);
	}

      replace_reg (psrc, FIRST_STACK_REG);
    }
  else
    {
      rtx pat = PATTERN (insn);

      gcc_assert (STACK_REG_P (dest));

      /* Load from MEM, or possibly integer REG or constant, into the
	 stack regs.  The actual target is always the top of the
	 stack. The stack mapping is changed to reflect that DEST is
	 now at top of stack.  */

      /* The destination ought to be dead.  However, there is a
	 special case with i387 UNSPEC_TAN, where destination is live
	 (an argument to fptan) but inherent load of 1.0 is modelled
	 as a load from a constant.  */
      if (GET_CODE (pat) == PARALLEL
	  && XVECLEN (pat, 0) == 2
	  && GET_CODE (XVECEXP (pat, 0, 1)) == SET
	  && GET_CODE (SET_SRC (XVECEXP (pat, 0, 1))) == UNSPEC
	  && XINT (SET_SRC (XVECEXP (pat, 0, 1)), 1) == UNSPEC_TAN)
	emit_swap_insn (insn, regstack, dest);
      else
	gcc_assert (get_hard_regnum (regstack, dest) < FIRST_STACK_REG
		    || any_malformed_asm);

      gcc_assert (regstack->top < REG_STACK_SIZE);

      regstack->reg[++regstack->top] = REGNO (dest);
      SET_HARD_REG_BIT (regstack->reg_set, REGNO (dest));
      replace_reg (pdest, FIRST_STACK_REG);
    }

  return control_flow_insn_deleted;
}

/* A helper function which replaces INSN with a pattern that loads up
   a NaN into DEST, then invokes move_for_stack_reg.  */

static bool
move_nan_for_stack_reg (rtx_insn *insn, stack_ptr regstack, rtx dest)
{
  rtx pat;

  dest = FP_MODE_REG (REGNO (dest), SFmode);
  pat = gen_rtx_SET (dest, not_a_num);
  PATTERN (insn) = pat;
  INSN_CODE (insn) = -1;

  return move_for_stack_reg (insn, regstack, pat);
}

/* Swap the condition on a branch, if there is one.  Return true if we
   found a condition to swap.  False if the condition was not used as
   such.  */

static bool
swap_rtx_condition_1 (rtx pat)
{
  const char *fmt;
  bool r = false;
  int i;

  if (COMPARISON_P (pat))
    {
      PUT_CODE (pat, swap_condition (GET_CODE (pat)));
      r = true;
    }
  else
    {
      fmt = GET_RTX_FORMAT (GET_CODE (pat));
      for (i = GET_RTX_LENGTH (GET_CODE (pat)) - 1; i >= 0; i--)
	{
	  if (fmt[i] == 'E')
	    {
	      int j;

	      for (j = XVECLEN (pat, i) - 1; j >= 0; j--)
		if (swap_rtx_condition_1 (XVECEXP (pat, i, j)))
		  r = true;
	    }
	  else if (fmt[i] == 'e' && swap_rtx_condition_1 (XEXP (pat, i)))
	    r = true;
	}
    }

  return r;
}

/* This function swaps condition in cc users and returns true
   if successful.  It is invoked in 2 different modes, one with
   DEBUG_SEEN set initially to 0.  In this mode, next_flags_user
   will skip DEBUG_INSNs that it would otherwise return and just
   sets DEBUG_SEEN to 1 in that case.  If DEBUG_SEEN is 0 at
   the end of toplevel swap_rtx_condition which returns true,
   it means no problematic DEBUG_INSNs were seen and all changes
   have been applied.  If it returns true but DEBUG_SEEN is 1,
   it means some problematic DEBUG_INSNs were seen and no changes
   have been applied so far.  In that case one needs to call
   swap_rtx_condition again with DEBUG_SEEN set to -1, in which
   case it doesn't skip DEBUG_INSNs, but instead adjusts the
   flags related condition in them or resets them as needed.  */

static bool
swap_rtx_condition (rtx_insn *insn, int &debug_seen)
{
  rtx pat = PATTERN (insn);

  /* We're looking for a single set to an HImode temporary.  */

  if (GET_CODE (pat) == SET
      && REG_P (SET_DEST (pat))
      && REGNO (SET_DEST (pat)) == FLAGS_REG)
    {
      insn = next_flags_user (insn, debug_seen);
      if (insn == NULL_RTX)
	return false;
      pat = PATTERN (insn);
    }

  /* See if this is, or ends in, a fnstsw.  If so, we're not doing anything
     with the cc value right now.  We may be able to search for one
     though.  */

  if (GET_CODE (pat) == SET
      && GET_CODE (SET_SRC (pat)) == UNSPEC
      && XINT (SET_SRC (pat), 1) == UNSPEC_FNSTSW)
    {
      rtx dest = SET_DEST (pat);

      /* Search forward looking for the first use of this value.
	 Stop at block boundaries.  */
      while (insn != BB_END (current_block))
	{
	  insn = NEXT_INSN (insn);
	  if (INSN_P (insn) && reg_mentioned_p (dest, insn))
	    {
	      if (DEBUG_INSN_P (insn))
		{
		  if (debug_seen >= 0)
		    debug_seen = 1;
		  else
		    /* Reset the DEBUG insn otherwise.  */
		    INSN_VAR_LOCATION_LOC (insn) = gen_rtx_UNKNOWN_VAR_LOC ();
		  continue;
		}
	      break;
	    }
	  if (CALL_P (insn))
	    return false;
	}

      /* We haven't found it.  */
      if (insn == BB_END (current_block))
	return false;

      /* So we've found the insn using this value.  If it is anything
	 other than sahf or the value does not die (meaning we'd have
	 to search further), then we must give up.  */
      pat = PATTERN (insn);
      if (GET_CODE (pat) != SET
	  || GET_CODE (SET_SRC (pat)) != UNSPEC
	  || XINT (SET_SRC (pat), 1) != UNSPEC_SAHF
	  || ! dead_or_set_p (insn, dest))
	return false;

      /* Now we are prepared to handle this.  */
      insn = next_flags_user (insn, debug_seen);
      if (insn == NULL_RTX)
	return false;
      pat = PATTERN (insn);
    }

  if (swap_rtx_condition_1 (pat))
    {
      bool fail = false;
      if (DEBUG_INSN_P (insn))
	gcc_assert (debug_seen < 0);
      else
	{
	  INSN_CODE (insn) = -1;
	  if (recog_memoized (insn) == -1)
	    fail = true;
	}
      /* In case the flags don't die here, recurse to try fix
	 following user too.  */
      if (!fail && !dead_or_set_p (insn, ix86_flags_rtx))
	{
	  insn = next_flags_user (insn, debug_seen);
	  if (!insn || !swap_rtx_condition (insn, debug_seen))
	    fail = true;
	}
      if (fail || debug_seen == 1)
	swap_rtx_condition_1 (pat);
      return !fail;
    }
  return false;
}

/* Handle a comparison.  Special care needs to be taken to avoid
   causing comparisons that a 387 cannot do correctly, such as EQ.

   Also, a pop insn may need to be emitted.  The 387 does have an
   `fcompp' insn that can pop two regs, but it is sometimes too expensive
   to do this - a `fcomp' followed by a `fstpl %st(0)' may be easier to
   set up.  */

static void
compare_for_stack_reg (rtx_insn *insn, stack_ptr regstack,
		       rtx pat_src, bool can_pop_second_op)
{
  rtx *src1, *src2;
  rtx src1_note, src2_note;
  int debug_seen = 0;

  src1 = get_true_reg (&XEXP (pat_src, 0));
  src2 = get_true_reg (&XEXP (pat_src, 1));

  /* ??? If fxch turns out to be cheaper than fstp, give priority to
     registers that die in this insn - move those to stack top first.  */
  if ((! STACK_REG_P (*src1)
       || (STACK_REG_P (*src2)
	   && get_hard_regnum (regstack, *src2) == FIRST_STACK_REG))
      && swap_rtx_condition (insn, debug_seen))
    {
      /* If swap_rtx_condition succeeded but some debug insns
	 were seen along the way, it has actually reverted all the
	 changes.  Rerun swap_rtx_condition in a mode where DEBUG_ISNSs
	 will be adjusted as well.  */
      if (debug_seen)
	{
	  debug_seen = -1;
	  swap_rtx_condition (insn, debug_seen);
	}
      std::swap (XEXP (pat_src, 0), XEXP (pat_src, 1));

      src1 = get_true_reg (&XEXP (pat_src, 0));
      src2 = get_true_reg (&XEXP (pat_src, 1));

      INSN_CODE (insn) = -1;
    }

  /* We will fix any death note later.  */

  src1_note = find_regno_note (insn, REG_DEAD, REGNO (*src1));

  if (STACK_REG_P (*src2))
    src2_note = find_regno_note (insn, REG_DEAD, REGNO (*src2));
  else
    src2_note = NULL_RTX;

  emit_swap_insn (insn, regstack, *src1);

  replace_reg (src1, FIRST_STACK_REG);

  if (STACK_REG_P (*src2))
    replace_reg (src2, get_hard_regnum (regstack, *src2));

  if (src1_note)
    {
      if (*src2 == CONST0_RTX (GET_MODE (*src2)))
	{
	  /* This is `ftst' insn that can't pop register.  */
	  remove_regno_note (insn, REG_DEAD, REGNO (XEXP (src1_note, 0)));
	  emit_pop_insn (insn, regstack, XEXP (src1_note, 0),
			 EMIT_AFTER);
	}
      else
	{
	  pop_stack (regstack, REGNO (XEXP (src1_note, 0)));
	  replace_reg (&XEXP (src1_note, 0), FIRST_STACK_REG);
	}
    }

  /* If the second operand dies, handle that.  But if the operands are
     the same stack register, don't bother, because only one death is
     needed, and it was just handled.  */

  if (src2_note
      && ! (STACK_REG_P (*src1) && STACK_REG_P (*src2)
	    && REGNO (*src1) == REGNO (*src2)))
    {
      /* As a special case, two regs may die in this insn if src2 is
	 next to top of stack and the top of stack also dies.  Since
	 we have already popped src1, "next to top of stack" is really
	 at top (FIRST_STACK_REG) now.  */

      if (get_hard_regnum (regstack, XEXP (src2_note, 0)) == FIRST_STACK_REG
	  && src1_note && can_pop_second_op)
	{
	  pop_stack (regstack, REGNO (XEXP (src2_note, 0)));
	  replace_reg (&XEXP (src2_note, 0), FIRST_STACK_REG + 1);
	}
      else
	{
	  /* The 386 can only represent death of the first operand in
	     the case handled above.  In all other cases, emit a separate
	     pop and remove the death note from here.  */
	  remove_regno_note (insn, REG_DEAD, REGNO (XEXP (src2_note, 0)));
	  emit_pop_insn (insn, regstack, XEXP (src2_note, 0),
			 EMIT_AFTER);
	}
    }
}

/* Substitute hardware stack regs in debug insn INSN, using stack
   layout REGSTACK.  If we can't find a hardware stack reg for any of
   the REGs in it, reset the debug insn.  */

static void
subst_all_stack_regs_in_debug_insn (rtx_insn *insn, struct stack_def *regstack)
{
  subrtx_ptr_iterator::array_type array;
  FOR_EACH_SUBRTX_PTR (iter, array, &INSN_VAR_LOCATION_LOC (insn), NONCONST)
    {
      rtx *loc = *iter;
      rtx x = *loc;
      if (STACK_REG_P (x))
	{
	  int hard_regno = get_hard_regnum (regstack, x);

	  /* If we can't find an active register, reset this debug insn.  */
	  if (hard_regno == -1)
	    {
	      INSN_VAR_LOCATION_LOC (insn) = gen_rtx_UNKNOWN_VAR_LOC ();
	      return;
	    }

	  gcc_assert (hard_regno >= FIRST_STACK_REG);
	  replace_reg (loc, hard_regno);
	  iter.skip_subrtxes ();
	}
    }
}

/* Substitute new registers in PAT, which is part of INSN.  REGSTACK
   is the current register layout.  Return whether a control flow insn
   was deleted in the process.  */

static bool
subst_stack_regs_pat (rtx_insn *insn, stack_ptr regstack, rtx pat)
{
  rtx *dest, *src;
  bool control_flow_insn_deleted = false;

  switch (GET_CODE (pat))
    {
    case USE:
      /* Deaths in USE insns can happen in non optimizing compilation.
	 Handle them by popping the dying register.  */
      src = get_true_reg (&XEXP (pat, 0));
      if (STACK_REG_P (*src)
	  && find_regno_note (insn, REG_DEAD, REGNO (*src)))
	{
	  /* USEs are ignored for liveness information so USEs of dead
	     register might happen.  */
          if (TEST_HARD_REG_BIT (regstack->reg_set, REGNO (*src)))
	    emit_pop_insn (insn, regstack, *src, EMIT_AFTER);
	  return control_flow_insn_deleted;
	}
      /* Uninitialized USE might happen for functions returning uninitialized
         value.  We will properly initialize the USE on the edge to EXIT_BLOCK,
	 so it is safe to ignore the use here. This is consistent with behavior
	 of dataflow analyzer that ignores USE too.  (This also imply that
	 forcibly initializing the register to NaN here would lead to ICE later,
	 since the REG_DEAD notes are not issued.)  */
      break;

    case VAR_LOCATION:
      gcc_unreachable ();

    case CLOBBER:
      {
	rtx note;

	dest = get_true_reg (&XEXP (pat, 0));
	if (STACK_REG_P (*dest))
	  {
	    note = find_reg_note (insn, REG_DEAD, *dest);

	    if (pat != PATTERN (insn))
	      {
		/* The fix_truncdi_1 pattern wants to be able to
		   allocate its own scratch register.  It does this by
		   clobbering an fp reg so that it is assured of an
		   empty reg-stack register.  If the register is live,
		   kill it now.  Remove the DEAD/UNUSED note so we
		   don't try to kill it later too.

		   In reality the UNUSED note can be absent in some
		   complicated cases when the register is reused for
		   partially set variable.  */

		if (note)
		  emit_pop_insn (insn, regstack, *dest, EMIT_BEFORE);
		else
		  note = find_reg_note (insn, REG_UNUSED, *dest);
		if (note)
		  remove_note (insn, note);
		replace_reg (dest, FIRST_STACK_REG + 1);
	      }
	    else
	      {
		/* A top-level clobber with no REG_DEAD, and no hard-regnum
		   indicates an uninitialized value.  Because reload removed
		   all other clobbers, this must be due to a function
		   returning without a value.  Load up a NaN.  */

		if (!note)
		  {
		    rtx t = *dest;
		    if (COMPLEX_MODE_P (GET_MODE (t)))
		      {
			rtx u = FP_MODE_REG (REGNO (t) + 1, SFmode);
			if (get_hard_regnum (regstack, u) == -1)
			  {
			    rtx pat2 = gen_rtx_CLOBBER (VOIDmode, u);
			    rtx_insn *insn2 = emit_insn_before (pat2, insn);
			    if (move_nan_for_stack_reg (insn2, regstack, u))
			      control_flow_insn_deleted = true;
			  }
		      }
		    if (get_hard_regnum (regstack, t) == -1
			&& move_nan_for_stack_reg (insn, regstack, t))
		      control_flow_insn_deleted = true;
		  }
	      }
	  }
	break;
      }

    case SET:
      {
	rtx *src1 = (rtx *) 0, *src2;
	rtx src1_note, src2_note;
	rtx pat_src;

	dest = get_true_reg (&SET_DEST (pat));
	src  = get_true_reg (&SET_SRC (pat));
	pat_src = SET_SRC (pat);

	/* See if this is a `movM' pattern, and handle elsewhere if so.  */
	if (STACK_REG_P (*src)
	    || (STACK_REG_P (*dest)
		&& (REG_P (*src) || MEM_P (*src)
		    || CONST_DOUBLE_P (*src))))
	  {
	    if (move_for_stack_reg (insn, regstack, pat))
	      control_flow_insn_deleted = true;
	    break;
	  }

	switch (GET_CODE (pat_src))
	  {
	  case CALL:
	    {
	      int count;
	      for (count = REG_NREGS (*dest); --count >= 0;)
		{
		  regstack->reg[++regstack->top] = REGNO (*dest) + count;
		  SET_HARD_REG_BIT (regstack->reg_set, REGNO (*dest) + count);
		}
	    }
	    replace_reg (dest, FIRST_STACK_REG);
	    break;

	  case REG:
	    gcc_unreachable ();

	    /* Fall through.  */

	  case FLOAT_TRUNCATE:
	  case SQRT:
	  case ABS:
	  case NEG:
	    /* These insns only operate on the top of the stack.  It's
	       possible that the tstM case results in a REG_DEAD note on the
	       source.  */

	    if (src1 == 0)
	      src1 = get_true_reg (&XEXP (pat_src, 0));

	    emit_swap_insn (insn, regstack, *src1);

	    src1_note = find_regno_note (insn, REG_DEAD, REGNO (*src1));

	    if (STACK_REG_P (*dest))
	      replace_reg (dest, FIRST_STACK_REG);

	    if (src1_note)
	      {
		replace_reg (&XEXP (src1_note, 0), FIRST_STACK_REG);
		regstack->top--;
		CLEAR_HARD_REG_BIT (regstack->reg_set, REGNO (*src1));
	      }

	    replace_reg (src1, FIRST_STACK_REG);
	    break;

	  case MINUS:
	  case DIV:
	    /* On i386, reversed forms of subM3 and divM3 exist for
	       MODE_FLOAT, so the same code that works for addM3 and mulM3
	       can be used.  */
	  case MULT:
	  case PLUS:
	    /* These insns can accept the top of stack as a destination
	       from a stack reg or mem, or can use the top of stack as a
	       source and some other stack register (possibly top of stack)
	       as a destination.  */

	    src1 = get_true_reg (&XEXP (pat_src, 0));
	    src2 = get_true_reg (&XEXP (pat_src, 1));

	    /* We will fix any death note later.  */

	    if (STACK_REG_P (*src1))
	      src1_note = find_regno_note (insn, REG_DEAD, REGNO (*src1));
	    else
	      src1_note = NULL_RTX;
	    if (STACK_REG_P (*src2))
	      src2_note = find_regno_note (insn, REG_DEAD, REGNO (*src2));
	    else
	      src2_note = NULL_RTX;

	    /* If either operand is not a stack register, then the dest
	       must be top of stack.  */

	    if (! STACK_REG_P (*src1) || ! STACK_REG_P (*src2))
	      emit_swap_insn (insn, regstack, *dest);
	    else
	      {
		/* Both operands are REG.  If neither operand is already
		   at the top of stack, choose to make the one that is the
		   dest the new top of stack.  */

		int src1_hard_regnum, src2_hard_regnum;

		src1_hard_regnum = get_hard_regnum (regstack, *src1);
		src2_hard_regnum = get_hard_regnum (regstack, *src2);

		/* If the source is not live, this is yet another case of
		   uninitialized variables.  Load up a NaN instead.  */
		if (src1_hard_regnum == -1)
		  {
		    rtx pat2 = gen_rtx_CLOBBER (VOIDmode, *src1);
		    rtx_insn *insn2 = emit_insn_before (pat2, insn);
		    if (move_nan_for_stack_reg (insn2, regstack, *src1))
		      control_flow_insn_deleted = true;
		  }
		if (src2_hard_regnum == -1)
		  {
		    rtx pat2 = gen_rtx_CLOBBER (VOIDmode, *src2);
		    rtx_insn *insn2 = emit_insn_before (pat2, insn);
		    if (move_nan_for_stack_reg (insn2, regstack, *src2))
		      control_flow_insn_deleted = true;
		  }

		if (src1_hard_regnum != FIRST_STACK_REG
		    && src2_hard_regnum != FIRST_STACK_REG)
		  emit_swap_insn (insn, regstack, *dest);
	      }

	    if (STACK_REG_P (*src1))
	      replace_reg (src1, get_hard_regnum (regstack, *src1));
	    if (STACK_REG_P (*src2))
	      replace_reg (src2, get_hard_regnum (regstack, *src2));

	    if (src1_note)
	      {
		rtx src1_reg = XEXP (src1_note, 0);

		/* If the register that dies is at the top of stack, then
		   the destination is somewhere else - merely substitute it.
		   But if the reg that dies is not at top of stack, then
		   move the top of stack to the dead reg, as though we had
		   done the insn and then a store-with-pop.  */

		if (REGNO (src1_reg) == regstack->reg[regstack->top])
		  {
		    SET_HARD_REG_BIT (regstack->reg_set, REGNO (*dest));
		    replace_reg (dest, get_hard_regnum (regstack, *dest));
		  }
		else
		  {
		    int regno = get_hard_regnum (regstack, src1_reg);

		    SET_HARD_REG_BIT (regstack->reg_set, REGNO (*dest));
		    replace_reg (dest, regno);

		    regstack->reg[regstack->top - (regno - FIRST_STACK_REG)]
		      = regstack->reg[regstack->top];
		  }

		CLEAR_HARD_REG_BIT (regstack->reg_set,
				    REGNO (XEXP (src1_note, 0)));
		replace_reg (&XEXP (src1_note, 0), FIRST_STACK_REG);
		regstack->top--;
	      }
	    else if (src2_note)
	      {
		rtx src2_reg = XEXP (src2_note, 0);
		if (REGNO (src2_reg) == regstack->reg[regstack->top])
		  {
		    SET_HARD_REG_BIT (regstack->reg_set, REGNO (*dest));
		    replace_reg (dest, get_hard_regnum (regstack, *dest));
		  }
		else
		  {
		    int regno = get_hard_regnum (regstack, src2_reg);

		    SET_HARD_REG_BIT (regstack->reg_set, REGNO (*dest));
		    replace_reg (dest, regno);

		    regstack->reg[regstack->top - (regno - FIRST_STACK_REG)]
		      = regstack->reg[regstack->top];
		  }

		CLEAR_HARD_REG_BIT (regstack->reg_set,
				    REGNO (XEXP (src2_note, 0)));
		replace_reg (&XEXP (src2_note, 0), FIRST_STACK_REG);
		regstack->top--;
	      }
	    else
	      {
		SET_HARD_REG_BIT (regstack->reg_set, REGNO (*dest));
		replace_reg (dest, get_hard_regnum (regstack, *dest));
	      }

	    /* Keep operand 1 matching with destination.  */
	    if (COMMUTATIVE_ARITH_P (pat_src)
		&& REG_P (*src1) && REG_P (*src2)
		&& REGNO (*src1) != REGNO (*dest))
	     {
		int tmp = REGNO (*src1);
		replace_reg (src1, REGNO (*src2));
		replace_reg (src2, tmp);
	     }
	    break;

	  case UNSPEC:
	    switch (XINT (pat_src, 1))
	      {
	      case UNSPEC_FIST:
	      case UNSPEC_FIST_ATOMIC:

	      case UNSPEC_FIST_FLOOR:
	      case UNSPEC_FIST_CEIL:

		/* These insns only operate on the top of the stack.  */

		src1 = get_true_reg (&XVECEXP (pat_src, 0, 0));
		emit_swap_insn (insn, regstack, *src1);

		src1_note = find_regno_note (insn, REG_DEAD, REGNO (*src1));

		if (STACK_REG_P (*dest))
		  replace_reg (dest, FIRST_STACK_REG);

		if (src1_note)
		  {
		    replace_reg (&XEXP (src1_note, 0), FIRST_STACK_REG);
		    regstack->top--;
		    CLEAR_HARD_REG_BIT (regstack->reg_set, REGNO (*src1));
		  }

		replace_reg (src1, FIRST_STACK_REG);
		break;

	      case UNSPEC_FXAM:

		/* This insn only operate on the top of the stack.  */

		src1 = get_true_reg (&XVECEXP (pat_src, 0, 0));
		emit_swap_insn (insn, regstack, *src1);

		src1_note = find_regno_note (insn, REG_DEAD, REGNO (*src1));

		replace_reg (src1, FIRST_STACK_REG);

		if (src1_note)
		  {
		    remove_regno_note (insn, REG_DEAD,
				       REGNO (XEXP (src1_note, 0)));
		    emit_pop_insn (insn, regstack, XEXP (src1_note, 0),
				   EMIT_AFTER);
		  }

		break;

	      case UNSPEC_SIN:
	      case UNSPEC_COS:
	      case UNSPEC_FRNDINT:
	      case UNSPEC_F2XM1:

	      case UNSPEC_FRNDINT_ROUNDEVEN:
	      case UNSPEC_FRNDINT_FLOOR:
	      case UNSPEC_FRNDINT_CEIL:
	      case UNSPEC_FRNDINT_TRUNC:

		/* Above insns operate on the top of the stack.  */

	      case UNSPEC_SINCOS_COS:
	      case UNSPEC_XTRACT_FRACT:

		/* Above insns operate on the top two stack slots,
		   first part of one input, double output insn.  */

		src1 = get_true_reg (&XVECEXP (pat_src, 0, 0));

		emit_swap_insn (insn, regstack, *src1);

		/* Input should never die, it is replaced with output.  */
		src1_note = find_regno_note (insn, REG_DEAD, REGNO (*src1));
		gcc_assert (!src1_note);

		if (STACK_REG_P (*dest))
		  replace_reg (dest, FIRST_STACK_REG);

		replace_reg (src1, FIRST_STACK_REG);
		break;

	      case UNSPEC_SINCOS_SIN:
	      case UNSPEC_XTRACT_EXP:

		/* These insns operate on the top two stack slots,
		   second part of one input, double output insn.  */

		regstack->top++;
		/* FALLTHRU */

	      case UNSPEC_TAN:

		/* For UNSPEC_TAN, regstack->top is already increased
		   by inherent load of constant 1.0.  */

		/* Output value is generated in the second stack slot.
		   Move current value from second slot to the top.  */
		regstack->reg[regstack->top]
		  = regstack->reg[regstack->top - 1];

		gcc_assert (STACK_REG_P (*dest));

		regstack->reg[regstack->top - 1] = REGNO (*dest);
		SET_HARD_REG_BIT (regstack->reg_set, REGNO (*dest));
		replace_reg (dest, FIRST_STACK_REG + 1);

		src1 = get_true_reg (&XVECEXP (pat_src, 0, 0));

		replace_reg (src1, FIRST_STACK_REG);
		break;

	      case UNSPEC_FPATAN:
	      case UNSPEC_FYL2X:
	      case UNSPEC_FYL2XP1:
		/* These insns operate on the top two stack slots.  */

		src1 = get_true_reg (&XVECEXP (pat_src, 0, 0));
		src2 = get_true_reg (&XVECEXP (pat_src, 0, 1));

		src1_note = find_regno_note (insn, REG_DEAD, REGNO (*src1));
		src2_note = find_regno_note (insn, REG_DEAD, REGNO (*src2));

		swap_to_top (insn, regstack, *src1, *src2);

		replace_reg (src1, FIRST_STACK_REG);
		replace_reg (src2, FIRST_STACK_REG + 1);

		if (src1_note)
		  replace_reg (&XEXP (src1_note, 0), FIRST_STACK_REG);
		if (src2_note)
		  replace_reg (&XEXP (src2_note, 0), FIRST_STACK_REG + 1);

		/* Pop both input operands from the stack.  */
		CLEAR_HARD_REG_BIT (regstack->reg_set,
				    regstack->reg[regstack->top]);
		CLEAR_HARD_REG_BIT (regstack->reg_set,
				    regstack->reg[regstack->top - 1]);
		regstack->top -= 2;

		/* Push the result back onto the stack.  */
		regstack->reg[++regstack->top] = REGNO (*dest);
		SET_HARD_REG_BIT (regstack->reg_set, REGNO (*dest));
		replace_reg (dest, FIRST_STACK_REG);
		break;

	      case UNSPEC_FSCALE_FRACT:
	      case UNSPEC_FPREM_F:
	      case UNSPEC_FPREM1_F:
		/* These insns operate on the top two stack slots,
		   first part of double input, double output insn.  */

		src1 = get_true_reg (&XVECEXP (pat_src, 0, 0));
		src2 = get_true_reg (&XVECEXP (pat_src, 0, 1));

		src1_note = find_regno_note (insn, REG_DEAD, REGNO (*src1));
		src2_note = find_regno_note (insn, REG_DEAD, REGNO (*src2));

		/* Inputs should never die, they are
		   replaced with outputs.  */
		gcc_assert (!src1_note);
		gcc_assert (!src2_note);

		swap_to_top (insn, regstack, *src1, *src2);

		/* Push the result back onto stack. Empty stack slot
		   will be filled in second part of insn.  */
		if (STACK_REG_P (*dest))
		  {
		    regstack->reg[regstack->top] = REGNO (*dest);
		    SET_HARD_REG_BIT (regstack->reg_set, REGNO (*dest));
		    replace_reg (dest, FIRST_STACK_REG);
		  }

		replace_reg (src1, FIRST_STACK_REG);
		replace_reg (src2, FIRST_STACK_REG + 1);
		break;

	      case UNSPEC_FSCALE_EXP:
	      case UNSPEC_FPREM_U:
	      case UNSPEC_FPREM1_U:
		/* These insns operate on the top two stack slots,
		   second part of double input, double output insn.  */

		src1 = get_true_reg (&XVECEXP (pat_src, 0, 0));
		src2 = get_true_reg (&XVECEXP (pat_src, 0, 1));

		/* Push the result back onto stack. Fill empty slot from
		   first part of insn and fix top of stack pointer.  */
		if (STACK_REG_P (*dest))
		  {
		    regstack->reg[regstack->top - 1] = REGNO (*dest);
		    SET_HARD_REG_BIT (regstack->reg_set, REGNO (*dest));
		    replace_reg (dest, FIRST_STACK_REG + 1);
		  }

		replace_reg (src1, FIRST_STACK_REG);
		replace_reg (src2, FIRST_STACK_REG + 1);
		break;

	      case UNSPEC_C2_FLAG:
		/* This insn operates on the top two stack slots,
		   third part of C2 setting double input insn.  */

		src1 = get_true_reg (&XVECEXP (pat_src, 0, 0));
		src2 = get_true_reg (&XVECEXP (pat_src, 0, 1));

		replace_reg (src1, FIRST_STACK_REG);
		replace_reg (src2, FIRST_STACK_REG + 1);
		break;

	      case UNSPEC_FNSTSW:
		/* Combined fcomp+fnstsw generated for doing well with
		   CSE.  When optimizing this would have been broken
		   up before now.  */

		pat_src = XVECEXP (pat_src, 0, 0);
		if (GET_CODE (pat_src) == COMPARE)
		  goto do_compare;

		/* Fall through.  */

	      case UNSPEC_NOTRAP:

		pat_src = XVECEXP (pat_src, 0, 0);
		gcc_assert (GET_CODE (pat_src) == COMPARE);
		goto do_compare;

	      default:
		gcc_unreachable ();
	      }
	    break;

	  case COMPARE:
	  do_compare:
	    /* `fcomi' insn can't pop two regs.  */
	    compare_for_stack_reg (insn, regstack, pat_src,
				   REGNO (*dest) != FLAGS_REG);
	    break;

	  case IF_THEN_ELSE:
	    /* This insn requires the top of stack to be the destination.  */

	    src1 = get_true_reg (&XEXP (pat_src, 1));
	    src2 = get_true_reg (&XEXP (pat_src, 2));

	    src1_note = find_regno_note (insn, REG_DEAD, REGNO (*src1));
	    src2_note = find_regno_note (insn, REG_DEAD, REGNO (*src2));

	    /* If the comparison operator is an FP comparison operator,
	       it is handled correctly by compare_for_stack_reg () who
	       will move the destination to the top of stack. But if the
	       comparison operator is not an FP comparison operator, we
	       have to handle it here.  */
	    if (get_hard_regnum (regstack, *dest) >= FIRST_STACK_REG
		&& REGNO (*dest) != regstack->reg[regstack->top])
	      {
		/* In case one of operands is the top of stack and the operands
		   dies, it is safe to make it the destination operand by
		   reversing the direction of cmove and avoid fxch.  */
		if ((REGNO (*src1) == regstack->reg[regstack->top]
		     && src1_note)
		    || (REGNO (*src2) == regstack->reg[regstack->top]
			&& src2_note))
		  {
		    int idx1 = (get_hard_regnum (regstack, *src1)
				- FIRST_STACK_REG);
		    int idx2 = (get_hard_regnum (regstack, *src2)
				- FIRST_STACK_REG);

		    /* Make reg-stack believe that the operands are already
		       swapped on the stack */
		    regstack->reg[regstack->top - idx1] = REGNO (*src2);
		    regstack->reg[regstack->top - idx2] = REGNO (*src1);

		    /* Reverse condition to compensate the operand swap.
		       i386 do have comparison always reversible.  */
		    PUT_CODE (XEXP (pat_src, 0),
			      reversed_comparison_code (XEXP (pat_src, 0), insn));
		  }
		else
	          emit_swap_insn (insn, regstack, *dest);
	      }

	    {
	      rtx src_note [3];
	      int i;

	      src_note[0] = 0;
	      src_note[1] = src1_note;
	      src_note[2] = src2_note;

	      if (STACK_REG_P (*src1))
		replace_reg (src1, get_hard_regnum (regstack, *src1));
	      if (STACK_REG_P (*src2))
		replace_reg (src2, get_hard_regnum (regstack, *src2));

	      for (i = 1; i <= 2; i++)
		if (src_note [i])
		  {
		    int regno = REGNO (XEXP (src_note[i], 0));

		    /* If the register that dies is not at the top of
		       stack, then move the top of stack to the dead reg.
		       Top of stack should never die, as it is the
		       destination.  */
		    gcc_assert (regno != regstack->reg[regstack->top]);
		    remove_regno_note (insn, REG_DEAD, regno);
		    emit_pop_insn (insn, regstack, XEXP (src_note[i], 0),
				    EMIT_AFTER);
		  }
	    }

	    /* Make dest the top of stack.  Add dest to regstack if
	       not present.  */
	    if (get_hard_regnum (regstack, *dest) < FIRST_STACK_REG)
	      regstack->reg[++regstack->top] = REGNO (*dest);
	    SET_HARD_REG_BIT (regstack->reg_set, REGNO (*dest));
	    replace_reg (dest, FIRST_STACK_REG);
	    break;

	  default:
	    gcc_unreachable ();
	  }
	break;
      }

    default:
      break;
    }

  return control_flow_insn_deleted;
}

/* Substitute hard regnums for any stack regs in INSN, which has
   N_INPUTS inputs and N_OUTPUTS outputs.  REGSTACK is the stack info
   before the insn, and is updated with changes made here.

   There are several requirements and assumptions about the use of
   stack-like regs in asm statements.  These rules are enforced by
   record_asm_stack_regs; see comments there for details.  Any
   asm_operands left in the RTL at this point may be assume to meet the
   requirements, since record_asm_stack_regs removes any problem asm.  */

static void
subst_asm_stack_regs (rtx_insn *insn, stack_ptr regstack)
{
  rtx body = PATTERN (insn);

  rtx *note_reg;		/* Array of note contents */
  rtx **note_loc;		/* Address of REG field of each note */
  enum reg_note *note_kind;	/* The type of each note */

  rtx *clobber_reg = 0;
  rtx **clobber_loc = 0;

  struct stack_def temp_stack;
  int n_notes;
  int n_clobbers;
  rtx note;
  int i;
  int n_inputs, n_outputs;

  if (! check_asm_stack_operands (insn))
    return;

  /* Find out what the constraints required.  If no constraint
     alternative matches, that is a compiler bug: we should have caught
     such an insn in check_asm_stack_operands.  */
  extract_constrain_insn (insn);

  preprocess_constraints (insn);
  const operand_alternative *op_alt = which_op_alt ();

  get_asm_operands_in_out (body, &n_outputs, &n_inputs);

  /* Strip SUBREGs here to make the following code simpler.  */
  for (i = 0; i < recog_data.n_operands; i++)
    if (GET_CODE (recog_data.operand[i]) == SUBREG
	&& REG_P (SUBREG_REG (recog_data.operand[i])))
      {
	recog_data.operand_loc[i] = & SUBREG_REG (recog_data.operand[i]);
	recog_data.operand[i] = SUBREG_REG (recog_data.operand[i]);
      }

  /* Set up NOTE_REG, NOTE_LOC and NOTE_KIND.  */

  for (i = 0, note = REG_NOTES (insn); note; note = XEXP (note, 1))
    i++;

  note_reg = XALLOCAVEC (rtx, i);
  note_loc = XALLOCAVEC (rtx *, i);
  note_kind = XALLOCAVEC (enum reg_note, i);

  n_notes = 0;
  for (note = REG_NOTES (insn); note; note = XEXP (note, 1))
    {
      if (GET_CODE (note) != EXPR_LIST)
	continue;
      rtx reg = XEXP (note, 0);
      rtx *loc = & XEXP (note, 0);

      if (GET_CODE (reg) == SUBREG && REG_P (SUBREG_REG (reg)))
	{
	  loc = & SUBREG_REG (reg);
	  reg = SUBREG_REG (reg);
	}

      if (STACK_REG_P (reg)
	  && (REG_NOTE_KIND (note) == REG_DEAD
	      || REG_NOTE_KIND (note) == REG_UNUSED))
	{
	  note_reg[n_notes] = reg;
	  note_loc[n_notes] = loc;
	  note_kind[n_notes] = REG_NOTE_KIND (note);
	  n_notes++;
	}
    }

  /* Set up CLOBBER_REG and CLOBBER_LOC.  */

  n_clobbers = 0;

  if (GET_CODE (body) == PARALLEL)
    {
      clobber_reg = XALLOCAVEC (rtx, XVECLEN (body, 0));
      clobber_loc = XALLOCAVEC (rtx *, XVECLEN (body, 0));

      for (i = 0; i < XVECLEN (body, 0); i++)
	if (GET_CODE (XVECEXP (body, 0, i)) == CLOBBER)
	  {
	    rtx clobber = XVECEXP (body, 0, i);
	    rtx reg = XEXP (clobber, 0);
	    rtx *loc = & XEXP (clobber, 0);

	    if (GET_CODE (reg) == SUBREG && REG_P (SUBREG_REG (reg)))
	      {
		loc = & SUBREG_REG (reg);
		reg = SUBREG_REG (reg);
	      }

	    if (STACK_REG_P (reg))
	      {
		clobber_reg[n_clobbers] = reg;
		clobber_loc[n_clobbers] = loc;
		n_clobbers++;
	      }
	  }
    }

  temp_stack = *regstack;

  /* Put the input regs into the desired place in TEMP_STACK.  */

  for (i = n_outputs; i < n_outputs + n_inputs; i++)
    if (STACK_REG_P (recog_data.operand[i])
	&& reg_class_subset_p (op_alt[i].cl, FLOAT_REGS)
	&& op_alt[i].cl != FLOAT_REGS)
      {
	/* If an operand needs to be in a particular reg in
	   FLOAT_REGS, the constraint was either 't' or 'u'.  Since
	   these constraints are for single register classes, and
	   reload guaranteed that operand[i] is already in that class,
	   we can just use REGNO (recog_data.operand[i]) to know which
	   actual reg this operand needs to be in.  */

	int regno = get_hard_regnum (&temp_stack, recog_data.operand[i]);

	gcc_assert (regno >= 0);

	if ((unsigned int) regno != REGNO (recog_data.operand[i]))
	  {
	    /* recog_data.operand[i] is not in the right place.  Find
	       it and swap it with whatever is already in I's place.
	       K is where recog_data.operand[i] is now.  J is where it
	       should be.  */
	    int j, k;

	    k = temp_stack.top - (regno - FIRST_STACK_REG);
	    j = (temp_stack.top
		 - (REGNO (recog_data.operand[i]) - FIRST_STACK_REG));

	    std::swap (temp_stack.reg[j], temp_stack.reg[k]);
	  }
      }

  /* Emit insns before INSN to make sure the reg-stack is in the right
     order.  */

  change_stack (insn, regstack, &temp_stack, EMIT_BEFORE);

  /* Make the needed input register substitutions.  Do death notes and
     clobbers too, because these are for inputs, not outputs.  */

  for (i = n_outputs; i < n_outputs + n_inputs; i++)
    if (STACK_REG_P (recog_data.operand[i]))
      {
	int regnum = get_hard_regnum (regstack, recog_data.operand[i]);

	gcc_assert (regnum >= 0);

	replace_reg (recog_data.operand_loc[i], regnum);
      }

  for (i = 0; i < n_notes; i++)
    if (note_kind[i] == REG_DEAD)
      {
	int regnum = get_hard_regnum (regstack, note_reg[i]);

	gcc_assert (regnum >= 0);

	replace_reg (note_loc[i], regnum);
      }

  for (i = 0; i < n_clobbers; i++)
    {
      /* It's OK for a CLOBBER to reference a reg that is not live.
         Don't try to replace it in that case.  */
      int regnum = get_hard_regnum (regstack, clobber_reg[i]);

      if (regnum >= 0)
	replace_reg (clobber_loc[i], regnum);
    }

  /* Now remove from REGSTACK any inputs that the asm implicitly popped.  */

  for (i = n_outputs; i < n_outputs + n_inputs; i++)
    if (STACK_REG_P (recog_data.operand[i]))
      {
	/* An input reg is implicitly popped if it is tied to an
	   output, or if there is a CLOBBER for it.  */
	int j;

	for (j = 0; j < n_clobbers; j++)
	  if (operands_match_p (clobber_reg[j], recog_data.operand[i]))
	    break;

	if (j < n_clobbers || op_alt[i].matches >= 0)
	  {
	    /* recog_data.operand[i] might not be at the top of stack.
	       But that's OK, because all we need to do is pop the
	       right number of regs off of the top of the reg-stack.
	       record_asm_stack_regs guaranteed that all implicitly
	       popped regs were grouped at the top of the reg-stack.  */

	    CLEAR_HARD_REG_BIT (regstack->reg_set,
				regstack->reg[regstack->top]);
	    regstack->top--;
	  }
      }

  /* Now add to REGSTACK any outputs that the asm implicitly pushed.
     Note that there isn't any need to substitute register numbers.
     ???  Explain why this is true.  */

  for (i = LAST_STACK_REG; i >= FIRST_STACK_REG; i--)
    {
      /* See if there is an output for this hard reg.  */
      int j;

      for (j = 0; j < n_outputs; j++)
	if (STACK_REG_P (recog_data.operand[j])
	    && REGNO (recog_data.operand[j]) == (unsigned) i)
	  {
	    regstack->reg[++regstack->top] = i;
	    SET_HARD_REG_BIT (regstack->reg_set, i);
	    break;
	  }
    }

  /* Now emit a pop insn for any REG_UNUSED output, or any REG_DEAD
     input that the asm didn't implicitly pop.  If the asm didn't
     implicitly pop an input reg, that reg will still be live.

     Note that we can't use find_regno_note here: the register numbers
     in the death notes have already been substituted.  */

  for (i = 0; i < n_outputs; i++)
    if (STACK_REG_P (recog_data.operand[i]))
      {
	int j;

	for (j = 0; j < n_notes; j++)
	  if (REGNO (recog_data.operand[i]) == REGNO (note_reg[j])
	      && note_kind[j] == REG_UNUSED)
	    {
	      insn = emit_pop_insn (insn, regstack, recog_data.operand[i],
				    EMIT_AFTER);
	      break;
	    }
      }

  for (i = n_outputs; i < n_outputs + n_inputs; i++)
    if (STACK_REG_P (recog_data.operand[i]))
      {
	int j;

	for (j = 0; j < n_notes; j++)
	  if (REGNO (recog_data.operand[i]) == REGNO (note_reg[j])
	      && note_kind[j] == REG_DEAD
	      && TEST_HARD_REG_BIT (regstack->reg_set,
				    REGNO (recog_data.operand[i])))
	    {
	      insn = emit_pop_insn (insn, regstack, recog_data.operand[i],
				    EMIT_AFTER);
	      break;
	    }
      }
}

/* Return true if a function call is allowed to alter some or all bits
   of any stack reg.  */
static bool
callee_clobbers_any_stack_reg (const function_abi & callee_abi)
{
  for (unsigned regno = FIRST_STACK_REG; regno <= LAST_STACK_REG; regno++)
    if (callee_abi.clobbers_at_least_part_of_reg_p (regno))
      return true;
  return false;
}


/* Substitute stack hard reg numbers for stack virtual registers in
   INSN.  Non-stack register numbers are not changed.  REGSTACK is the
   current stack content.  Insns may be emitted as needed to arrange the
   stack for the 387 based on the contents of the insn.  Return whether
   a control flow insn was deleted in the process.  */

static bool
subst_stack_regs (rtx_insn *insn, stack_ptr regstack)
{
  rtx *note_link, note;
  bool control_flow_insn_deleted = false;
  int i;

  /* If the target of the call doesn't clobber any stack registers,
     Don't clear the arguments.  */
  if (CALL_P (insn)
      && callee_clobbers_any_stack_reg (insn_callee_abi (insn)))
    {
      int top = regstack->top;

      /* If there are any floating point parameters to be passed in
	 registers for this call, make sure they are in the right
	 order.  */

      if (top >= 0)
	{
	  straighten_stack (insn, regstack);

	  /* Now mark the arguments as dead after the call.  */

	  while (regstack->top >= 0)
	    {
	      CLEAR_HARD_REG_BIT (regstack->reg_set, FIRST_STACK_REG + regstack->top);
	      regstack->top--;
	    }
	}
    }

  /* Do the actual substitution if any stack regs are mentioned.
     Since we only record whether entire insn mentions stack regs, and
     subst_stack_regs_pat only works for patterns that contain stack regs,
     we must check each pattern in a parallel here.  A call_value_pop could
     fail otherwise.  */

  if (stack_regs_mentioned (insn))
    {
      int n_operands = asm_noperands (PATTERN (insn));
      if (n_operands >= 0)
	{
	  /* This insn is an `asm' with operands.  Decode the operands,
	     decide how many are inputs, and do register substitution.
	     Any REG_UNUSED notes will be handled by subst_asm_stack_regs.  */

	  subst_asm_stack_regs (insn, regstack);
	  return control_flow_insn_deleted;
	}

      if (GET_CODE (PATTERN (insn)) == PARALLEL)
	for (i = 0; i < XVECLEN (PATTERN (insn), 0); i++)
	  {
	    if (stack_regs_mentioned_p (XVECEXP (PATTERN (insn), 0, i)))
	      {
	        if (GET_CODE (XVECEXP (PATTERN (insn), 0, i)) == CLOBBER)
	           XVECEXP (PATTERN (insn), 0, i)
		     = shallow_copy_rtx (XVECEXP (PATTERN (insn), 0, i));
		if (subst_stack_regs_pat (insn, regstack,
					  XVECEXP (PATTERN (insn), 0, i)))
		  control_flow_insn_deleted = true;
	      }
	  }
      else if (subst_stack_regs_pat (insn, regstack, PATTERN (insn)))
	control_flow_insn_deleted = true;
    }

  /* subst_stack_regs_pat may have deleted a no-op insn.  If so, any
     REG_UNUSED will already have been dealt with, so just return.  */

  if (NOTE_P (insn) || insn->deleted ())
    return control_flow_insn_deleted;

  /* If this a noreturn call, we can't insert pop insns after it.
     Instead, reset the stack state to empty.  */
  if (CALL_P (insn)
      && find_reg_note (insn, REG_NORETURN, NULL))
    {
      regstack->top = -1;
      CLEAR_HARD_REG_SET (regstack->reg_set);
      return control_flow_insn_deleted;
    }

  /* If there is a REG_UNUSED note on a stack register on this insn,
     the indicated reg must be popped.  The REG_UNUSED note is removed,
     since the form of the newly emitted pop insn references the reg,
     making it no longer `unset'.  */

  note_link = &REG_NOTES (insn);
  for (note = *note_link; note; note = XEXP (note, 1))
    if (REG_NOTE_KIND (note) == REG_UNUSED && STACK_REG_P (XEXP (note, 0)))
      {
	*note_link = XEXP (note, 1);
	insn = emit_pop_insn (insn, regstack, XEXP (note, 0), EMIT_AFTER);
      }
    else
      note_link = &XEXP (note, 1);

  return control_flow_insn_deleted;
}

/* Change the organization of the stack so that it fits a new basic
   block.  Some registers might have to be popped, but there can never be
   a register live in the new block that is not now live.

   Insert any needed insns before or after INSN, as indicated by
   WHERE.  OLD is the original stack layout, and NEW is the desired
   form.  OLD is updated to reflect the code emitted, i.e., it will be
   the same as NEW upon return.

   This function will not preserve block_end[].  But that information
   is no longer needed once this has executed.  */

static void
change_stack (rtx_insn *insn, stack_ptr old, stack_ptr new_stack,
	      enum emit_where where)
{
  int reg;
  machine_mode raw_mode = reg_raw_mode[FIRST_STACK_REG];
  rtx_insn *update_end = NULL;
  int i;

  /* Stack adjustments for the first insn in a block update the
     current_block's stack_in instead of inserting insns directly.
     compensate_edges will add the necessary code later.  */
  if (current_block
      && starting_stack_p
      && where == EMIT_BEFORE)
    {
      BLOCK_INFO (current_block)->stack_in = *new_stack;
      starting_stack_p = false;
      *old = *new_stack;
      return;
    }

  /* We will be inserting new insns "backwards".  If we are to insert
     after INSN, find the next insn, and insert before it.  */

  if (where == EMIT_AFTER)
    {
      if (current_block && BB_END (current_block) == insn)
	update_end = insn;
      insn = NEXT_INSN (insn);
    }

  /* Initialize partially dead variables.  */
  for (i = FIRST_STACK_REG; i < LAST_STACK_REG + 1; i++)
    if (TEST_HARD_REG_BIT (new_stack->reg_set, i)
	&& !TEST_HARD_REG_BIT (old->reg_set, i))
      {
	old->reg[++old->top] = i;
        SET_HARD_REG_BIT (old->reg_set, i);
	emit_insn_before (gen_rtx_SET (FP_MODE_REG (i, SFmode), not_a_num),
			  insn);
      }

  /* Pop any registers that are not needed in the new block.  */

  /* If the destination block's stack already has a specified layout
     and contains two or more registers, use a more intelligent algorithm
     to pop registers that minimizes the number of fxchs below.  */
  if (new_stack->top > 0)
    {
      bool slots[REG_STACK_SIZE];
      int pops[REG_STACK_SIZE];
      int next, dest, topsrc;

      /* First pass to determine the free slots.  */
      for (reg = 0; reg <= new_stack->top; reg++)
	slots[reg] = TEST_HARD_REG_BIT (new_stack->reg_set, old->reg[reg]);

      /* Second pass to allocate preferred slots.  */
      topsrc = -1;
      for (reg = old->top; reg > new_stack->top; reg--)
	if (TEST_HARD_REG_BIT (new_stack->reg_set, old->reg[reg]))
	  {
	    dest = -1;
	    for (next = 0; next <= new_stack->top; next++)
	      if (!slots[next] && new_stack->reg[next] == old->reg[reg])
		{
		  /* If this is a preference for the new top of stack, record
		     the fact by remembering it's old->reg in topsrc.  */
                  if (next == new_stack->top)
		    topsrc = reg;
		  slots[next] = true;
		  dest = next;
		  break;
		}
	    pops[reg] = dest;
	  }
	else
	  pops[reg] = reg;

      /* Intentionally, avoid placing the top of stack in it's correct
	 location, if we still need to permute the stack below and we
	 can usefully place it somewhere else.  This is the case if any
	 slot is still unallocated, in which case we should place the
	 top of stack there.  */
      if (topsrc != -1)
	for (reg = 0; reg < new_stack->top; reg++)
	  if (!slots[reg])
	    {
	      pops[topsrc] = reg;
	      slots[new_stack->top] = false;
	      slots[reg] = true;
	      break;
	    }

      /* Third pass allocates remaining slots and emits pop insns.  */
      next = new_stack->top;
      for (reg = old->top; reg > new_stack->top; reg--)
	{
	  dest = pops[reg];
	  if (dest == -1)
	    {
	      /* Find next free slot.  */
	      while (slots[next])
		next--;
	      dest = next--;
	    }
	  emit_pop_insn (insn, old, FP_MODE_REG (old->reg[dest], raw_mode),
			 EMIT_BEFORE);
	}
    }
  else
    {
      /* The following loop attempts to maximize the number of times we
	 pop the top of the stack, as this permits the use of the faster
	 ffreep instruction on platforms that support it.  */
      int live, next;

      live = 0;
      for (reg = 0; reg <= old->top; reg++)
        if (TEST_HARD_REG_BIT (new_stack->reg_set, old->reg[reg]))
          live++;

      next = live;
      while (old->top >= live)
        if (TEST_HARD_REG_BIT (new_stack->reg_set, old->reg[old->top]))
	  {
	    while (TEST_HARD_REG_BIT (new_stack->reg_set, old->reg[next]))
	      next--;
	    emit_pop_insn (insn, old, FP_MODE_REG (old->reg[next], raw_mode),
			   EMIT_BEFORE);
	  }
	else
	  emit_pop_insn (insn, old, FP_MODE_REG (old->reg[old->top], raw_mode),
			 EMIT_BEFORE);
    }

  if (new_stack->top == -2)
    {
      /* If the new block has never been processed, then it can inherit
	 the old stack order.  */

      new_stack->top = old->top;
      memcpy (new_stack->reg, old->reg, sizeof (new_stack->reg));
    }
  else
    {
      /* This block has been entered before, and we must match the
	 previously selected stack order.  */

      /* By now, the only difference should be the order of the stack,
	 not their depth or liveliness.  */

      gcc_assert (old->reg_set == new_stack->reg_set);
      gcc_assert (old->top == new_stack->top);

      /* If the stack is not empty (new_stack->top != -1), loop here emitting
	 swaps until the stack is correct.

	 The worst case number of swaps emitted is N + 2, where N is the
	 depth of the stack.  In some cases, the reg at the top of
	 stack may be correct, but swapped anyway in order to fix
	 other regs.  But since we never swap any other reg away from
	 its correct slot, this algorithm will converge.  */

      if (new_stack->top != -1)
	do
	  {
	    /* Swap the reg at top of stack into the position it is
	       supposed to be in, until the correct top of stack appears.  */

	    while (old->reg[old->top] != new_stack->reg[new_stack->top])
	      {
		for (reg = new_stack->top; reg >= 0; reg--)
		  if (new_stack->reg[reg] == old->reg[old->top])
		    break;

		gcc_assert (reg != -1);

		emit_swap_insn (insn, old,
				FP_MODE_REG (old->reg[reg], raw_mode));
	      }

	    /* See if any regs remain incorrect.  If so, bring an
	     incorrect reg to the top of stack, and let the while loop
	     above fix it.  */

	    for (reg = new_stack->top; reg >= 0; reg--)
	      if (new_stack->reg[reg] != old->reg[reg])
		{
		  emit_swap_insn (insn, old,
				  FP_MODE_REG (old->reg[reg], raw_mode));
		  break;
		}
	  } while (reg >= 0);

      /* At this point there must be no differences.  */

      for (reg = old->top; reg >= 0; reg--)
	gcc_assert (old->reg[reg] == new_stack->reg[reg]);
    }

  if (update_end)
    {
      for (update_end = NEXT_INSN (update_end); update_end != insn;
	   update_end = NEXT_INSN (update_end))
	{
	  set_block_for_insn (update_end, current_block);
	  if (INSN_P (update_end))
	    df_insn_rescan (update_end);
	}
      BB_END (current_block) = PREV_INSN (insn);
    }
}

/* Print stack configuration.  */

static void
print_stack (FILE *file, stack_ptr s)
{
  if (! file)
    return;

  if (s->top == -2)
    fprintf (file, "uninitialized\n");
  else if (s->top == -1)
    fprintf (file, "empty\n");
  else
    {
      int i;
      fputs ("[ ", file);
      for (i = 0; i <= s->top; ++i)
	fprintf (file, "%d ", s->reg[i]);
      fputs ("]\n", file);
    }
}

/* This function was doing life analysis.  We now let the regular live
   code do it's job, so we only need to check some extra invariants
   that reg-stack expects.  Primary among these being that all registers
   are initialized before use.

   The function returns true when code was emitted to CFG edges and
   commit_edge_insertions needs to be called.  */

static bool
convert_regs_entry (void)
{
  bool inserted = false;
  edge e;
  edge_iterator ei;

  /* Load something into each stack register live at function entry.
     Such live registers can be caused by uninitialized variables or
     functions not returning values on all paths.  In order to keep
     the push/pop code happy, and to not scrog the register stack, we
     must put something in these registers.  Use a QNaN.

     Note that we are inserting converted code here.  This code is
     never seen by the convert_regs pass.  */

  FOR_EACH_EDGE (e, ei, ENTRY_BLOCK_PTR_FOR_FN (cfun)->succs)
    {
      basic_block block = e->dest;
      block_info bi = BLOCK_INFO (block);
      int reg, top = -1;

      for (reg = LAST_STACK_REG; reg >= FIRST_STACK_REG; --reg)
	if (TEST_HARD_REG_BIT (bi->stack_in.reg_set, reg))
	  {
	    rtx init;

	    bi->stack_in.reg[++top] = reg;

	    init = gen_rtx_SET (FP_MODE_REG (FIRST_STACK_REG, SFmode),
				not_a_num);
	    insert_insn_on_edge (init, e);
	    inserted = true;
	  }

      bi->stack_in.top = top;
    }

  return inserted;
}

/* Construct the desired stack for function exit.  This will either
   be `empty', or the function return value at top-of-stack.  */

static void
convert_regs_exit (void)
{
  int value_reg_low, value_reg_high;
  stack_ptr output_stack;
  rtx retvalue;

  retvalue = stack_result (current_function_decl);
  value_reg_low = value_reg_high = -1;
  if (retvalue)
    {
      value_reg_low = REGNO (retvalue);
      value_reg_high = END_REGNO (retvalue) - 1;
    }

  output_stack = &BLOCK_INFO (EXIT_BLOCK_PTR_FOR_FN (cfun))->stack_in;
  if (value_reg_low == -1)
    output_stack->top = -1;
  else
    {
      int reg;

      output_stack->top = value_reg_high - value_reg_low;
      for (reg = value_reg_low; reg <= value_reg_high; ++reg)
	{
	  output_stack->reg[value_reg_high - reg] = reg;
	  SET_HARD_REG_BIT (output_stack->reg_set, reg);
	}
    }
}

/* Copy the stack info from the end of edge E's source block to the
   start of E's destination block.  */

static void
propagate_stack (edge e)
{
  stack_ptr src_stack = &BLOCK_INFO (e->src)->stack_out;
  stack_ptr dest_stack = &BLOCK_INFO (e->dest)->stack_in;
  int reg;

  /* Preserve the order of the original stack, but check whether
     any pops are needed.  */
  dest_stack->top = -1;
  for (reg = 0; reg <= src_stack->top; ++reg)
    if (TEST_HARD_REG_BIT (dest_stack->reg_set, src_stack->reg[reg]))
      dest_stack->reg[++dest_stack->top] = src_stack->reg[reg];

  /* Push in any partially dead values.  */
  for (reg = FIRST_STACK_REG; reg < LAST_STACK_REG + 1; reg++)
    if (TEST_HARD_REG_BIT (dest_stack->reg_set, reg)
        && !TEST_HARD_REG_BIT (src_stack->reg_set, reg))
      dest_stack->reg[++dest_stack->top] = reg;
}


/* Adjust the stack of edge E's source block on exit to match the stack
   of it's target block upon input.  The stack layouts of both blocks
   should have been defined by now.  */

static bool
compensate_edge (edge e)
{
  basic_block source = e->src, target = e->dest;
  stack_ptr target_stack = &BLOCK_INFO (target)->stack_in;
  stack_ptr source_stack = &BLOCK_INFO (source)->stack_out;
  struct stack_def regstack;
  int reg;

  if (dump_file)
    fprintf (dump_file, "Edge %d->%d: ", source->index, target->index);

  gcc_assert (target_stack->top != -2);

  /* Check whether stacks are identical.  */
  if (target_stack->top == source_stack->top)
    {
      for (reg = target_stack->top; reg >= 0; --reg)
	if (target_stack->reg[reg] != source_stack->reg[reg])
	  break;

      if (reg == -1)
	{
	  if (dump_file)
	    fprintf (dump_file, "no changes needed\n");
	  return false;
	}
    }

  if (dump_file)
    {
      fprintf (dump_file, "correcting stack to ");
      print_stack (dump_file, target_stack);
    }

  /* Abnormal calls may appear to have values live in st(0), but the
     abnormal return path will not have actually loaded the values.  */
  if (e->flags & EDGE_ABNORMAL_CALL)
    {
      /* Assert that the lifetimes are as we expect -- one value
         live at st(0) on the end of the source block, and no
         values live at the beginning of the destination block.
	 For complex return values, we may have st(1) live as well.  */
      gcc_assert (source_stack->top == 0 || source_stack->top == 1);
      gcc_assert (target_stack->top == -1);
      return false;
    }

  /* Handle non-call EH edges specially.  The normal return path have
     values in registers.  These will be popped en masse by the unwind
     library.  */
  if (e->flags & EDGE_EH)
    {
      gcc_assert (target_stack->top == -1);
      return false;
    }

  /* We don't support abnormal edges.  Global takes care to
     avoid any live register across them, so we should never
     have to insert instructions on such edges.  */
  gcc_assert (! (e->flags & EDGE_ABNORMAL));

  /* Make a copy of source_stack as change_stack is destructive.  */
  regstack = *source_stack;

  /* It is better to output directly to the end of the block
     instead of to the edge, because emit_swap can do minimal
     insn scheduling.  We can do this when there is only one
     edge out, and it is not abnormal.  */
  if (EDGE_COUNT (source->succs) == 1)
    {
      current_block = source;
      change_stack (BB_END (source), &regstack, target_stack,
		    (JUMP_P (BB_END (source)) ? EMIT_BEFORE : EMIT_AFTER));
    }
  else
    {
      rtx_insn *seq;
      rtx_note *after;

      current_block = NULL;
      start_sequence ();

      /* ??? change_stack needs some point to emit insns after.  */
      after = emit_note (NOTE_INSN_DELETED);

      change_stack (after, &regstack, target_stack, EMIT_BEFORE);

      seq = end_sequence ();

      set_insn_locations (seq, e->goto_locus);
      insert_insn_on_edge (seq, e);
      return true;
    }
  return false;
}

/* Traverse all non-entry edges in the CFG, and emit the necessary
   edge compensation code to change the stack from stack_out of the
   source block to the stack_in of the destination block.  */

static bool
compensate_edges (void)
{
  bool inserted = false;
  basic_block bb;

  starting_stack_p = false;

  FOR_EACH_BB_FN (bb, cfun)
    if (bb != ENTRY_BLOCK_PTR_FOR_FN (cfun))
      {
        edge e;
        edge_iterator ei;

        FOR_EACH_EDGE (e, ei, bb->succs)
	  if (compensate_edge (e))
	    inserted = true;
      }
  return inserted;
}

/* Select the better of two edges E1 and E2 to use to determine the
   stack layout for their shared destination basic block.  This is
   typically the more frequently executed.  The edge E1 may be NULL
   (in which case E2 is returned), but E2 is always non-NULL.  */

static edge
better_edge (edge e1, edge e2)
{
  if (!e1)
    return e2;

  if (e1->count () > e2->count ())
    return e1;
  if (e1->count () < e2->count ())
    return e2;

  /* Prefer critical edges to minimize inserting compensation code on
     critical edges.  */

  if (EDGE_CRITICAL_P (e1) != EDGE_CRITICAL_P (e2))
    return EDGE_CRITICAL_P (e1) ? e1 : e2;

  /* Avoid non-deterministic behavior.  */
  return (e1->src->index < e2->src->index) ? e1 : e2;
}

/* Convert stack register references in one block.  Return true if the CFG
   has been modified in the process.  */

static bool
convert_regs_1 (basic_block block)
{
  struct stack_def regstack;
  block_info bi = BLOCK_INFO (block);
  int reg;
  rtx_insn *insn, *next;
  bool control_flow_insn_deleted = false;
  bool cfg_altered = false;
  int debug_insns_with_starting_stack = 0;

  /* Choose an initial stack layout, if one hasn't already been chosen.  */
  if (bi->stack_in.top == -2)
    {
      edge e, beste = NULL;
      edge_iterator ei;

      /* Select the best incoming edge (typically the most frequent) to
	 use as a template for this basic block.  */
      FOR_EACH_EDGE (e, ei, block->preds)
	if (BLOCK_INFO (e->src)->done)
	  beste = better_edge (beste, e);

      if (beste)
	propagate_stack (beste);
      else
	{
	  /* No predecessors.  Create an arbitrary input stack.  */
	  bi->stack_in.top = -1;
	  for (reg = LAST_STACK_REG; reg >= FIRST_STACK_REG; --reg)
	    if (TEST_HARD_REG_BIT (bi->stack_in.reg_set, reg))
	      bi->stack_in.reg[++bi->stack_in.top] = reg;
	}
    }

  if (dump_file)
    {
      fprintf (dump_file, "\nBasic block %d\nInput stack: ", block->index);
      print_stack (dump_file, &bi->stack_in);
    }

  /* Process all insns in this block.  Keep track of NEXT so that we
     don't process insns emitted while substituting in INSN.  */
  current_block = block;
  next = BB_HEAD (block);
  regstack = bi->stack_in;
  starting_stack_p = true;

  do
    {
      insn = next;
      next = NEXT_INSN (insn);

      /* Ensure we have not missed a block boundary.  */
      gcc_assert (next);
      if (insn == BB_END (block))
	next = NULL;

      /* Don't bother processing unless there is a stack reg
	 mentioned or if it's a CALL_INSN.  */
      if (DEBUG_BIND_INSN_P (insn))
	{
	  if (starting_stack_p)
	    debug_insns_with_starting_stack++;
	  else
	    {
	      subst_all_stack_regs_in_debug_insn (insn, &regstack);

	      /* Nothing must ever die at a debug insn.  If something
		 is referenced in it that becomes dead, it should have
		 died before and the reference in the debug insn
		 should have been removed so as to avoid changing code
		 generation.  */
	      gcc_assert (!find_reg_note (insn, REG_DEAD, NULL));
	    }
	}
      else if (stack_regs_mentioned (insn)
	       || CALL_P (insn))
	{
	  if (dump_file)
	    {
	      fprintf (dump_file, "  insn %d input stack: ",
		       INSN_UID (insn));
	      print_stack (dump_file, &regstack);
	    }
	  if (subst_stack_regs (insn, &regstack))
	    control_flow_insn_deleted = true;
	  starting_stack_p = false;
	}
    }
  while (next);

  if (debug_insns_with_starting_stack)
    {
      /* Since it's the first non-debug instruction that determines
	 the stack requirements of the current basic block, we refrain
	 from updating debug insns before it in the loop above, and
	 fix them up here.  */
      for (insn = BB_HEAD (block); debug_insns_with_starting_stack;
	   insn = NEXT_INSN (insn))
	{
	  if (!DEBUG_BIND_INSN_P (insn))
	    continue;

	  debug_insns_with_starting_stack--;
	  subst_all_stack_regs_in_debug_insn (insn, &bi->stack_in);
	}
    }

  if (dump_file)
    {
      fprintf (dump_file, "Expected live registers [");
      for (reg = FIRST_STACK_REG; reg <= LAST_STACK_REG; ++reg)
	if (TEST_HARD_REG_BIT (bi->out_reg_set, reg))
	  fprintf (dump_file, " %d", reg);
      fprintf (dump_file, " ]\nOutput stack: ");
      print_stack (dump_file, &regstack);
    }

  insn = BB_END (block);
  if (JUMP_P (insn))
    insn = PREV_INSN (insn);

  /* If the function is declared to return a value, but it returns one
     in only some cases, some registers might come live here.  Emit
     necessary moves for them.  */

  for (reg = FIRST_STACK_REG; reg <= LAST_STACK_REG; ++reg)
    {
      if (TEST_HARD_REG_BIT (bi->out_reg_set, reg)
	  && ! TEST_HARD_REG_BIT (regstack.reg_set, reg))
	{
	  rtx set;

	  if (dump_file)
	    fprintf (dump_file, "Emitting insn initializing reg %d\n", reg);

	  set = gen_rtx_SET (FP_MODE_REG (reg, SFmode), not_a_num);
	  insn = emit_insn_after (set, insn);
	  if (subst_stack_regs (insn, &regstack))
	    control_flow_insn_deleted = true;
	}
    }

  /* Amongst the insns possibly deleted during the substitution process above,
     might have been the only trapping insn in the block.  We purge the now
     possibly dead EH edges here to avoid an ICE from fixup_abnormal_edges,
     called at the end of convert_regs.  The order in which we process the
     blocks ensures that we never delete an already processed edge.

     Note that, at this point, the CFG may have been damaged by the emission
     of instructions after an abnormal call, which moves the basic block end
     (and is the reason why we call fixup_abnormal_edges later).  So we must
     be sure that the trapping insn has been deleted before trying to purge
     dead edges, otherwise we risk purging valid edges.

     ??? We are normally supposed not to delete trapping insns, so we pretend
     that the insns deleted above don't actually trap.  It would have been
     better to detect this earlier and avoid creating the EH edge in the first
     place, still, but we don't have enough information at that time.  */

  if (control_flow_insn_deleted && purge_dead_edges (block))
    cfg_altered = true;

  /* Something failed if the stack lives don't match.  If we had malformed
     asms, we zapped the instruction itself, but that didn't produce the
     same pattern of register kills as before.  */

  gcc_assert (regstack.reg_set == bi->out_reg_set || any_malformed_asm);
  bi->stack_out = regstack;
  bi->done = true;

  return cfg_altered;
}

/* Convert registers in all blocks reachable from BLOCK.  Return true if the
   CFG has been modified in the process.  */

static bool
convert_regs_2 (basic_block block)
{
  basic_block *stack, *sp;
  bool cfg_altered = false;

  /* We process the blocks in a top-down manner, in a way such that one block
     is only processed after all its predecessors.  The number of predecessors
     of every block has already been computed.  */

  stack = XNEWVEC (basic_block, n_basic_blocks_for_fn (cfun));
  sp = stack;

  *sp++ = block;

  do
    {
      edge e;
      edge_iterator ei;

      block = *--sp;

      /* Processing BLOCK is achieved by convert_regs_1, which may purge
	 some dead EH outgoing edge after the deletion of the trapping
	 insn inside the block.  Since the number of predecessors of
	 BLOCK's successors was computed based on the initial edge set,
	 we check the necessity to process some of these successors
	 before such an edge deletion may happen.  However, there is
	 a pitfall: if BLOCK is the only predecessor of a successor and
	 the edge between them happens to be deleted, the successor
	 becomes unreachable and should not be processed.  The problem
	 is that there is no way to preventively detect this case so we
	 stack the successor in all cases and hand over the task of
	 fixing up the discrepancy to convert_regs_1.  */

      FOR_EACH_EDGE (e, ei, block->succs)
	if (! (e->flags & EDGE_DFS_BACK))
	  {
	    BLOCK_INFO (e->dest)->predecessors--;
	    if (!BLOCK_INFO (e->dest)->predecessors)
	      *sp++ = e->dest;
	  }

      if (convert_regs_1 (block))
	cfg_altered = true;
    }
  while (sp != stack);

  free (stack);

  return cfg_altered;
}

/* Traverse all basic blocks in a function, converting the register
   references in each insn from the "flat" register file that gcc uses,
   to the stack-like registers the 387 uses.  */

static void
convert_regs (void)
{
  bool cfg_altered = false;
  bool inserted;
  basic_block b;
  edge e;
  edge_iterator ei;

  /* Initialize uninitialized registers on function entry.  */
  inserted = convert_regs_entry ();

  /* Construct the desired stack for function exit.  */
  convert_regs_exit ();
  BLOCK_INFO (EXIT_BLOCK_PTR_FOR_FN (cfun))->done = true;

  /* ??? Future: process inner loops first, and give them arbitrary
     initial stacks which emit_swap_insn can modify.  This ought to
     prevent double fxch that often appears at the head of a loop.  */

  /* Process all blocks reachable from all entry points.  */
  FOR_EACH_EDGE (e, ei, ENTRY_BLOCK_PTR_FOR_FN (cfun)->succs)
    if (convert_regs_2 (e->dest))
      cfg_altered = true;

  /* ??? Process all unreachable blocks.  Though there's no excuse
     for keeping these even when not optimizing.  */
  FOR_EACH_BB_FN (b, cfun)
    {
      block_info bi = BLOCK_INFO (b);

      if (!bi->done && convert_regs_2 (b))
	cfg_altered = true;
    }

  /* We must fix up abnormal edges before inserting compensation code
     because both mechanisms insert insns on edges.  */
  if (fixup_abnormal_edges ())
    inserted = true;

  if (compensate_edges ())
    inserted = true;

  clear_aux_for_blocks ();

  if (inserted)
    commit_edge_insertions ();

  if (cfg_altered)
    cleanup_cfg (0);

  if (dump_file)
    fputc ('\n', dump_file);
}

/* Convert register usage from "flat" register file usage to a "stack
   register file.  FILE is the dump file, if used.

   Construct a CFG and run life analysis.  Then convert each insn one
   by one.  Run a last cleanup_cfg pass, if optimizing, to eliminate
   code duplication created when the converter inserts pop insns on
   the edges.  */

static bool
reg_to_stack (void)
{
  basic_block bb;
  int i;
  int max_uid;

  /* Clean up previous run.  */
  stack_regs_mentioned_data.release ();

  /* See if there is something to do.  Flow analysis is quite
     expensive so we might save some compilation time.  */
  for (i = FIRST_STACK_REG; i <= LAST_STACK_REG; i++)
    if (df_regs_ever_live_p (i))
      break;
  if (i > LAST_STACK_REG)
    return false;

  df_note_add_problem ();
  df_analyze ();

  mark_dfs_back_edges ();

  /* Set up block info for each basic block.  */
  alloc_aux_for_blocks (sizeof (struct block_info_def));
  FOR_EACH_BB_FN (bb, cfun)
    {
      block_info bi = BLOCK_INFO (bb);
      edge_iterator ei;
      edge e;
      int reg;

      FOR_EACH_EDGE (e, ei, bb->preds)
	if (!(e->flags & EDGE_DFS_BACK)
	    && e->src != ENTRY_BLOCK_PTR_FOR_FN (cfun))
	  bi->predecessors++;

      /* Set current register status at last instruction `uninitialized'.  */
      bi->stack_in.top = -2;

      /* Copy live_at_end and live_at_start into temporaries.  */
      for (reg = FIRST_STACK_REG; reg <= LAST_STACK_REG; reg++)
	{
	  if (REGNO_REG_SET_P (DF_LR_OUT (bb), reg))
	    SET_HARD_REG_BIT (bi->out_reg_set, reg);
	  if (REGNO_REG_SET_P (DF_LR_IN (bb), reg))
	    SET_HARD_REG_BIT (bi->stack_in.reg_set, reg);
	}
    }

  /* Create the replacement registers up front.  */
  for (i = FIRST_STACK_REG; i <= LAST_STACK_REG; i++)
    {
      machine_mode mode;
      FOR_EACH_MODE_IN_CLASS (mode, MODE_FLOAT)
	FP_MODE_REG (i, mode) = gen_rtx_REG (mode, i);
      FOR_EACH_MODE_IN_CLASS (mode, MODE_COMPLEX_FLOAT)
	FP_MODE_REG (i, mode) = gen_rtx_REG (mode, i);
    }

  ix86_flags_rtx = gen_rtx_REG (CCmode, FLAGS_REG);

  /* A QNaN for initializing uninitialized variables.

     ??? We can't load from constant memory in PIC mode, because
     we're inserting these instructions before the prologue and
     the PIC register hasn't been set up.  In that case, fall back
     on zero, which we can get from `fldz'.  */

  if ((flag_pic && !TARGET_64BIT)
      || ix86_cmodel == CM_LARGE || ix86_cmodel == CM_LARGE_PIC)
    not_a_num = CONST0_RTX (SFmode);
  else
    {
      REAL_VALUE_TYPE r;

      real_nan (&r, "", 1, SFmode);
      not_a_num = const_double_from_real_value (r, SFmode);
      not_a_num = force_const_mem (SFmode, not_a_num);
    }

  /* Allocate a cache for stack_regs_mentioned.  */
  max_uid = get_max_uid ();
  stack_regs_mentioned_data.create (max_uid + 1);
  memset (stack_regs_mentioned_data.address (),
	  0, sizeof (char) * (max_uid + 1));

  convert_regs ();
  any_malformed_asm = false;

  free_aux_for_blocks ();
  return true;
}
#endif /* STACK_REGS */

namespace {

const pass_data pass_data_stack_regs =
{
  RTL_PASS, /* type */
  "*stack_regs", /* name */
  OPTGROUP_NONE, /* optinfo_flags */
  TV_REG_STACK, /* tv_id */
  0, /* properties_required */
  0, /* properties_provided */
  0, /* properties_destroyed */
  0, /* todo_flags_start */
  0, /* todo_flags_finish */
};

class pass_stack_regs : public rtl_opt_pass
{
public:
  pass_stack_regs (gcc::context *ctxt)
    : rtl_opt_pass (pass_data_stack_regs, ctxt)
  {}

  /* opt_pass methods: */
  bool gate (function *) final override
    {
#ifdef STACK_REGS
      return true;
#else
      return false;
#endif
    }

}; // class pass_stack_regs

} // anon namespace

rtl_opt_pass *
make_pass_stack_regs (gcc::context *ctxt)
{
  return new pass_stack_regs (ctxt);
}

/* Convert register usage from flat register file usage to a stack
   register file.  */
static unsigned int
rest_of_handle_stack_regs (void)
{
#ifdef STACK_REGS
  if (reg_to_stack ())
    df_insn_rescan_all ();
  regstack_completed = 1;
#endif
  return 0;
}

namespace {

const pass_data pass_data_stack_regs_run =
{
  RTL_PASS, /* type */
  "stack", /* name */
  OPTGROUP_NONE, /* optinfo_flags */
  TV_REG_STACK, /* tv_id */
  0, /* properties_required */
  0, /* properties_provided */
  0, /* properties_destroyed */
  0, /* todo_flags_start */
  TODO_df_finish, /* todo_flags_finish */
};

class pass_stack_regs_run : public rtl_opt_pass
{
public:
  pass_stack_regs_run (gcc::context *ctxt)
    : rtl_opt_pass (pass_data_stack_regs_run, ctxt)
  {}

  /* opt_pass methods: */
  unsigned int execute (function *) final override
    {
      return rest_of_handle_stack_regs ();
    }

}; // class pass_stack_regs_run

} // anon namespace

rtl_opt_pass *
make_pass_stack_regs_run (gcc::context *ctxt)
{
  return new pass_stack_regs_run (ctxt);
}
