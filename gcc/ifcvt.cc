/* If-conversion support.
   Copyright (C) 2000-2025 Free Software Foundation, Inc.

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

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "target.h"
#include "rtl.h"
#include "tree.h"
#include "cfghooks.h"
#include "df.h"
#include "memmodel.h"
#include "tm_p.h"
#include "expmed.h"
#include "optabs.h"
#include "regs.h"
#include "emit-rtl.h"
#include "recog.h"

#include "cfgrtl.h"
#include "cfganal.h"
#include "cfgcleanup.h"
#include "expr.h"
#include "output.h"
#include "cfgloop.h"
#include "tree-pass.h"
#include "dbgcnt.h"
#include "shrink-wrap.h"
#include "rtl-iter.h"
#include "ifcvt.h"

#ifndef MAX_CONDITIONAL_EXECUTE
#define MAX_CONDITIONAL_EXECUTE \
  (BRANCH_COST (optimize_function_for_speed_p (cfun), false) \
   + 1)
#endif

#define IFCVT_MULTIPLE_DUMPS 1

#define NULL_BLOCK	((basic_block) NULL)

/* True if after combine pass.  */
static bool ifcvt_after_combine;

/* True if the target has the cbranchcc4 optab.  */
static bool have_cbranchcc4;

/* # of IF-THEN or IF-THEN-ELSE blocks we looked at  */
static int num_possible_if_blocks;

/* # of IF-THEN or IF-THEN-ELSE blocks were converted to conditional
   execution.  */
static int num_updated_if_blocks;

/* # of changes made.  */
static int num_true_changes;

/* Whether conditional execution changes were made.  */
static bool cond_exec_changed_p;

/* Forward references.  */
static int count_bb_insns (const_basic_block);
static bool cheap_bb_rtx_cost_p (const_basic_block, profile_probability, int);
static rtx_insn *first_active_insn (basic_block);
static rtx_insn *last_active_insn (basic_block, bool);
static rtx_insn *find_active_insn_before (basic_block, rtx_insn *);
static rtx_insn *find_active_insn_after (basic_block, rtx_insn *);
static basic_block block_fallthru (basic_block);
static rtx cond_exec_get_condition (rtx_insn *, bool);
static rtx noce_get_condition (rtx_insn *, rtx_insn **, bool);
static bool noce_operand_ok (const_rtx);
static void merge_if_block (ce_if_block *);
static bool find_cond_trap (basic_block, edge, edge);
static basic_block find_if_header (basic_block, int);
static int block_jumps_and_fallthru (basic_block, basic_block);
static bool noce_find_if_block (basic_block, edge, edge, int);
static bool cond_exec_find_if_block (ce_if_block *);
static bool find_if_case_1 (basic_block, edge, edge);
static bool find_if_case_2 (basic_block, edge, edge);
static bool dead_or_predicable (basic_block, basic_block, basic_block,
				edge, bool);
static void noce_emit_move_insn (rtx, rtx);
static rtx_insn *block_has_only_trap (basic_block);
static void init_noce_multiple_sets_info (basic_block,
  auto_delete_vec<noce_multiple_sets_info> &);
static bool noce_convert_multiple_sets_1 (struct noce_if_info *,
  auto_delete_vec<noce_multiple_sets_info> &, int *);

/* Count the number of non-jump active insns in BB.  */

static int
count_bb_insns (const_basic_block bb)
{
  int count = 0;
  rtx_insn *insn = BB_HEAD (bb);

  while (1)
    {
      if (active_insn_p (insn) && !JUMP_P (insn))
	count++;

      if (insn == BB_END (bb))
	break;
      insn = NEXT_INSN (insn);
    }

  return count;
}

/* Determine whether the total insn_cost on non-jump insns in
   basic block BB is less than MAX_COST.  This function returns
   false if the cost of any instruction could not be estimated.

   The cost of the non-jump insns in BB is scaled by REG_BR_PROB_BASE
   as those insns are being speculated.  MAX_COST is scaled with SCALE
   plus a small fudge factor.  */

static bool
cheap_bb_rtx_cost_p (const_basic_block bb,
		     profile_probability prob, int max_cost)
{
  int count = 0;
  rtx_insn *insn = BB_HEAD (bb);
  bool speed = optimize_bb_for_speed_p (bb);
  int scale = prob.initialized_p () ? prob.to_reg_br_prob_base ()
	      : REG_BR_PROB_BASE;

  /* Set scale to REG_BR_PROB_BASE to void the identical scaling
     applied to insn_cost when optimizing for size.  Only do
     this after combine because if-conversion might interfere with
     passes before combine.

     Use optimize_function_for_speed_p instead of the pre-defined
     variable speed to make sure it is set to same value for all
     basic blocks in one if-conversion transformation.  */
  if (!optimize_function_for_speed_p (cfun) && ifcvt_after_combine)
    scale = REG_BR_PROB_BASE;
  /* Our branch probability/scaling factors are just estimates and don't
     account for cases where we can get speculation for free and other
     secondary benefits.  So we fudge the scale factor to make speculating
     appear a little more profitable when optimizing for performance.  */
  else
    scale += REG_BR_PROB_BASE / 8;


  max_cost *= scale;

  while (1)
    {
      if (NONJUMP_INSN_P (insn))
	{
	  /* Inline-asm's cost is not very estimatable.
	     It could be a costly instruction but the
	     estimate would be the same as a non costly
	     instruction.  */
	  if (asm_noperands (PATTERN (insn)) >= 0)
	    return false;

	  int cost = insn_cost (insn, speed) * REG_BR_PROB_BASE;
	  if (cost == 0)
	    return false;

	  /* If this instruction is the load or set of a "stack" register,
	     such as a floating point register on x87, then the cost of
	     speculatively executing this insn may need to include
	     the additional cost of popping its result off of the
	     register stack.  Unfortunately, correctly recognizing and
	     accounting for this additional overhead is tricky, so for
	     now we simply prohibit such speculative execution.  */
#ifdef STACK_REGS
	  {
	    rtx set = single_set (insn);
	    if (set && STACK_REG_P (SET_DEST (set)))
	      return false;
	  }
#endif

	  count += cost;
	  if (count >= max_cost)
	    return false;
	}
      else if (CALL_P (insn))
	return false;

      if (insn == BB_END (bb))
	break;
      insn = NEXT_INSN (insn);
    }

  return true;
}

/* Return the first non-jump active insn in the basic block.  */

static rtx_insn *
first_active_insn (basic_block bb)
{
  rtx_insn *insn = BB_HEAD (bb);

  if (LABEL_P (insn))
    {
      if (insn == BB_END (bb))
	return NULL;
      insn = NEXT_INSN (insn);
    }

  while (NOTE_P (insn) || DEBUG_INSN_P (insn))
    {
      if (insn == BB_END (bb))
	return NULL;
      insn = NEXT_INSN (insn);
    }

  if (JUMP_P (insn))
    return NULL;

  return insn;
}

/* Return the last non-jump active (non-jump) insn in the basic block.  */

static rtx_insn *
last_active_insn (basic_block bb, bool skip_use_p)
{
  rtx_insn *insn = BB_END (bb);
  rtx_insn *head = BB_HEAD (bb);

  while (NOTE_P (insn)
	 || JUMP_P (insn)
	 || DEBUG_INSN_P (insn)
	 || (skip_use_p
	     && NONJUMP_INSN_P (insn)
	     && GET_CODE (PATTERN (insn)) == USE))
    {
      if (insn == head)
	return NULL;
      insn = PREV_INSN (insn);
    }

  if (LABEL_P (insn))
    return NULL;

  return insn;
}

/* Return the active insn before INSN inside basic block CURR_BB. */

static rtx_insn *
find_active_insn_before (basic_block curr_bb, rtx_insn *insn)
{
  if (!insn || insn == BB_HEAD (curr_bb))
    return NULL;

  while ((insn = PREV_INSN (insn)) != NULL_RTX)
    {
      if (NONJUMP_INSN_P (insn) || JUMP_P (insn) || CALL_P (insn))
        break;

      /* No other active insn all the way to the start of the basic block. */
      if (insn == BB_HEAD (curr_bb))
        return NULL;
    }

  return insn;
}

/* Return the active insn after INSN inside basic block CURR_BB. */

static rtx_insn *
find_active_insn_after (basic_block curr_bb, rtx_insn *insn)
{
  if (!insn || insn == BB_END (curr_bb))
    return NULL;

  while ((insn = NEXT_INSN (insn)) != NULL_RTX)
    {
      if (NONJUMP_INSN_P (insn) || JUMP_P (insn) || CALL_P (insn))
        break;

      /* No other active insn all the way to the end of the basic block. */
      if (insn == BB_END (curr_bb))
        return NULL;
    }

  return insn;
}

/* Return the basic block reached by falling though the basic block BB.  */

static basic_block
block_fallthru (basic_block bb)
{
  edge e = find_fallthru_edge (bb->succs);

  return (e) ? e->dest : NULL_BLOCK;
}

/* Return true if RTXs A and B can be safely interchanged.  */

static bool
rtx_interchangeable_p (const_rtx a, const_rtx b)
{
  if (!rtx_equal_p (a, b))
    return false;

  if (GET_CODE (a) != MEM)
    return true;

  /* A dead type-unsafe memory reference is legal, but a live type-unsafe memory
     reference is not.  Interchanging a dead type-unsafe memory reference with
     a live type-safe one creates a live type-unsafe memory reference, in other
     words, it makes the program illegal.
     We check here conservatively whether the two memory references have equal
     memory attributes.  */

  return mem_attrs_eq_p (get_mem_attrs (a), get_mem_attrs (b));
}


/* Go through a bunch of insns, converting them to conditional
   execution format if possible.  Return TRUE if all of the non-note
   insns were processed.  */

static bool
cond_exec_process_insns (ce_if_block *ce_info ATTRIBUTE_UNUSED,
			 /* if block information */rtx_insn *start,
			 /* first insn to look at */rtx end,
			 /* last insn to look at */rtx test,
			 /* conditional execution test */profile_probability
							    prob_val,
			 /* probability of branch taken. */bool mod_ok)
{
  bool must_be_last = false;
  rtx_insn *insn;
  rtx xtest;
  rtx pattern;

  if (!start || !end)
    return false;

  for (insn = start; ; insn = NEXT_INSN (insn))
    {
      /* dwarf2out can't cope with conditional prologues.  */
      if (NOTE_P (insn) && NOTE_KIND (insn) == NOTE_INSN_PROLOGUE_END)
	return false;

      if (NOTE_P (insn) || DEBUG_INSN_P (insn))
	goto insn_done;

      gcc_assert (NONJUMP_INSN_P (insn) || CALL_P (insn));

      /* dwarf2out can't cope with conditional unwind info.  */
      if (RTX_FRAME_RELATED_P (insn))
	return false;

      /* Remove USE insns that get in the way.  */
      if (reload_completed && GET_CODE (PATTERN (insn)) == USE)
	{
	  /* ??? Ug.  Actually unlinking the thing is problematic,
	     given what we'd have to coordinate with our callers.  */
	  SET_INSN_DELETED (insn);
	  goto insn_done;
	}

      /* Last insn wasn't last?  */
      if (must_be_last)
	return false;

      if (modified_in_p (test, insn))
	{
	  if (!mod_ok)
	    return false;
	  must_be_last = true;
	}

      /* Now build the conditional form of the instruction.  */
      pattern = PATTERN (insn);
      xtest = copy_rtx (test);

      /* If this is already a COND_EXEC, rewrite the test to be an AND of the
         two conditions.  */
      if (GET_CODE (pattern) == COND_EXEC)
	{
	  if (GET_MODE (xtest) != GET_MODE (COND_EXEC_TEST (pattern)))
	    return false;

	  xtest = gen_rtx_AND (GET_MODE (xtest), xtest,
			       COND_EXEC_TEST (pattern));
	  pattern = COND_EXEC_CODE (pattern);
	}

      pattern = gen_rtx_COND_EXEC (VOIDmode, xtest, pattern);

      /* If the machine needs to modify the insn being conditionally executed,
         say for example to force a constant integer operand into a temp
         register, do so here.  */
#ifdef IFCVT_MODIFY_INSN
      IFCVT_MODIFY_INSN (ce_info, pattern, insn);
      if (! pattern)
	return false;
#endif

      validate_change (insn, &PATTERN (insn), pattern, 1);

      if (CALL_P (insn) && prob_val.initialized_p ())
	validate_change (insn, &REG_NOTES (insn),
			 gen_rtx_INT_LIST ((machine_mode) REG_BR_PROB,
					   prob_val.to_reg_br_prob_note (),
					   REG_NOTES (insn)), 1);

    insn_done:
      if (insn == end)
	break;
    }

  return true;
}

/* Return the condition for a jump.  Do not do any special processing.  */

static rtx
cond_exec_get_condition (rtx_insn *jump, bool get_reversed = false)
{
  rtx test_if, cond;

  if (any_condjump_p (jump))
    test_if = SET_SRC (pc_set (jump));
  else
    return NULL_RTX;
  cond = XEXP (test_if, 0);

  /* If this branches to JUMP_LABEL when the condition is false,
     reverse the condition.  */
  if (get_reversed
      || (GET_CODE (XEXP (test_if, 2)) == LABEL_REF
	  && label_ref_label (XEXP (test_if, 2))
	  == JUMP_LABEL (jump)))
    {
      enum rtx_code rev = reversed_comparison_code (cond, jump);
      if (rev == UNKNOWN)
	return NULL_RTX;

      cond = gen_rtx_fmt_ee (rev, GET_MODE (cond), XEXP (cond, 0),
			     XEXP (cond, 1));
    }

  return cond;
}

/* Given a simple IF-THEN or IF-THEN-ELSE block, attempt to convert it
   to conditional execution.  Return TRUE if we were successful at
   converting the block.  */

static bool
cond_exec_process_if_block (ce_if_block * ce_info,
			    /* if block information */bool do_multiple_p)
{
  basic_block test_bb = ce_info->test_bb;	/* last test block */
  basic_block then_bb = ce_info->then_bb;	/* THEN */
  basic_block else_bb = ce_info->else_bb;	/* ELSE or NULL */
  rtx test_expr;		/* expression in IF_THEN_ELSE that is tested */
  rtx_insn *then_start;		/* first insn in THEN block */
  rtx_insn *then_end;		/* last insn + 1 in THEN block */
  rtx_insn *else_start = NULL;	/* first insn in ELSE block or NULL */
  rtx_insn *else_end = NULL;	/* last insn + 1 in ELSE block */
  int max;			/* max # of insns to convert.  */
  bool then_mod_ok;		/* whether conditional mods are ok in THEN */
  rtx true_expr;		/* test for else block insns */
  rtx false_expr;		/* test for then block insns */
  profile_probability true_prob_val;/* probability of else block */
  profile_probability false_prob_val;/* probability of then block */
  rtx_insn *then_last_head = NULL;	/* Last match at the head of THEN */
  rtx_insn *else_last_head = NULL;	/* Last match at the head of ELSE */
  rtx_insn *then_first_tail = NULL;	/* First match at the tail of THEN */
  rtx_insn *else_first_tail = NULL;	/* First match at the tail of ELSE */
  int then_n_insns, else_n_insns, n_insns;
  enum rtx_code false_code;
  rtx note;

  /* If test is comprised of && or || elements, and we've failed at handling
     all of them together, just use the last test if it is the special case of
     && elements without an ELSE block.  */
  if (!do_multiple_p && ce_info->num_multiple_test_blocks)
    {
      if (else_bb || ! ce_info->and_and_p)
	return false;

      ce_info->test_bb = test_bb = ce_info->last_test_bb;
      ce_info->num_multiple_test_blocks = 0;
      ce_info->num_and_and_blocks = 0;
      ce_info->num_or_or_blocks = 0;
    }

  /* Find the conditional jump to the ELSE or JOIN part, and isolate
     the test.  */
  test_expr = cond_exec_get_condition (BB_END (test_bb));
  if (! test_expr)
    return false;

  /* If the conditional jump is more than just a conditional jump,
     then we cannot do conditional execution conversion on this block.  */
  if (! onlyjump_p (BB_END (test_bb)))
    return false;

  /* Collect the bounds of where we're to search, skipping any labels, jumps
     and notes at the beginning and end of the block.  Then count the total
     number of insns and see if it is small enough to convert.  */
  then_start = first_active_insn (then_bb);
  then_end = last_active_insn (then_bb, true);
  then_n_insns = ce_info->num_then_insns = count_bb_insns (then_bb);
  n_insns = then_n_insns;
  max = MAX_CONDITIONAL_EXECUTE;

  if (else_bb)
    {
      int n_matching;

      max *= 2;
      else_start = first_active_insn (else_bb);
      else_end = last_active_insn (else_bb, true);
      else_n_insns = ce_info->num_else_insns = count_bb_insns (else_bb);
      n_insns += else_n_insns;

      /* Look for matching sequences at the head and tail of the two blocks,
	 and limit the range of insns to be converted if possible.  */
      n_matching = flow_find_cross_jump (then_bb, else_bb,
					 &then_first_tail, &else_first_tail,
					 NULL);
      if (then_first_tail == BB_HEAD (then_bb))
	then_start = then_end = NULL;
      if (else_first_tail == BB_HEAD (else_bb))
	else_start = else_end = NULL;

      if (n_matching > 0)
	{
	  if (then_end)
	    then_end = find_active_insn_before (then_bb, then_first_tail);
	  if (else_end)
	    else_end = find_active_insn_before (else_bb, else_first_tail);
	  n_insns -= 2 * n_matching;
	}

      if (then_start
	  && else_start
	  && then_n_insns > n_matching
	  && else_n_insns > n_matching)
	{
	  int longest_match = MIN (then_n_insns - n_matching,
				   else_n_insns - n_matching);
	  n_matching
	    = flow_find_head_matching_sequence (then_bb, else_bb,
						&then_last_head,
						&else_last_head,
						longest_match);

	  if (n_matching > 0)
	    {
	      rtx_insn *insn;

	      /* We won't pass the insns in the head sequence to
		 cond_exec_process_insns, so we need to test them here
		 to make sure that they don't clobber the condition.  */
	      for (insn = BB_HEAD (then_bb);
		   insn != NEXT_INSN (then_last_head);
		   insn = NEXT_INSN (insn))
		if (!LABEL_P (insn) && !NOTE_P (insn)
		    && !DEBUG_INSN_P (insn)
		    && modified_in_p (test_expr, insn))
		  return false;
	    }

	  if (then_last_head == then_end)
	    then_start = then_end = NULL;
	  if (else_last_head == else_end)
	    else_start = else_end = NULL;

	  if (n_matching > 0)
	    {
	      if (then_start)
		then_start = find_active_insn_after (then_bb, then_last_head);
	      if (else_start)
		else_start = find_active_insn_after (else_bb, else_last_head);
	      n_insns -= 2 * n_matching;
	    }
	}
    }

  if (n_insns > max)
    return false;

  /* Map test_expr/test_jump into the appropriate MD tests to use on
     the conditionally executed code.  */

  true_expr = test_expr;

  false_code = reversed_comparison_code (true_expr, BB_END (test_bb));
  if (false_code != UNKNOWN)
    false_expr = gen_rtx_fmt_ee (false_code, GET_MODE (true_expr),
				 XEXP (true_expr, 0), XEXP (true_expr, 1));
  else
    false_expr = NULL_RTX;

#ifdef IFCVT_MODIFY_TESTS
  /* If the machine description needs to modify the tests, such as setting a
     conditional execution register from a comparison, it can do so here.  */
  IFCVT_MODIFY_TESTS (ce_info, true_expr, false_expr);

  /* See if the conversion failed.  */
  if (!true_expr || !false_expr)
    goto fail;
#endif

  note = find_reg_note (BB_END (test_bb), REG_BR_PROB, NULL_RTX);
  if (note)
    {
      true_prob_val = profile_probability::from_reg_br_prob_note (XINT (note, 0));
      false_prob_val = true_prob_val.invert ();
    }
  else
    {
      true_prob_val = profile_probability::uninitialized ();
      false_prob_val = profile_probability::uninitialized ();
    }

  /* If we have && or || tests, do them here.  These tests are in the adjacent
     blocks after the first block containing the test.  */
  if (ce_info->num_multiple_test_blocks > 0)
    {
      basic_block bb = test_bb;
      basic_block last_test_bb = ce_info->last_test_bb;

      if (! false_expr)
	goto fail;

      do
	{
	  rtx_insn *start, *end;
	  rtx t, f;
	  enum rtx_code f_code;

	  bb = block_fallthru (bb);
	  start = first_active_insn (bb);
	  end = last_active_insn (bb, true);
	  if (start
	      && ! cond_exec_process_insns (ce_info, start, end, false_expr,
					    false_prob_val, false))
	    goto fail;

	  /* If the conditional jump is more than just a conditional jump, then
	     we cannot do conditional execution conversion on this block.  */
	  if (! onlyjump_p (BB_END (bb)))
	    goto fail;

	  /* Find the conditional jump and isolate the test.  */
	  t = cond_exec_get_condition (BB_END (bb));
	  if (! t)
	    goto fail;

	  f_code = reversed_comparison_code (t, BB_END (bb));
	  if (f_code == UNKNOWN)
	    goto fail;

	  f = gen_rtx_fmt_ee (f_code, GET_MODE (t), XEXP (t, 0), XEXP (t, 1));
	  if (ce_info->and_and_p)
	    {
	      t = gen_rtx_AND (GET_MODE (t), true_expr, t);
	      f = gen_rtx_IOR (GET_MODE (t), false_expr, f);
	    }
	  else
	    {
	      t = gen_rtx_IOR (GET_MODE (t), true_expr, t);
	      f = gen_rtx_AND (GET_MODE (t), false_expr, f);
	    }

	  /* If the machine description needs to modify the tests, such as
	     setting a conditional execution register from a comparison, it can
	     do so here.  */
#ifdef IFCVT_MODIFY_MULTIPLE_TESTS
	  IFCVT_MODIFY_MULTIPLE_TESTS (ce_info, bb, t, f);

	  /* See if the conversion failed.  */
	  if (!t || !f)
	    goto fail;
#endif

	  true_expr = t;
	  false_expr = f;
	}
      while (bb != last_test_bb);
    }

  /* For IF-THEN-ELSE blocks, we don't allow modifications of the test
     on then THEN block.  */
  then_mod_ok = (else_bb == NULL_BLOCK);

  /* Go through the THEN and ELSE blocks converting the insns if possible
     to conditional execution.  */

  if (then_end
      && (! false_expr
	  || ! cond_exec_process_insns (ce_info, then_start, then_end,
					false_expr, false_prob_val,
					then_mod_ok)))
    goto fail;

  if (else_bb && else_end
      && ! cond_exec_process_insns (ce_info, else_start, else_end,
				    true_expr, true_prob_val, true))
    goto fail;

  /* If we cannot apply the changes, fail.  Do not go through the normal fail
     processing, since apply_change_group will call cancel_changes.  */
  if (! apply_change_group ())
    {
#ifdef IFCVT_MODIFY_CANCEL
      /* Cancel any machine dependent changes.  */
      IFCVT_MODIFY_CANCEL (ce_info);
#endif
      return false;
    }

#ifdef IFCVT_MODIFY_FINAL
  /* Do any machine dependent final modifications.  */
  IFCVT_MODIFY_FINAL (ce_info);
#endif

  /* Conversion succeeded.  */
  if (dump_file)
    fprintf (dump_file, "%d insn%s converted to conditional execution.\n",
	     n_insns, (n_insns == 1) ? " was" : "s were");

  /* Merge the blocks!  If we had matching sequences, make sure to delete one
     copy at the appropriate location first: delete the copy in the THEN branch
     for a tail sequence so that the remaining one is executed last for both
     branches, and delete the copy in the ELSE branch for a head sequence so
     that the remaining one is executed first for both branches.  */
  if (then_first_tail)
    {
      rtx_insn *from = then_first_tail;
      if (!INSN_P (from))
	from = find_active_insn_after (then_bb, from);
      delete_insn_chain (from, get_last_bb_insn (then_bb), false);
    }
  if (else_last_head)
    delete_insn_chain (first_active_insn (else_bb), else_last_head, false);

  merge_if_block (ce_info);
  cond_exec_changed_p = true;
  return true;

 fail:
#ifdef IFCVT_MODIFY_CANCEL
  /* Cancel any machine dependent changes.  */
  IFCVT_MODIFY_CANCEL (ce_info);
#endif

  cancel_changes (0);
  return false;
}

static rtx noce_emit_store_flag (struct noce_if_info *, rtx, bool, int);
static bool noce_try_move (struct noce_if_info *);
static bool noce_try_ifelse_collapse (struct noce_if_info *);
static bool noce_try_store_flag (struct noce_if_info *);
static bool noce_try_addcc (struct noce_if_info *);
static bool noce_try_store_flag_constants (struct noce_if_info *);
static bool noce_try_store_flag_mask (struct noce_if_info *);
static rtx noce_emit_cmove (struct noce_if_info *, rtx, enum rtx_code, rtx,
			    rtx, rtx, rtx, rtx = NULL, rtx = NULL);
static bool noce_try_cmove (struct noce_if_info *);
static bool noce_try_cmove_arith (struct noce_if_info *);
static rtx noce_get_alt_condition (struct noce_if_info *, rtx, rtx_insn **);
static bool noce_try_minmax (struct noce_if_info *);
static bool noce_try_abs (struct noce_if_info *);
static bool noce_try_sign_mask (struct noce_if_info *);
static int noce_try_cond_zero_arith (struct noce_if_info *);

/* Return the comparison code for reversed condition for IF_INFO,
   or UNKNOWN if reversing the condition is not possible.  */

static inline enum rtx_code
noce_reversed_cond_code (struct noce_if_info *if_info)
{
  if (if_info->rev_cond)
    return GET_CODE (if_info->rev_cond);
  return reversed_comparison_code (if_info->cond, if_info->jump);
}

/* Return true if SEQ is a good candidate as a replacement for the
   if-convertible sequence described in IF_INFO.
   This is the default implementation that targets can override
   through a target hook.  */

bool
default_noce_conversion_profitable_p (rtx_insn *seq,
				      struct noce_if_info *if_info)
{
  bool speed_p = if_info->speed_p;

  /* Cost up the new sequence.  */
  unsigned int cost = seq_cost (seq, speed_p);

  if (cost <= if_info->original_cost)
    return true;

  /* When compiling for size, we can make a reasonably accurately guess
     at the size growth.  When compiling for speed, use the maximum.  */
  return speed_p && cost <= if_info->max_seq_cost;
}

/* Helper function for noce_try_store_flag*.  */

static rtx
noce_emit_store_flag (struct noce_if_info *if_info, rtx x, bool reversep,
		      int normalize)
{
  rtx cond = if_info->cond;
  bool cond_complex;
  enum rtx_code code;

  cond_complex = (! general_operand (XEXP (cond, 0), VOIDmode)
		  || ! general_operand (XEXP (cond, 1), VOIDmode));

  /* If earliest == jump, or when the condition is complex, try to
     build the store_flag insn directly.  */

  if (cond_complex)
    {
      rtx set = pc_set (if_info->jump);
      cond = XEXP (SET_SRC (set), 0);
      if (GET_CODE (XEXP (SET_SRC (set), 2)) == LABEL_REF
	  && label_ref_label (XEXP (SET_SRC (set), 2)) == JUMP_LABEL (if_info->jump))
	reversep = !reversep;
      if (if_info->then_else_reversed)
	reversep = !reversep;
    }
  else if (reversep
	   && if_info->rev_cond
	   && general_operand (XEXP (if_info->rev_cond, 0), VOIDmode)
	   && general_operand (XEXP (if_info->rev_cond, 1), VOIDmode))
    {
      cond = if_info->rev_cond;
      reversep = false;
    }

  if (reversep)
    code = reversed_comparison_code (cond, if_info->jump);
  else
    code = GET_CODE (cond);

  if ((if_info->cond_earliest == if_info->jump || cond_complex)
      && (normalize == 0 || STORE_FLAG_VALUE == normalize))
    {
      rtx src = gen_rtx_fmt_ee (code, GET_MODE (x), XEXP (cond, 0),
				XEXP (cond, 1));
      rtx set = gen_rtx_SET (x, src);

      start_sequence ();
      rtx_insn *insn = emit_insn (set);

      if (recog_memoized (insn) >= 0)
	{
	  rtx_insn *seq = end_sequence ();
	  emit_insn (seq);

	  if_info->cond_earliest = if_info->jump;

	  return x;
	}

      end_sequence ();
    }

  /* Don't even try if the comparison operands or the mode of X are weird.  */
  if (cond_complex || !SCALAR_INT_MODE_P (GET_MODE (x)))
    return NULL_RTX;

  return emit_store_flag (x, code, XEXP (cond, 0),
			  XEXP (cond, 1), VOIDmode,
			  (code == LTU || code == LEU
			   || code == GEU || code == GTU), normalize);
}

/* Return true if X can be safely forced into a register by copy_to_mode_reg
   / force_operand.  */

static bool
noce_can_force_operand (rtx x)
{
  if (general_operand (x, VOIDmode))
    return true;
  if (SUBREG_P (x))
    {
      if (!noce_can_force_operand (SUBREG_REG (x)))
	return false;
      return true;
    }
  if (ARITHMETIC_P (x))
    {
      if (!noce_can_force_operand (XEXP (x, 0))
	  || !noce_can_force_operand (XEXP (x, 1)))
	return false;
      switch (GET_CODE (x))
	{
	case MULT:
	case DIV:
	case MOD:
	case UDIV:
	case UMOD:
	  return true;
	default:
	  return code_to_optab (GET_CODE (x));
	}
    }
  if (UNARY_P (x))
    {
      if (!noce_can_force_operand (XEXP (x, 0)))
	return false;
      switch (GET_CODE (x))
	{
	case ZERO_EXTEND:
	case SIGN_EXTEND:
	case TRUNCATE:
	case FLOAT_EXTEND:
	case FLOAT_TRUNCATE:
	case FIX:
	case UNSIGNED_FIX:
	case FLOAT:
	case UNSIGNED_FLOAT:
	  return true;
	default:
	  return code_to_optab (GET_CODE (x));
	}
    }
  return false;
}

/* Emit instruction to move an rtx, possibly into STRICT_LOW_PART.
   X is the destination/target and Y is the value to copy.  */

static void
noce_emit_move_insn (rtx x, rtx y)
{
  machine_mode outmode;
  rtx outer, inner;
  poly_int64 bitpos;

  if (GET_CODE (x) != STRICT_LOW_PART)
    {
      rtx_insn *seq, *insn;
      rtx target;
      optab ot;

      start_sequence ();
      /* Check that the SET_SRC is reasonable before calling emit_move_insn,
	 otherwise construct a suitable SET pattern ourselves.  */
      insn = (OBJECT_P (y) || CONSTANT_P (y) || GET_CODE (y) == SUBREG)
	     ? emit_move_insn (x, y)
	     : emit_insn (gen_rtx_SET (x, y));
      seq = end_sequence ();

      if (recog_memoized (insn) <= 0)
	{
	  if (GET_CODE (x) == ZERO_EXTRACT)
	    {
	      rtx op = XEXP (x, 0);
	      unsigned HOST_WIDE_INT size = INTVAL (XEXP (x, 1));
	      unsigned HOST_WIDE_INT start = INTVAL (XEXP (x, 2));

	      /* store_bit_field expects START to be relative to
		 BYTES_BIG_ENDIAN and adjusts this value for machines with
		 BITS_BIG_ENDIAN != BYTES_BIG_ENDIAN.  In order to be able to
		 invoke store_bit_field again it is necessary to have the START
		 value from the first call.  */
	      if (BITS_BIG_ENDIAN != BYTES_BIG_ENDIAN)
		{
		  if (MEM_P (op))
		    start = BITS_PER_UNIT - start - size;
		  else
		    {
		      gcc_assert (REG_P (op));
		      start = BITS_PER_WORD - start - size;
		    }
		}

	      gcc_assert (start < (MEM_P (op) ? BITS_PER_UNIT : BITS_PER_WORD));
	      store_bit_field (op, size, start, 0, 0, GET_MODE (x), y, false,
			       false);
	      return;
	    }

	  switch (GET_RTX_CLASS (GET_CODE (y)))
	    {
	    case RTX_UNARY:
	      ot = code_to_optab (GET_CODE (y));
	      if (ot && noce_can_force_operand (XEXP (y, 0)))
		{
		  start_sequence ();
		  target = expand_unop (GET_MODE (y), ot, XEXP (y, 0), x, 0);
		  if (target != NULL_RTX)
		    {
		      if (target != x)
			emit_move_insn (x, target);
		      seq = get_insns ();
		    }
		  end_sequence ();
		}
	      break;

	    case RTX_BIN_ARITH:
	    case RTX_COMM_ARITH:
	      ot = code_to_optab (GET_CODE (y));
	      if (ot
		  && noce_can_force_operand (XEXP (y, 0))
		  && noce_can_force_operand (XEXP (y, 1)))
		{
		  start_sequence ();
		  target = expand_binop (GET_MODE (y), ot,
					 XEXP (y, 0), XEXP (y, 1),
					 x, 0, OPTAB_DIRECT);
		  if (target != NULL_RTX)
		    {
		      if (target != x)
			  emit_move_insn (x, target);
		      seq = get_insns ();
		    }
		  end_sequence ();
		}
	      break;

	    default:
	      break;
	    }
	}

      emit_insn (seq);
      return;
    }

  outer = XEXP (x, 0);
  inner = XEXP (outer, 0);
  outmode = GET_MODE (outer);
  bitpos = SUBREG_BYTE (outer) * BITS_PER_UNIT;
  store_bit_field (inner, GET_MODE_BITSIZE (outmode), bitpos,
		   0, 0, outmode, y, false, false);
}

/* Return the CC reg if it is used in COND.  */

static rtx
cc_in_cond (rtx cond)
{
  if (have_cbranchcc4 && cond
      && GET_MODE_CLASS (GET_MODE (XEXP (cond, 0))) == MODE_CC)
    return XEXP (cond, 0);

  return NULL_RTX;
}

/* Return sequence of instructions generated by if conversion.  This
   function calls end_sequence() to end the current stream, ensures
   that the instructions are unshared, recognizable non-jump insns.
   On failure, this function returns a NULL_RTX.  */

static rtx_insn *
end_ifcvt_sequence (struct noce_if_info *if_info)
{
  rtx_insn *insn;
  rtx_insn *seq = get_insns ();
  rtx cc = cc_in_cond (if_info->cond);

  set_used_flags (if_info->x);
  set_used_flags (if_info->cond);
  set_used_flags (if_info->a);
  set_used_flags (if_info->b);

  for (insn = seq; insn; insn = NEXT_INSN (insn))
    set_used_flags (insn);

  unshare_all_rtl_in_chain (seq);
  end_sequence ();

  /* Make sure that all of the instructions emitted are recognizable,
     and that we haven't introduced a new jump instruction.
     As an exercise for the reader, build a general mechanism that
     allows proper placement of required clobbers.  */
  for (insn = seq; insn; insn = NEXT_INSN (insn))
    if (JUMP_P (insn)
	|| recog_memoized (insn) == -1
	   /* Make sure new generated code does not clobber CC.  */
	|| (cc && set_of (cc, insn)))
      return NULL;

  return seq;
}

/* Return true iff the then and else basic block (if it exists)
   consist of a single simple set instruction.  */

static bool
noce_simple_bbs (struct noce_if_info *if_info)
{
  if (!if_info->then_simple)
    return false;

  if (if_info->else_bb)
    return if_info->else_simple;

  return true;
}

/* Convert "if (a != b) x = a; else x = b" into "x = a" and
   "if (a == b) x = a; else x = b" into "x = b".  */

static bool
noce_try_move (struct noce_if_info *if_info)
{
  rtx cond = if_info->cond;
  enum rtx_code code = GET_CODE (cond);
  rtx y;
  rtx_insn *seq;

  if (code != NE && code != EQ)
    return false;

  if (!noce_simple_bbs (if_info))
    return false;

  /* This optimization isn't valid if either A or B could be a NaN
     or a signed zero.  */
  if (HONOR_NANS (if_info->x)
      || HONOR_SIGNED_ZEROS (if_info->x))
    return false;

  /* Check whether the operands of the comparison are A and in
     either order.  */
  if ((rtx_equal_p (if_info->a, XEXP (cond, 0))
       && rtx_equal_p (if_info->b, XEXP (cond, 1)))
      || (rtx_equal_p (if_info->a, XEXP (cond, 1))
	  && rtx_equal_p (if_info->b, XEXP (cond, 0))))
    {
      if (!rtx_interchangeable_p (if_info->a, if_info->b))
	return false;

      y = (code == EQ) ? if_info->a : if_info->b;

      /* Avoid generating the move if the source is the destination.  */
      if (! rtx_equal_p (if_info->x, y))
	{
	  start_sequence ();
	  noce_emit_move_insn (if_info->x, y);
	  seq = end_ifcvt_sequence (if_info);
	  if (!seq)
	    return false;

	  emit_insn_before_setloc (seq, if_info->jump,
				   INSN_LOCATION (if_info->insn_a));
	}
      if_info->transform_name = "noce_try_move";
      return true;
    }
  return false;
}

/* Try forming an IF_THEN_ELSE (cond, b, a) and collapsing that
   through simplify_rtx.  Sometimes that can eliminate the IF_THEN_ELSE.
   If that is the case, emit the result into x.  */

static bool
noce_try_ifelse_collapse (struct noce_if_info * if_info)
{
  if (!noce_simple_bbs (if_info))
    return false;

  machine_mode mode = GET_MODE (if_info->x);
  rtx if_then_else = simplify_gen_ternary (IF_THEN_ELSE, mode, mode,
					    if_info->cond, if_info->b,
					    if_info->a);

  if (GET_CODE (if_then_else) == IF_THEN_ELSE)
    return false;

  rtx_insn *seq;
  start_sequence ();
  noce_emit_move_insn (if_info->x, if_then_else);
  seq = end_ifcvt_sequence (if_info);
  if (!seq)
    return false;

  emit_insn_before_setloc (seq, if_info->jump,
			  INSN_LOCATION (if_info->insn_a));

  if_info->transform_name = "noce_try_ifelse_collapse";
  return true;
}


/* Convert "if (test) x = 1; else x = 0".

   Only try 0 and STORE_FLAG_VALUE here.  Other combinations will be
   tried in noce_try_store_flag_constants after noce_try_cmove has had
   a go at the conversion.  */

static bool
noce_try_store_flag (struct noce_if_info *if_info)
{
  bool reversep;
  rtx target;
  rtx_insn *seq;

  if (!noce_simple_bbs (if_info))
    return false;

  if (CONST_INT_P (if_info->b)
      && INTVAL (if_info->b) == STORE_FLAG_VALUE
      && if_info->a == const0_rtx)
    reversep = false;
  else if (if_info->b == const0_rtx
	   && CONST_INT_P (if_info->a)
	   && INTVAL (if_info->a) == STORE_FLAG_VALUE
	   && noce_reversed_cond_code (if_info) != UNKNOWN)
    reversep = true;
  else
    return false;

  start_sequence ();

  target = noce_emit_store_flag (if_info, if_info->x, reversep, 0);
  if (target)
    {
      if (target != if_info->x)
	noce_emit_move_insn (if_info->x, target);

      seq = end_ifcvt_sequence (if_info);
      if (! seq)
	return false;

      emit_insn_before_setloc (seq, if_info->jump,
			       INSN_LOCATION (if_info->insn_a));
      if_info->transform_name = "noce_try_store_flag";
      return true;
    }
  else
    {
      end_sequence ();
      return false;
    }
}


/* Convert "if (test) x = -A; else x = A" into
   x = A; if (test) x = -x if the machine can do the
   conditional negate form of this cheaply.
   Try this before noce_try_cmove that will just load the
   immediates into two registers and do a conditional select
   between them.  If the target has a conditional negate or
   conditional invert operation we can save a potentially
   expensive constant synthesis.  */

static bool
noce_try_inverse_constants (struct noce_if_info *if_info)
{
  if (!noce_simple_bbs (if_info))
    return false;

  if (!CONST_INT_P (if_info->a)
      || !CONST_INT_P (if_info->b)
      || !REG_P (if_info->x))
    return false;

  machine_mode mode = GET_MODE (if_info->x);

  HOST_WIDE_INT val_a = INTVAL (if_info->a);
  HOST_WIDE_INT val_b = INTVAL (if_info->b);

  rtx cond = if_info->cond;

  rtx x = if_info->x;
  rtx target;

  start_sequence ();

  rtx_code code;
  if (val_b != HOST_WIDE_INT_MIN && val_a == -val_b)
    code = NEG;
  else if (val_a == ~val_b)
    code = NOT;
  else
    {
      end_sequence ();
      return false;
    }

  rtx tmp = gen_reg_rtx (mode);
  noce_emit_move_insn (tmp, if_info->a);

  target = emit_conditional_neg_or_complement (x, code, mode, cond, tmp, tmp);

  if (target)
    {
      rtx_insn *seq = get_insns ();

      if (!seq)
	{
	  end_sequence ();
	  return false;
	}

      if (target != if_info->x)
	noce_emit_move_insn (if_info->x, target);

      seq = end_ifcvt_sequence (if_info);

      if (!seq)
	return false;

      emit_insn_before_setloc (seq, if_info->jump,
			       INSN_LOCATION (if_info->insn_a));
      if_info->transform_name = "noce_try_inverse_constants";
      return true;
    }

  end_sequence ();
  return false;
}


/* Convert "if (test) x = a; else x = b", for A and B constant.
   Also allow A = y + c1, B = y + c2, with a common y between A
   and B.  */

static bool
noce_try_store_flag_constants (struct noce_if_info *if_info)
{
  rtx target;
  rtx_insn *seq;
  bool reversep;
  HOST_WIDE_INT itrue, ifalse, diff, tmp;
  int normalize;
  bool can_reverse;
  machine_mode mode = GET_MODE (if_info->x);
  rtx common = NULL_RTX;

  rtx a = if_info->a;
  rtx b = if_info->b;

  /* Handle cases like x := test ? y + 3 : y + 4.  */
  if (GET_CODE (a) == PLUS
      && GET_CODE (b) == PLUS
      && CONST_INT_P (XEXP (a, 1))
      && CONST_INT_P (XEXP (b, 1))
      && rtx_equal_p (XEXP (a, 0), XEXP (b, 0))
      /* Allow expressions that are not using the result or plain
         registers where we handle overlap below.  */
      && (REG_P (XEXP (a, 0))
	  || (noce_operand_ok (XEXP (a, 0))
	      && ! reg_overlap_mentioned_p (if_info->x, XEXP (a, 0)))))
    {
      common = XEXP (a, 0);
      a = XEXP (a, 1);
      b = XEXP (b, 1);
    }

  if (!noce_simple_bbs (if_info))
    return false;

  if (CONST_INT_P (a)
      && CONST_INT_P (b))
    {
      ifalse = INTVAL (a);
      itrue = INTVAL (b);
      bool subtract_flag_p = false;

      diff = (unsigned HOST_WIDE_INT) itrue - ifalse;
      /* Make sure we can represent the difference between the two values.  */
      if ((diff > 0)
	  != ((ifalse < 0) != (itrue < 0) ? ifalse < 0 : ifalse < itrue))
	return false;

      diff = trunc_int_for_mode (diff, mode);

      can_reverse = noce_reversed_cond_code (if_info) != UNKNOWN;
      reversep = false;
      if (diff == STORE_FLAG_VALUE || diff == -STORE_FLAG_VALUE)
	{
	  normalize = 0;
	  /* We could collapse these cases but it is easier to follow the
	     diff/STORE_FLAG_VALUE combinations when they are listed
	     explicitly.  */

	  /* test ? 3 : 4
	     => 4 + (test != 0).  */
	  if (diff < 0 && STORE_FLAG_VALUE < 0)
	      reversep = false;
	  /* test ? 4 : 3
	     => can_reverse  | 4 + (test == 0)
		!can_reverse | 3 - (test != 0).  */
	  else if (diff > 0 && STORE_FLAG_VALUE < 0)
	    {
	      reversep = can_reverse;
	      subtract_flag_p = !can_reverse;
	      /* If we need to subtract the flag and we have PLUS-immediate
		 A and B then it is unlikely to be beneficial to play tricks
		 here.  */
	      if (subtract_flag_p && common)
		return false;
	    }
	  /* test ? 3 : 4
	     => can_reverse  | 3 + (test == 0)
		!can_reverse | 4 - (test != 0).  */
	  else if (diff < 0 && STORE_FLAG_VALUE > 0)
	    {
	      reversep = can_reverse;
	      subtract_flag_p = !can_reverse;
	      /* If we need to subtract the flag and we have PLUS-immediate
		 A and B then it is unlikely to be beneficial to play tricks
		 here.  */
	      if (subtract_flag_p && common)
		return false;
	    }
	  /* test ? 4 : 3
	     => 4 + (test != 0).  */
	  else if (diff > 0 && STORE_FLAG_VALUE > 0)
	    reversep = false;
	  else
	    gcc_unreachable ();
	}
      /* Is this (cond) ? 2^n : 0?  */
      else if (ifalse == 0 && pow2p_hwi (itrue)
	       && STORE_FLAG_VALUE == 1)
	normalize = 1;
      /* Is this (cond) ? 0 : 2^n?  */
      else if (itrue == 0 && pow2p_hwi (ifalse) && can_reverse
	       && STORE_FLAG_VALUE == 1)
	{
	  normalize = 1;
	  reversep = true;
	}
      /* Is this (cond) ? -1 : x?  */
      else if (itrue == -1
	       && STORE_FLAG_VALUE == -1)
	normalize = -1;
      /* Is this (cond) ? x : -1?  */
      else if (ifalse == -1 && can_reverse
	       && STORE_FLAG_VALUE == -1)
	{
	  normalize = -1;
	  reversep = true;
	}
      else
	return false;

      if (reversep)
	{
	  std::swap (itrue, ifalse);
	  diff = trunc_int_for_mode (-(unsigned HOST_WIDE_INT) diff, mode);
	}

      start_sequence ();

      /* If we have x := test ? x + 3 : x + 4 then move the original
	 x out of the way while we store flags.  */
      if (common && rtx_equal_p (common, if_info->x))
	{
	  common = gen_reg_rtx (mode);
	  noce_emit_move_insn (common, if_info->x);
	}

      target = noce_emit_store_flag (if_info, if_info->x, reversep, normalize);
      if (! target)
	{
	  end_sequence ();
	  return false;
	}

      /* if (test) x = 3; else x = 4;
	 =>   x = 3 + (test == 0);  */
      if (diff == STORE_FLAG_VALUE || diff == -STORE_FLAG_VALUE)
	{
	  /* Add the common part now.  This may allow combine to merge this
	     with the store flag operation earlier into some sort of conditional
	     increment/decrement if the target allows it.  */
	  if (common)
	    target = expand_simple_binop (mode, PLUS,
					   target, common,
					   target, 0, OPTAB_WIDEN);

	  /* Always use ifalse here.  It should have been swapped with itrue
	     when appropriate when reversep is true.  */
	  target = expand_simple_binop (mode, subtract_flag_p ? MINUS : PLUS,
					gen_int_mode (ifalse, mode), target,
					if_info->x, 0, OPTAB_WIDEN);
	}
      /* Other cases are not beneficial when the original A and B are PLUS
	 expressions.  */
      else if (common)
	{
	  end_sequence ();
	  return false;
	}
      /* if (test) x = 8; else x = 0;
	 =>   x = (test != 0) << 3;  */
      else if (ifalse == 0 && (tmp = exact_log2 (itrue)) >= 0)
	{
	  target = expand_simple_binop (mode, ASHIFT,
					target, GEN_INT (tmp), if_info->x, 0,
					OPTAB_WIDEN);
	}

      /* if (test) x = -1; else x = b;
	 =>   x = -(test != 0) | b;  */
      else if (itrue == -1)
	{
	  target = expand_simple_binop (mode, IOR,
					target, gen_int_mode (ifalse, mode),
					if_info->x, 0, OPTAB_WIDEN);
	}
      else
	{
	  end_sequence ();
	  return false;
	}

      if (! target)
	{
	  end_sequence ();
	  return false;
	}

      if (target != if_info->x)
	noce_emit_move_insn (if_info->x, target);

      seq = end_ifcvt_sequence (if_info);
      if (!seq || !targetm.noce_conversion_profitable_p (seq, if_info))
	return false;

      emit_insn_before_setloc (seq, if_info->jump,
			       INSN_LOCATION (if_info->insn_a));
      if_info->transform_name = "noce_try_store_flag_constants";

      return true;
    }

  return false;
}

/* Convert "if (test) foo++" into "foo += (test != 0)", and
   similarly for "foo--".  */

static bool
noce_try_addcc (struct noce_if_info *if_info)
{
  rtx target;
  rtx_insn *seq;
  bool subtract;
  int normalize;

  if (!noce_simple_bbs (if_info))
    return false;

  if (GET_CODE (if_info->a) == PLUS
      && rtx_equal_p (XEXP (if_info->a, 0), if_info->b)
      && noce_reversed_cond_code (if_info) != UNKNOWN)
    {
      rtx cond = if_info->rev_cond;
      enum rtx_code code;

      if (cond == NULL_RTX)
	{
	  cond = if_info->cond;
	  code = reversed_comparison_code (cond, if_info->jump);
	}
      else
	code = GET_CODE (cond);

      /* First try to use addcc pattern.  */
      if (general_operand (XEXP (cond, 0), VOIDmode)
	  && general_operand (XEXP (cond, 1), VOIDmode))
	{
	  start_sequence ();
	  target = emit_conditional_add (if_info->x, code,
					 XEXP (cond, 0),
					 XEXP (cond, 1),
					 VOIDmode,
					 if_info->b,
					 XEXP (if_info->a, 1),
					 GET_MODE (if_info->x),
					 (code == LTU || code == GEU
					  || code == LEU || code == GTU));
	  if (target)
	    {
	      if (target != if_info->x)
		noce_emit_move_insn (if_info->x, target);

	      seq = end_ifcvt_sequence (if_info);
	      if (!seq || !targetm.noce_conversion_profitable_p (seq, if_info))
		return false;

	      emit_insn_before_setloc (seq, if_info->jump,
				       INSN_LOCATION (if_info->insn_a));
	      if_info->transform_name = "noce_try_addcc";

	      return true;
	    }
	  end_sequence ();
	}

      /* If that fails, construct conditional increment or decrement using
	 setcc.  We're changing a branch and an increment to a comparison and
	 an ADD/SUB.  */
      if (XEXP (if_info->a, 1) == const1_rtx
	  || XEXP (if_info->a, 1) == constm1_rtx)
        {
	  start_sequence ();
	  if (STORE_FLAG_VALUE == INTVAL (XEXP (if_info->a, 1)))
	    subtract = false, normalize = 0;
	  else if (-STORE_FLAG_VALUE == INTVAL (XEXP (if_info->a, 1)))
	    subtract = true, normalize = 0;
	  else
	    subtract = false, normalize = INTVAL (XEXP (if_info->a, 1));


	  target = noce_emit_store_flag (if_info,
					 gen_reg_rtx (GET_MODE (if_info->x)),
					 true, normalize);

	  if (target)
	    target = expand_simple_binop (GET_MODE (if_info->x),
					  subtract ? MINUS : PLUS,
					  if_info->b, target, if_info->x,
					  0, OPTAB_WIDEN);
	  if (target)
	    {
	      if (target != if_info->x)
		noce_emit_move_insn (if_info->x, target);

	      seq = end_ifcvt_sequence (if_info);
	      if (!seq || !targetm.noce_conversion_profitable_p (seq, if_info))
		return false;

	      emit_insn_before_setloc (seq, if_info->jump,
				       INSN_LOCATION (if_info->insn_a));
	      if_info->transform_name = "noce_try_addcc";
	      return true;
	    }
	  end_sequence ();
	}
    }

  return false;
}

/* Convert "if (test) x = 0;" to "x &= -(test == 0);"  */

static bool
noce_try_store_flag_mask (struct noce_if_info *if_info)
{
  rtx target;
  rtx_insn *seq;
  bool reversep;

  if (!noce_simple_bbs (if_info))
    return false;

  reversep = false;

  if ((if_info->a == const0_rtx
       && (REG_P (if_info->b) || rtx_equal_p (if_info->b, if_info->x)))
      || ((reversep = (noce_reversed_cond_code (if_info) != UNKNOWN))
	  && if_info->b == const0_rtx
	  && (REG_P (if_info->a) || rtx_equal_p (if_info->a, if_info->x))))
    {
      start_sequence ();
      target = noce_emit_store_flag (if_info,
				     gen_reg_rtx (GET_MODE (if_info->x)),
				     reversep, -1);
      if (target)
        target = expand_simple_binop (GET_MODE (if_info->x), AND,
				      reversep ? if_info->a : if_info->b,
				      target, if_info->x, 0,
				      OPTAB_WIDEN);

      if (target)
	{
	  if (target != if_info->x)
	    noce_emit_move_insn (if_info->x, target);

	  seq = end_ifcvt_sequence (if_info);
	  if (!seq || !targetm.noce_conversion_profitable_p (seq, if_info))
	    return false;

	  emit_insn_before_setloc (seq, if_info->jump,
				   INSN_LOCATION (if_info->insn_a));
	  if_info->transform_name = "noce_try_store_flag_mask";

	  return true;
	}

      end_sequence ();
    }

  return false;
}

/* Helper function for noce_try_cmove and noce_try_cmove_arith.  */

static rtx
noce_emit_cmove (struct noce_if_info *if_info, rtx x, enum rtx_code code,
		 rtx cmp_a, rtx cmp_b, rtx vfalse, rtx vtrue, rtx cc_cmp,
		 rtx rev_cc_cmp)
{
  rtx target ATTRIBUTE_UNUSED;
  bool unsignedp ATTRIBUTE_UNUSED;

  /* If earliest == jump, try to build the cmove insn directly.
     This is helpful when combine has created some complex condition
     (like for alpha's cmovlbs) that we can't hope to regenerate
     through the normal interface.  */

  if (if_info->cond_earliest == if_info->jump)
    {
      rtx cond = gen_rtx_fmt_ee (code, GET_MODE (if_info->cond), cmp_a, cmp_b);
      rtx if_then_else = gen_rtx_IF_THEN_ELSE (GET_MODE (x),
					       cond, vtrue, vfalse);
      rtx set = gen_rtx_SET (x, if_then_else);

      start_sequence ();
      rtx_insn *insn = emit_insn (set);

      if (recog_memoized (insn) >= 0)
	{
	  rtx_insn *seq = end_sequence ();
	  emit_insn (seq);

	  return x;
	}

      end_sequence ();
    }

  unsignedp = (code == LTU || code == GEU
	       || code == LEU || code == GTU);

  if (cc_cmp != NULL_RTX && rev_cc_cmp != NULL_RTX)
    target = emit_conditional_move (x, cc_cmp, rev_cc_cmp,
				    vtrue, vfalse, GET_MODE (x));
  else
    {
      /* Don't even try if the comparison operands are weird
	 except that the target supports cbranchcc4.  */
      if (! general_operand (cmp_a, GET_MODE (cmp_a))
	  || ! general_operand (cmp_b, GET_MODE (cmp_b)))
	{
	  if (!have_cbranchcc4
	      || GET_MODE_CLASS (GET_MODE (cmp_a)) != MODE_CC
	      || cmp_b != const0_rtx)
	    return NULL_RTX;
	}

      target = emit_conditional_move (x, { code, cmp_a, cmp_b, VOIDmode },
				      vtrue, vfalse, GET_MODE (x),
				      unsignedp);
    }

  if (target)
    return target;

  /* We might be faced with a situation like:

     x = (reg:M TARGET)
     vtrue = (subreg:M (reg:N VTRUE) BYTE)
     vfalse = (subreg:M (reg:N VFALSE) BYTE)

     We can't do a conditional move in mode M, but it's possible that we
     could do a conditional move in mode N instead and take a subreg of
     the result.

     If we can't create new pseudos, though, don't bother.  */
  if (reload_completed)
    return NULL_RTX;

  if (GET_CODE (vtrue) == SUBREG && GET_CODE (vfalse) == SUBREG)
    {
      rtx reg_vtrue = SUBREG_REG (vtrue);
      rtx reg_vfalse = SUBREG_REG (vfalse);
      poly_uint64 byte_vtrue = SUBREG_BYTE (vtrue);
      poly_uint64 byte_vfalse = SUBREG_BYTE (vfalse);
      rtx promoted_target;

      if (GET_MODE (reg_vtrue) != GET_MODE (reg_vfalse)
	  || maybe_ne (byte_vtrue, byte_vfalse)
	  || (SUBREG_PROMOTED_VAR_P (vtrue)
	      != SUBREG_PROMOTED_VAR_P (vfalse))
	  || (SUBREG_PROMOTED_GET (vtrue)
	      != SUBREG_PROMOTED_GET (vfalse)))
	return NULL_RTX;

      promoted_target = gen_reg_rtx (GET_MODE (reg_vtrue));

      target = emit_conditional_move (promoted_target,
				      { code, cmp_a, cmp_b, VOIDmode },
				      reg_vtrue, reg_vfalse,
				      GET_MODE (reg_vtrue), unsignedp);
      /* Nope, couldn't do it in that mode either.  */
      if (!target)
	return NULL_RTX;

      target = gen_rtx_SUBREG (GET_MODE (vtrue), promoted_target, byte_vtrue);
      SUBREG_PROMOTED_VAR_P (target) = SUBREG_PROMOTED_VAR_P (vtrue);
      SUBREG_PROMOTED_SET (target, SUBREG_PROMOTED_GET (vtrue));
      emit_move_insn (x, target);
      return x;
    }
  else
    return NULL_RTX;
}

/*  Emit a conditional zero, returning TARGET or NULL_RTX upon failure.
    IF_INFO describes the if-conversion scenario under consideration.
    CZERO_CODE selects the condition (EQ/NE).
    NON_ZERO_OP is the nonzero operand of the conditional move
    TARGET is the desired output register.  */

static rtx
noce_emit_czero (struct noce_if_info *if_info, enum rtx_code czero_code,
		 rtx non_zero_op, rtx target)
{
  machine_mode mode = GET_MODE (target);
  rtx cond_op0 = XEXP (if_info->cond, 0);
  rtx czero_cond
    = gen_rtx_fmt_ee (czero_code, GET_MODE (cond_op0), cond_op0, const0_rtx);
  rtx if_then_else
    = gen_rtx_IF_THEN_ELSE (mode, czero_cond, const0_rtx, non_zero_op);
  rtx set = gen_rtx_SET (target, if_then_else);

  rtx_insn *insn = make_insn_raw (set);

  if (recog_memoized (insn) >= 0)
    {
      add_insn (insn);
      return target;
    }

  return NULL_RTX;
}

/* Try only simple constants and registers here.  More complex cases
   are handled in noce_try_cmove_arith after noce_try_store_flag_arith
   has had a go at it.  */

static bool
noce_try_cmove (struct noce_if_info *if_info)
{
  enum rtx_code code;
  rtx target;
  rtx_insn *seq;

  if (!noce_simple_bbs (if_info))
    return false;

  if ((CONSTANT_P (if_info->a) || register_operand (if_info->a, VOIDmode))
      && (CONSTANT_P (if_info->b) || register_operand (if_info->b, VOIDmode)))
    {
      start_sequence ();

      code = GET_CODE (if_info->cond);
      target = noce_emit_cmove (if_info, if_info->x, code,
				XEXP (if_info->cond, 0),
				XEXP (if_info->cond, 1),
				if_info->a, if_info->b);

      if (target)
	{
	  if (target != if_info->x)
	    noce_emit_move_insn (if_info->x, target);

	  seq = end_ifcvt_sequence (if_info);
	  if (!seq || !targetm.noce_conversion_profitable_p (seq, if_info))
	    return false;

	  emit_insn_before_setloc (seq, if_info->jump,
				   INSN_LOCATION (if_info->insn_a));
	  if_info->transform_name = "noce_try_cmove";

	  return true;
	}
      /* If both a and b are constants try a last-ditch transformation:
	 if (test) x = a; else x = b;
	 =>   x = (-(test != 0) & (b - a)) + a;
	 Try this only if the target-specific expansion above has failed.
	 The target-specific expander may want to generate sequences that
	 we don't know about, so give them a chance before trying this
	 approach.  */
      else if (!targetm.have_conditional_execution ()
		&& CONST_INT_P (if_info->a) && CONST_INT_P (if_info->b))
	{
	  machine_mode mode = GET_MODE (if_info->x);
	  HOST_WIDE_INT ifalse = INTVAL (if_info->a);
	  HOST_WIDE_INT itrue = INTVAL (if_info->b);
	  rtx target = noce_emit_store_flag (if_info, if_info->x, false, -1);
	  if (!target)
	    {
	      end_sequence ();
	      return false;
	    }

	  HOST_WIDE_INT diff = (unsigned HOST_WIDE_INT) itrue - ifalse;
	  /* Make sure we can represent the difference
	     between the two values.  */
	  if ((diff > 0)
	      != ((ifalse < 0) != (itrue < 0) ? ifalse < 0 : ifalse < itrue))
	    {
	      end_sequence ();
	      return false;
	    }

	  diff = trunc_int_for_mode (diff, mode);
	  target = expand_simple_binop (mode, AND,
					target, gen_int_mode (diff, mode),
					if_info->x, 0, OPTAB_WIDEN);
	  if (target)
	    target = expand_simple_binop (mode, PLUS,
					  target, gen_int_mode (ifalse, mode),
					  if_info->x, 0, OPTAB_WIDEN);
	  if (target)
	    {
	      if (target != if_info->x)
		noce_emit_move_insn (if_info->x, target);

	      seq = end_ifcvt_sequence (if_info);
	      if (!seq || !targetm.noce_conversion_profitable_p (seq, if_info))
		return false;

	      emit_insn_before_setloc (seq, if_info->jump,
				   INSN_LOCATION (if_info->insn_a));
	      if_info->transform_name = "noce_try_cmove";
	      return true;
	    }
	  else
	    {
	      end_sequence ();
	      return false;
	    }
	}
      else
	end_sequence ();
    }

  return false;
}

/* Return true if X contains a conditional code mode rtx.  */

static bool
contains_ccmode_rtx_p (rtx x)
{
  subrtx_iterator::array_type array;
  FOR_EACH_SUBRTX (iter, array, x, ALL)
    if (GET_MODE_CLASS (GET_MODE (*iter)) == MODE_CC)
      return true;

  return false;
}

/* Helper for bb_valid_for_noce_process_p.  Validate that
   the rtx insn INSN is a single set that does not set
   the conditional register CC and is in general valid for
   if-conversion.  */

static bool
insn_valid_noce_process_p (rtx_insn *insn, rtx cc)
{
  if (!insn
      || !NONJUMP_INSN_P (insn)
      || (cc && set_of (cc, insn)))
      return false;

  rtx sset = single_set (insn);

  /* Currently support only simple single sets in test_bb.  */
  if (!sset
      || !noce_operand_ok (SET_DEST (sset))
      || contains_ccmode_rtx_p (SET_DEST (sset))
      || !noce_operand_ok (SET_SRC (sset)))
    return false;

  return true;
}


/* Return true iff the registers that the insns in BB_A set do not get
   used in BB_B.  If TO_RENAME is non-NULL then it is a location that will be
   renamed later by the caller and so conflicts on it should be ignored
   in this function.  */

static bool
bbs_ok_for_cmove_arith (basic_block bb_a, basic_block bb_b, rtx to_rename)
{
  rtx_insn *a_insn;
  bitmap bba_sets = BITMAP_ALLOC (&reg_obstack);

  df_ref def;
  df_ref use;

  FOR_BB_INSNS (bb_a, a_insn)
    {
      if (!active_insn_p (a_insn))
	continue;

      rtx sset_a = single_set (a_insn);

      if (!sset_a)
	{
	  BITMAP_FREE (bba_sets);
	  return false;
	}
      /* Record all registers that BB_A sets.  */
      FOR_EACH_INSN_DEF (def, a_insn)
	if (!(to_rename && DF_REF_REG (def) == to_rename))
	  bitmap_set_bit (bba_sets, DF_REF_REGNO (def));
    }

  rtx_insn *b_insn;

  FOR_BB_INSNS (bb_b, b_insn)
    {
      if (!active_insn_p (b_insn))
	continue;

      rtx sset_b = single_set (b_insn);

      if (!sset_b)
	{
	  BITMAP_FREE (bba_sets);
	  return false;
	}

      /* Make sure this is a REG and not some instance
	 of ZERO_EXTRACT or non-paradoxical SUBREG or other dangerous stuff.
	 If we have a memory destination then we have a pair of simple
	 basic blocks performing an operation of the form [addr] = c ? a : b.
	 bb_valid_for_noce_process_p will have ensured that these are
	 the only stores present.  In that case [addr] should be the location
	 to be renamed.  Assert that the callers set this up properly.  */
      if (MEM_P (SET_DEST (sset_b)))
	gcc_assert (rtx_equal_p (SET_DEST (sset_b), to_rename));
      else if (!REG_P (SET_DEST (sset_b))
	       && !paradoxical_subreg_p (SET_DEST (sset_b)))
	{
	  BITMAP_FREE (bba_sets);
	  return false;
	}

      /* If the insn uses a reg set in BB_A return false.  */
      FOR_EACH_INSN_USE (use, b_insn)
	{
	  if (bitmap_bit_p (bba_sets, DF_REF_REGNO (use)))
	    {
	      BITMAP_FREE (bba_sets);
	      return false;
	    }
	}

    }

  BITMAP_FREE (bba_sets);
  return true;
}

/* Emit copies of all the active instructions in BB except the last.
   This is a helper for noce_try_cmove_arith.  */

static void
noce_emit_all_but_last (basic_block bb)
{
  rtx_insn *last = last_active_insn (bb, false);
  rtx_insn *insn;
  FOR_BB_INSNS (bb, insn)
    {
      if (insn != last && active_insn_p (insn))
	{
	  rtx_insn *to_emit = as_a <rtx_insn *> (copy_rtx (insn));

	  emit_insn (PATTERN (to_emit));
	}
    }
}

/* Helper for noce_try_cmove_arith.  Emit the pattern TO_EMIT and return
   the resulting insn or NULL if it's not a valid insn.  */

static rtx_insn *
noce_emit_insn (rtx to_emit)
{
  gcc_assert (to_emit);
  rtx_insn *insn = emit_insn (to_emit);

  if (recog_memoized (insn) < 0)
    return NULL;

  return insn;
}

/* Helper for noce_try_cmove_arith.  Emit a copy of the insns up to
   and including the penultimate one in BB if it is not simple
   (as indicated by SIMPLE).  Then emit LAST_INSN as the last
   insn in the block.  The reason for that is that LAST_INSN may
   have been modified by the preparation in noce_try_cmove_arith.  */

static bool
noce_emit_bb (rtx last_insn, basic_block bb, bool simple)
{
  if (bb && !simple)
    noce_emit_all_but_last (bb);

  if (last_insn && !noce_emit_insn (last_insn))
    return false;

  return true;
}

/* Try more complex cases involving conditional_move.  */

static bool
noce_try_cmove_arith (struct noce_if_info *if_info)
{
  rtx a = if_info->a;
  rtx b = if_info->b;
  rtx x = if_info->x;
  rtx orig_a, orig_b;
  rtx_insn *insn_a, *insn_b;
  bool a_simple = if_info->then_simple;
  bool b_simple = if_info->else_simple;
  basic_block then_bb = if_info->then_bb;
  basic_block else_bb = if_info->else_bb;
  rtx target;
  bool is_mem = false;
  enum rtx_code code;
  rtx cond = if_info->cond;
  rtx_insn *ifcvt_seq;

  /* A conditional move from two memory sources is equivalent to a
     conditional on their addresses followed by a load.  Don't do this
     early because it'll screw alias analysis.  Note that we've
     already checked for no side effects.  */
  if (cse_not_expected
      && MEM_P (a) && MEM_P (b)
      && MEM_ADDR_SPACE (a) == MEM_ADDR_SPACE (b))
    {
      machine_mode address_mode = get_address_mode (a);

      a = XEXP (a, 0);
      b = XEXP (b, 0);
      x = gen_reg_rtx (address_mode);
      is_mem = true;
    }

  /* ??? We could handle this if we knew that a load from A or B could
     not trap or fault.  This is also true if we've already loaded
     from the address along the path from ENTRY.  */
  else if (may_trap_or_fault_p (a) || may_trap_or_fault_p (b))
    return false;

  /* if (test) x = a + b; else x = c - d;
     => y = a + b;
        x = c - d;
	if (test)
	  x = y;
  */

  code = GET_CODE (cond);
  insn_a = if_info->insn_a;
  insn_b = if_info->insn_b;

  machine_mode x_mode = GET_MODE (x);

  if (!can_conditionally_move_p (x_mode))
    return false;

  /* Possibly rearrange operands to make things come out more natural.  */
  if (noce_reversed_cond_code (if_info) != UNKNOWN)
    {
      bool reversep = false;
      if (rtx_equal_p (b, x))
	reversep = true;
      else if (general_operand (b, GET_MODE (b)))
	reversep = true;

      if (reversep)
	{
	  if (if_info->rev_cond)
	    {
	      cond = if_info->rev_cond;
	      code = GET_CODE (cond);
	    }
	  else
	    code = reversed_comparison_code (cond, if_info->jump);
	  std::swap (a, b);
	  std::swap (insn_a, insn_b);
	  std::swap (a_simple, b_simple);
	  std::swap (then_bb, else_bb);
	}
    }

  if (then_bb && else_bb
      && (!bbs_ok_for_cmove_arith (then_bb, else_bb,  if_info->orig_x)
	  || !bbs_ok_for_cmove_arith (else_bb, then_bb,  if_info->orig_x)))
    return false;

  start_sequence ();

  /* If one of the blocks is empty then the corresponding B or A value
     came from the test block.  The non-empty complex block that we will
     emit might clobber the register used by B or A, so move it to a pseudo
     first.  */

  rtx tmp_a = NULL_RTX;
  rtx tmp_b = NULL_RTX;

  if (b_simple || !else_bb)
    tmp_b = gen_reg_rtx (x_mode);

  if (a_simple || !then_bb)
    tmp_a = gen_reg_rtx (x_mode);

  orig_a = a;
  orig_b = b;

  rtx emit_a = NULL_RTX;
  rtx emit_b = NULL_RTX;
  rtx_insn *tmp_insn = NULL;
  bool modified_in_a = false;
  bool modified_in_b = false;
  /* If either operand is complex, load it into a register first.
     The best way to do this is to copy the original insn.  In this
     way we preserve any clobbers etc that the insn may have had.
     This is of course not possible in the IS_MEM case.  */

  if (! general_operand (a, GET_MODE (a)) || tmp_a)
    {

      if (is_mem)
	{
	  rtx reg = gen_reg_rtx (GET_MODE (a));
	  emit_a = gen_rtx_SET (reg, a);
	}
      else
	{
	  if (insn_a)
	    {
	      a = tmp_a ? tmp_a : gen_reg_rtx (GET_MODE (a));

	      rtx_insn *copy_of_a = as_a <rtx_insn *> (copy_rtx (insn_a));
	      rtx set = single_set (copy_of_a);
	      SET_DEST (set) = a;

	      emit_a = PATTERN (copy_of_a);
	    }
	  else
	    {
	      rtx tmp_reg = tmp_a ? tmp_a : gen_reg_rtx (GET_MODE (a));
	      emit_a = gen_rtx_SET (tmp_reg, a);
	      a = tmp_reg;
	    }
	}
    }

  if (! general_operand (b, GET_MODE (b)) || tmp_b)
    {
      if (is_mem)
	{
          rtx reg = gen_reg_rtx (GET_MODE (b));
	  emit_b = gen_rtx_SET (reg, b);
	}
      else
	{
	  if (insn_b)
	    {
	      b = tmp_b ? tmp_b : gen_reg_rtx (GET_MODE (b));
	      rtx_insn *copy_of_b = as_a <rtx_insn *> (copy_rtx (insn_b));
	      rtx set = single_set (copy_of_b);

	      SET_DEST (set) = b;
	      emit_b = PATTERN (copy_of_b);
	    }
	  else
	    {
	      rtx tmp_reg = tmp_b ? tmp_b : gen_reg_rtx (GET_MODE (b));
	      emit_b = gen_rtx_SET (tmp_reg, b);
	      b = tmp_reg;
	    }
	}
    }

  modified_in_a = emit_a != NULL_RTX && modified_in_p (orig_b, emit_a);
  if (tmp_b && then_bb)
    {
      FOR_BB_INSNS (then_bb, tmp_insn)
	/* Don't check inside insn_a.  We will have changed it to emit_a
	   with a destination that doesn't conflict.  */
	if (!(insn_a && tmp_insn == insn_a)
	    && modified_in_p (orig_b, tmp_insn))
	  {
	    modified_in_a = true;
	    break;
	  }

    }

  modified_in_b = emit_b != NULL_RTX && modified_in_p (orig_a, emit_b);
  if (tmp_a && else_bb)
    {
      FOR_BB_INSNS (else_bb, tmp_insn)
      /* Don't check inside insn_b.  We will have changed it to emit_b
	 with a destination that doesn't conflict.  */
      if (!(insn_b && tmp_insn == insn_b)
	  && modified_in_p (orig_a, tmp_insn))
	{
	  modified_in_b = true;
	  break;
	}
    }

  /* If insn to set up A clobbers any registers B depends on, try to
     swap insn that sets up A with the one that sets up B.  If even
     that doesn't help, punt.  */
  if (modified_in_a && !modified_in_b)
    {
      if (!noce_emit_bb (emit_b, else_bb, b_simple))
	goto end_seq_and_fail;

      if (!noce_emit_bb (emit_a, then_bb, a_simple))
	goto end_seq_and_fail;
    }
  else if (!modified_in_a)
    {
      if (!noce_emit_bb (emit_a, then_bb, a_simple))
	goto end_seq_and_fail;

      if (!noce_emit_bb (emit_b, else_bb, b_simple))
	goto end_seq_and_fail;
    }
  else
    goto end_seq_and_fail;

  target = noce_emit_cmove (if_info, x, code, XEXP (cond, 0), XEXP (cond, 1),
			    a, b);

  if (! target)
    goto end_seq_and_fail;

  /* If we're handling a memory for above, emit the load now.  */
  if (is_mem)
    {
      rtx mem = gen_rtx_MEM (GET_MODE (if_info->x), target);

      /* Copy over flags as appropriate.  */
      if (MEM_VOLATILE_P (if_info->a) || MEM_VOLATILE_P (if_info->b))
	MEM_VOLATILE_P (mem) = 1;
      if (MEM_ALIAS_SET (if_info->a) == MEM_ALIAS_SET (if_info->b))
	set_mem_alias_set (mem, MEM_ALIAS_SET (if_info->a));
      set_mem_align (mem,
		     MIN (MEM_ALIGN (if_info->a), MEM_ALIGN (if_info->b)));

      gcc_assert (MEM_ADDR_SPACE (if_info->a) == MEM_ADDR_SPACE (if_info->b));
      set_mem_addr_space (mem, MEM_ADDR_SPACE (if_info->a));

      noce_emit_move_insn (if_info->x, mem);
    }
  else if (target != x)
    noce_emit_move_insn (x, target);

  ifcvt_seq = end_ifcvt_sequence (if_info);
  if (!ifcvt_seq || !targetm.noce_conversion_profitable_p (ifcvt_seq, if_info))
    return false;

  emit_insn_before_setloc (ifcvt_seq, if_info->jump,
			   INSN_LOCATION (if_info->insn_a));
  if_info->transform_name = "noce_try_cmove_arith";
  return true;

 end_seq_and_fail:
  end_sequence ();
  return false;
}

/* For most cases, the simplified condition we found is the best
   choice, but this is not the case for the min/max/abs transforms.
   For these we wish to know that it is A or B in the condition.  */

static rtx
noce_get_alt_condition (struct noce_if_info *if_info, rtx target,
			rtx_insn **earliest)
{
  rtx cond, set;
  rtx_insn *insn;
  bool reverse;

  /* If target is already mentioned in the known condition, return it.  */
  if (reg_mentioned_p (target, if_info->cond))
    {
      *earliest = if_info->cond_earliest;
      return if_info->cond;
    }

  set = pc_set (if_info->jump);
  cond = XEXP (SET_SRC (set), 0);
  reverse
    = GET_CODE (XEXP (SET_SRC (set), 2)) == LABEL_REF
      && label_ref_label (XEXP (SET_SRC (set), 2)) == JUMP_LABEL (if_info->jump);
  if (if_info->then_else_reversed)
    reverse = !reverse;

  /* If we're looking for a constant, try to make the conditional
     have that constant in it.  There are two reasons why it may
     not have the constant we want:

     1. GCC may have needed to put the constant in a register, because
        the target can't compare directly against that constant.  For
        this case, we look for a SET immediately before the comparison
        that puts a constant in that register.

     2. GCC may have canonicalized the conditional, for example
	replacing "if x < 4" with "if x <= 3".  We can undo that (or
	make equivalent types of changes) to get the constants we need
	if they're off by one in the right direction.  */

  if (CONST_INT_P (target))
    {
      enum rtx_code code = GET_CODE (if_info->cond);
      rtx op_a = XEXP (if_info->cond, 0);
      rtx op_b = XEXP (if_info->cond, 1);
      rtx_insn *prev_insn;

      /* First, look to see if we put a constant in a register.  */
      prev_insn = prev_nonnote_nondebug_insn (if_info->cond_earliest);
      if (prev_insn
	  && BLOCK_FOR_INSN (prev_insn)
	     == BLOCK_FOR_INSN (if_info->cond_earliest)
	  && INSN_P (prev_insn)
	  && GET_CODE (PATTERN (prev_insn)) == SET)
	{
	  rtx src = find_reg_equal_equiv_note (prev_insn);
	  if (!src)
	    src = SET_SRC (PATTERN (prev_insn));
	  if (CONST_INT_P (src))
	    {
	      if (rtx_equal_p (op_a, SET_DEST (PATTERN (prev_insn))))
		op_a = src;
	      else if (rtx_equal_p (op_b, SET_DEST (PATTERN (prev_insn))))
		op_b = src;

	      if (CONST_INT_P (op_a))
		{
		  std::swap (op_a, op_b);
		  code = swap_condition (code);
		}
	    }
	}

      /* Now, look to see if we can get the right constant by
	 adjusting the conditional.  */
      if (CONST_INT_P (op_b))
	{
	  HOST_WIDE_INT desired_val = INTVAL (target);
	  HOST_WIDE_INT actual_val = INTVAL (op_b);

	  switch (code)
	    {
	    case LT:
	      if (desired_val != HOST_WIDE_INT_MAX
		  && actual_val == desired_val + 1)
		{
		  code = LE;
		  op_b = GEN_INT (desired_val);
		}
	      break;
	    case LE:
	      if (desired_val != HOST_WIDE_INT_MIN
		  && actual_val == desired_val - 1)
		{
		  code = LT;
		  op_b = GEN_INT (desired_val);
		}
	      break;
	    case GT:
	      if (desired_val != HOST_WIDE_INT_MIN
		  && actual_val == desired_val - 1)
		{
		  code = GE;
		  op_b = GEN_INT (desired_val);
		}
	      break;
	    case GE:
	      if (desired_val != HOST_WIDE_INT_MAX
		  && actual_val == desired_val + 1)
		{
		  code = GT;
		  op_b = GEN_INT (desired_val);
		}
	      break;
	    default:
	      break;
	    }
	}

      /* If we made any changes, generate a new conditional that is
	 equivalent to what we started with, but has the right
	 constants in it.  */
      if (code != GET_CODE (if_info->cond)
	  || op_a != XEXP (if_info->cond, 0)
	  || op_b != XEXP (if_info->cond, 1))
	{
	  cond = gen_rtx_fmt_ee (code, GET_MODE (cond), op_a, op_b);
	  *earliest = if_info->cond_earliest;
	  return cond;
	}
    }

  cond = canonicalize_condition (if_info->jump, cond, reverse,
				 earliest, target, have_cbranchcc4, true);
  if (! cond || ! reg_mentioned_p (target, cond))
    return NULL;

  /* We almost certainly searched back to a different place.
     Need to re-verify correct lifetimes.  */

  /* X may not be mentioned in the range (cond_earliest, jump].  */
  for (insn = if_info->jump; insn != *earliest; insn = PREV_INSN (insn))
    if (INSN_P (insn) && reg_overlap_mentioned_p (if_info->x, PATTERN (insn)))
      return NULL;

  /* A and B may not be modified in the range [cond_earliest, jump).  */
  for (insn = *earliest; insn != if_info->jump; insn = NEXT_INSN (insn))
    if (INSN_P (insn)
	&& (modified_in_p (if_info->a, insn)
	    || modified_in_p (if_info->b, insn)))
      return NULL;

  return cond;
}

/* Convert "if (a < b) x = a; else x = b;" to "x = min(a, b);", etc.  */

static bool
noce_try_minmax (struct noce_if_info *if_info)
{
  rtx cond, target;
  rtx_insn *earliest, *seq;
  enum rtx_code code, op;
  bool unsignedp;

  if (!noce_simple_bbs (if_info))
    return false;

  /* ??? Reject modes with NaNs or signed zeros since we don't know how
     they will be resolved with an SMIN/SMAX.  It wouldn't be too hard
     to get the target to tell us...  */
  if (HONOR_SIGNED_ZEROS (if_info->x)
      || HONOR_NANS (if_info->x))
    return false;

  cond = noce_get_alt_condition (if_info, if_info->a, &earliest);
  if (!cond)
    return false;

  /* Verify the condition is of the form we expect, and canonicalize
     the comparison code.  */
  code = GET_CODE (cond);
  if (rtx_equal_p (XEXP (cond, 0), if_info->a))
    {
      if (! rtx_equal_p (XEXP (cond, 1), if_info->b))
	return false;
    }
  else if (rtx_equal_p (XEXP (cond, 1), if_info->a))
    {
      if (! rtx_equal_p (XEXP (cond, 0), if_info->b))
	return false;
      code = swap_condition (code);
    }
  else
    return false;

  /* Determine what sort of operation this is.  Note that the code is for
     a taken branch, so the code->operation mapping appears backwards.  */
  switch (code)
    {
    case LT:
    case LE:
    case UNLT:
    case UNLE:
      op = SMAX;
      unsignedp = false;
      break;
    case GT:
    case GE:
    case UNGT:
    case UNGE:
      op = SMIN;
      unsignedp = false;
      break;
    case LTU:
    case LEU:
      op = UMAX;
      unsignedp = true;
      break;
    case GTU:
    case GEU:
      op = UMIN;
      unsignedp = true;
      break;
    default:
      return false;
    }

  start_sequence ();

  target = expand_simple_binop (GET_MODE (if_info->x), op,
				if_info->a, if_info->b,
				if_info->x, unsignedp, OPTAB_WIDEN);
  if (! target)
    {
      end_sequence ();
      return false;
    }
  if (target != if_info->x)
    noce_emit_move_insn (if_info->x, target);

  seq = end_ifcvt_sequence (if_info);
  if (!seq)
    return false;

  emit_insn_before_setloc (seq, if_info->jump, INSN_LOCATION (if_info->insn_a));
  if_info->cond = cond;
  if_info->cond_earliest = earliest;
  if_info->rev_cond = NULL_RTX;
  if_info->transform_name = "noce_try_minmax";

  return true;
}

/* Convert "if (a < 0) x = -a; else x = a;" to "x = abs(a);",
   "if (a < 0) x = ~a; else x = a;" to "x = one_cmpl_abs(a);",
   etc.  */

static bool
noce_try_abs (struct noce_if_info *if_info)
{
  rtx cond, target, a, b, c;
  rtx_insn *earliest, *seq;
  bool negate;
  bool one_cmpl = false;

  if (!noce_simple_bbs (if_info))
    return false;

  /* Reject modes with signed zeros.  */
  if (HONOR_SIGNED_ZEROS (if_info->x))
    return false;

  /* Recognize A and B as constituting an ABS or NABS.  The canonical
     form is a branch around the negation, taken when the object is the
     first operand of a comparison against 0 that evaluates to true.  */
  a = if_info->a;
  b = if_info->b;
  if (GET_CODE (a) == NEG && rtx_equal_p (XEXP (a, 0), b))
    negate = false;
  else if (GET_CODE (b) == NEG && rtx_equal_p (XEXP (b, 0), a))
    {
      std::swap (a, b);
      negate = true;
    }
  else if (GET_CODE (a) == NOT && rtx_equal_p (XEXP (a, 0), b))
    {
      negate = false;
      one_cmpl = true;
    }
  else if (GET_CODE (b) == NOT && rtx_equal_p (XEXP (b, 0), a))
    {
      std::swap (a, b);
      negate = true;
      one_cmpl = true;
    }
  else
    return false;

  cond = noce_get_alt_condition (if_info, b, &earliest);
  if (!cond)
    return false;

  /* Verify the condition is of the form we expect.  */
  if (rtx_equal_p (XEXP (cond, 0), b))
    c = XEXP (cond, 1);
  else if (rtx_equal_p (XEXP (cond, 1), b))
    {
      c = XEXP (cond, 0);
      negate = !negate;
    }
  else
    return false;

  /* Verify that C is zero.  Search one step backward for a
     REG_EQUAL note or a simple source if necessary.  */
  if (REG_P (c))
    {
      rtx set;
      rtx_insn *insn = prev_nonnote_nondebug_insn (earliest);
      if (insn
	  && BLOCK_FOR_INSN (insn) == BLOCK_FOR_INSN (earliest)
	  && (set = single_set (insn))
	  && rtx_equal_p (SET_DEST (set), c))
	{
	  rtx note = find_reg_equal_equiv_note (insn);
	  if (note)
	    c = XEXP (note, 0);
	  else
	    c = SET_SRC (set);
	}
      else
	return false;
    }
  if (MEM_P (c)
      && GET_CODE (XEXP (c, 0)) == SYMBOL_REF
      && CONSTANT_POOL_ADDRESS_P (XEXP (c, 0)))
    c = get_pool_constant (XEXP (c, 0));

  /* Work around funny ideas get_condition has wrt canonicalization.
     Note that these rtx constants are known to be CONST_INT, and
     therefore imply integer comparisons.
     The one_cmpl case is more complicated, as we want to handle
     only x < 0 ? ~x : x or x >= 0 ? x : ~x to one_cmpl_abs (x)
     and x < 0 ? x : ~x or x >= 0 ? ~x : x to ~one_cmpl_abs (x),
     but not other cases (x > -1 is equivalent of x >= 0).  */
  if (c == constm1_rtx && GET_CODE (cond) == GT)
    ;
  else if (c == const1_rtx && GET_CODE (cond) == LT)
    {
      if (one_cmpl)
	return false;
    }
  else if (c == CONST0_RTX (GET_MODE (b)))
    {
      if (one_cmpl
	  && GET_CODE (cond) != GE
	  && GET_CODE (cond) != LT)
	return false;
    }
  else
    return false;

  /* Determine what sort of operation this is.  */
  switch (GET_CODE (cond))
    {
    case LT:
    case LE:
    case UNLT:
    case UNLE:
      negate = !negate;
      break;
    case GT:
    case GE:
    case UNGT:
    case UNGE:
      break;
    default:
      return false;
    }

  start_sequence ();
  if (one_cmpl)
    target = expand_one_cmpl_abs_nojump (GET_MODE (if_info->x), b,
                                         if_info->x);
  else
    target = expand_abs_nojump (GET_MODE (if_info->x), b, if_info->x, 1);

  /* ??? It's a quandary whether cmove would be better here, especially
     for integers.  Perhaps combine will clean things up.  */
  if (target && negate)
    {
      if (one_cmpl)
        target = expand_simple_unop (GET_MODE (target), NOT, target,
                                     if_info->x, 0);
      else
        target = expand_simple_unop (GET_MODE (target), NEG, target,
                                     if_info->x, 0);
    }

  if (! target)
    {
      end_sequence ();
      return false;
    }

  if (target != if_info->x)
    noce_emit_move_insn (if_info->x, target);

  seq = end_ifcvt_sequence (if_info);
  if (!seq)
    return false;

  emit_insn_before_setloc (seq, if_info->jump, INSN_LOCATION (if_info->insn_a));
  if_info->cond = cond;
  if_info->cond_earliest = earliest;
  if_info->rev_cond = NULL_RTX;
  if_info->transform_name = "noce_try_abs";

  return true;
}

/* Convert "if (m < 0) x = b; else x = 0;" to "x = (m >> C) & b;".  */

static bool
noce_try_sign_mask (struct noce_if_info *if_info)
{
  rtx cond, t, m, c;
  rtx_insn *seq;
  machine_mode mode;
  enum rtx_code code;
  bool t_unconditional;

  if (!noce_simple_bbs (if_info))
    return false;

  cond = if_info->cond;
  code = GET_CODE (cond);
  m = XEXP (cond, 0);
  c = XEXP (cond, 1);

  t = NULL_RTX;
  if (if_info->a == const0_rtx)
    {
      if ((code == LT && c == const0_rtx)
	  || (code == LE && c == constm1_rtx))
	t = if_info->b;
    }
  else if (if_info->b == const0_rtx)
    {
      if ((code == GE && c == const0_rtx)
	  || (code == GT && c == constm1_rtx))
	t = if_info->a;
    }

  if (! t || side_effects_p (t))
    return false;

  /* We currently don't handle different modes.  */
  mode = GET_MODE (t);
  if (GET_MODE (m) != mode)
    return false;

  /* This is only profitable if T is unconditionally executed/evaluated in the
     original insn sequence or T is cheap and can't trap or fault.  The former
     happens if B is the non-zero (T) value and if INSN_B was taken from
     TEST_BB, or there was no INSN_B which can happen for e.g. conditional
     stores to memory.  For the cost computation use the block TEST_BB where
     the evaluation will end up after the transformation.  */
  t_unconditional
    = (t == if_info->b
       && (if_info->insn_b == NULL_RTX
	   || BLOCK_FOR_INSN (if_info->insn_b) == if_info->test_bb));
  if (!(t_unconditional
	|| ((set_src_cost (t, mode, if_info->speed_p)
	     < COSTS_N_INSNS (2))
	    && !may_trap_or_fault_p (t))))
    return false;

  if (!noce_can_force_operand (t))
    return false;

  start_sequence ();
  /* Use emit_store_flag to generate "m < 0 ? -1 : 0" instead of expanding
     "(signed) m >> 31" directly.  This benefits targets with specialized
     insns to obtain the signmask, but still uses ashr_optab otherwise.  */
  m = emit_store_flag (gen_reg_rtx (mode), LT, m, const0_rtx, mode, 0, -1);
  t = m ? expand_binop (mode, and_optab, m, t, NULL_RTX, 0, OPTAB_DIRECT)
	: NULL_RTX;

  if (!t)
    {
      end_sequence ();
      return false;
    }

  noce_emit_move_insn (if_info->x, t);

  seq = end_ifcvt_sequence (if_info);
  if (!seq)
    return false;

  emit_insn_before_setloc (seq, if_info->jump, INSN_LOCATION (if_info->insn_a));
  if_info->transform_name = "noce_try_sign_mask";

  return true;
}

/*  Check if OP is supported by conditional zero based if conversion,
    returning TRUE if satisfied otherwise FALSE.

    OP is the operation to check.  */

static bool
noce_cond_zero_binary_op_supported (rtx op)
{
  enum rtx_code opcode = GET_CODE (op);

  if (opcode == PLUS || opcode == MINUS || opcode == IOR || opcode == XOR
      || opcode == ASHIFT || opcode == ASHIFTRT || opcode == LSHIFTRT
      || opcode == ROTATE || opcode == ROTATERT || opcode == AND)
    return true;

  return false;
}

/*  Helper function to return REG itself,
    otherwise NULL_RTX for other RTX_CODE.  */

static rtx
get_base_reg (rtx exp)
{
  if (REG_P (exp))
    return exp;

  return NULL_RTX;
}

/*  Check if IF-BB and THEN-BB satisfy the condition for conditional zero
    based if conversion, returning TRUE if satisfied otherwise FALSE.

    IF_INFO describes the if-conversion scenario under consideration.
    COMMON_PTR points to the common REG of canonicalized IF_INFO->A and
    IF_INFO->B.
    CZERO_CODE_PTR points to the comparison code to use in czero RTX.
    A_PTR points to the A expression of canonicalized IF_INFO->A.
    TO_REPLACE points to the RTX to be replaced by czero RTX destnation.  */

static bool
noce_bbs_ok_for_cond_zero_arith (struct noce_if_info *if_info, rtx *common_ptr,
				 rtx *bin_exp_ptr,
				 enum rtx_code *czero_code_ptr, rtx *a_ptr,
				 rtx **to_replace)
{
  rtx common = NULL_RTX;
  rtx cond = if_info->cond;
  rtx a = copy_rtx (if_info->a);
  rtx b = copy_rtx (if_info->b);
  rtx bin_op1 = NULL_RTX;
  enum rtx_code czero_code = UNKNOWN;
  bool reverse = false;
  rtx op0, op1, bin_exp;

  if (!noce_simple_bbs (if_info))
    return false;

  /* COND must be EQ or NE comparision of a reg and 0.  */
  if (GET_CODE (cond) != NE && GET_CODE (cond) != EQ)
    return false;
  if (!REG_P (XEXP (cond, 0)) || !rtx_equal_p (XEXP (cond, 1), const0_rtx))
    return false;

  /* Canonicalize x = y : (y op z) to x = (y op z) : y.  */
  if (REG_P (a) && noce_cond_zero_binary_op_supported (b))
    {
      std::swap (a, b);
      reverse = !reverse;
    }

  /* Check if x = (y op z) : y is supported by czero based ifcvt.  */
  if (!(noce_cond_zero_binary_op_supported (a) && REG_P (b)))
    return false;

  bin_exp = a;

  /* Canonicalize x = (z op y) : y to x = (y op z) : y */
  op1 = get_base_reg (XEXP (bin_exp, 1));
  if (op1 && rtx_equal_p (op1, b) && COMMUTATIVE_ARITH_P (bin_exp))
    std::swap (XEXP (bin_exp, 0), XEXP (bin_exp, 1));

  op0 = get_base_reg (XEXP (bin_exp, 0));
  if (op0 && rtx_equal_p (op0, b))
    {
      common = b;
      bin_op1 = XEXP (bin_exp, 1);
      czero_code = (reverse ^ (GET_CODE (bin_exp) == AND))
		     ? noce_reversed_cond_code (if_info)
		     : GET_CODE (cond);
    }
  else
    return false;

  if (czero_code == UNKNOWN)
    return false;

  if (REG_P (bin_op1))
    *to_replace = &XEXP (bin_exp, 1);
  else
    return false;

  *common_ptr = common;
  *bin_exp_ptr = bin_exp;
  *czero_code_ptr = czero_code;
  *a_ptr = a;

  return true;
}

/*  Try to covert if-then-else with conditional zero,
    returning TURE on success or FALSE on failure.
    IF_INFO describes the if-conversion scenario under consideration.  */

static int
noce_try_cond_zero_arith (struct noce_if_info *if_info)
{
  rtx target, rtmp, a;
  rtx_insn *seq;
  machine_mode mode = GET_MODE (if_info->x);
  rtx common = NULL_RTX;
  enum rtx_code czero_code = UNKNOWN;
  rtx bin_exp = NULL_RTX;
  enum rtx_code bin_code = UNKNOWN;
  rtx non_zero_op = NULL_RTX;
  rtx *to_replace = NULL;

  if (!noce_bbs_ok_for_cond_zero_arith (if_info, &common, &bin_exp, &czero_code,
					&a, &to_replace))
    return false;

  start_sequence ();

  bin_code = GET_CODE (bin_exp);

  if (bin_code == AND)
    {
      rtmp = gen_reg_rtx (mode);
      noce_emit_move_insn (rtmp, a);

      target = noce_emit_czero (if_info, czero_code, common, if_info->x);
      if (!target)
	{
	  end_sequence ();
	  return false;
	}

      target = expand_simple_binop (mode, IOR, rtmp, target, if_info->x, 0,
				    OPTAB_WIDEN);
      if (!target)
	{
	  end_sequence ();
	  return false;
	}

      if (target != if_info->x)
	noce_emit_move_insn (if_info->x, target);
    }
  else
    {
      non_zero_op = *to_replace;
      /* If x is used in both input and out like x = c ? x + z : x,
	 use a new reg to avoid modifying x  */
      if (common && rtx_equal_p (common, if_info->x))
	target = gen_reg_rtx (mode);
      else
	target = if_info->x;

      target = noce_emit_czero (if_info, czero_code, non_zero_op, target);
      if (!target || !to_replace)
	{
	  end_sequence ();
	  return false;
	}

      *to_replace = target;
      noce_emit_move_insn (if_info->x, a);
    }

  seq = end_ifcvt_sequence (if_info);
  if (!seq || !targetm.noce_conversion_profitable_p (seq, if_info))
    return false;

  emit_insn_before_setloc (seq, if_info->jump, INSN_LOCATION (if_info->insn_a));
  if_info->transform_name = "noce_try_cond_zero_arith";
  return true;
}

/* Optimize away "if (x & C) x |= C" and similar bit manipulation
   transformations.  */

static bool
noce_try_bitop (struct noce_if_info *if_info)
{
  rtx cond, x, a, result;
  rtx_insn *seq;
  scalar_int_mode mode;
  enum rtx_code code;
  int bitnum;

  x = if_info->x;
  cond = if_info->cond;
  code = GET_CODE (cond);

  /* Check for an integer operation.  */
  if (!is_a <scalar_int_mode> (GET_MODE (x), &mode))
    return false;

  if (!noce_simple_bbs (if_info))
    return false;

  /* Check for no else condition.  */
  if (! rtx_equal_p (x, if_info->b))
    return false;

  /* Check for a suitable condition.  */
  if (code != NE && code != EQ)
    return false;
  if (XEXP (cond, 1) != const0_rtx)
    return false;
  cond = XEXP (cond, 0);

  /* ??? We could also handle AND here.  */
  if (GET_CODE (cond) == ZERO_EXTRACT)
    {
      if (XEXP (cond, 1) != const1_rtx
	  || !CONST_INT_P (XEXP (cond, 2))
	  || ! rtx_equal_p (x, XEXP (cond, 0)))
	return false;
      bitnum = INTVAL (XEXP (cond, 2));
      if (BITS_BIG_ENDIAN)
	bitnum = GET_MODE_BITSIZE (mode) - 1 - bitnum;
      if (bitnum < 0 || bitnum >= HOST_BITS_PER_WIDE_INT)
	return false;
    }
  else
    return false;

  a = if_info->a;
  if (GET_CODE (a) == IOR || GET_CODE (a) == XOR)
    {
      /* Check for "if (X & C) x = x op C".  */
      if (! rtx_equal_p (x, XEXP (a, 0))
          || !CONST_INT_P (XEXP (a, 1))
	  || (INTVAL (XEXP (a, 1)) & GET_MODE_MASK (mode))
	     != HOST_WIDE_INT_1U << bitnum)
	return false;

      /* if ((x & C) == 0) x |= C; is transformed to x |= C.   */
      /* if ((x & C) != 0) x |= C; is transformed to nothing.  */
      if (GET_CODE (a) == IOR)
	result = (code == NE) ? a : NULL_RTX;
      else if (code == NE)
	{
	  /* if ((x & C) == 0) x ^= C; is transformed to x |= C.   */
	  result = gen_int_mode (HOST_WIDE_INT_1 << bitnum, mode);
	  result = simplify_gen_binary (IOR, mode, x, result);
	}
      else
	{
	  /* if ((x & C) != 0) x ^= C; is transformed to x &= ~C.  */
	  result = gen_int_mode (~(HOST_WIDE_INT_1 << bitnum), mode);
	  result = simplify_gen_binary (AND, mode, x, result);
	}
    }
  else if (GET_CODE (a) == AND)
    {
      /* Check for "if (X & C) x &= ~C".  */
      if (! rtx_equal_p (x, XEXP (a, 0))
	  || !CONST_INT_P (XEXP (a, 1))
	  || (INTVAL (XEXP (a, 1)) & GET_MODE_MASK (mode))
	     != (~(HOST_WIDE_INT_1 << bitnum) & GET_MODE_MASK (mode)))
	return false;

      /* if ((x & C) == 0) x &= ~C; is transformed to nothing.  */
      /* if ((x & C) != 0) x &= ~C; is transformed to x &= ~C.  */
      result = (code == EQ) ? a : NULL_RTX;
    }
  else
    return false;

  if (result)
    {
      start_sequence ();
      noce_emit_move_insn (x, result);
      seq = end_ifcvt_sequence (if_info);
      if (!seq)
	return false;

      emit_insn_before_setloc (seq, if_info->jump,
			       INSN_LOCATION (if_info->insn_a));
    }
  if_info->transform_name = "noce_try_bitop";
  return true;
}


/* Similar to get_condition, only the resulting condition must be
   valid at JUMP, instead of at EARLIEST.

   If THEN_ELSE_REVERSED is true, the fallthrough does not go to the
   THEN block of the caller, and we have to reverse the condition.  */

static rtx
noce_get_condition (rtx_insn *jump, rtx_insn **earliest,
		    bool then_else_reversed)
{
  rtx cond, set, tmp;
  bool reverse;

  if (! any_condjump_p (jump))
    return NULL_RTX;

  set = pc_set (jump);

  /* If this branches to JUMP_LABEL when the condition is false,
     reverse the condition.  */
  reverse = (GET_CODE (XEXP (SET_SRC (set), 2)) == LABEL_REF
	     && label_ref_label (XEXP (SET_SRC (set), 2)) == JUMP_LABEL (jump));

  /* We may have to reverse because the caller's if block is not canonical,
     i.e. the THEN block isn't the fallthrough block for the TEST block
     (see find_if_header).  */
  if (then_else_reversed)
    reverse = !reverse;

  /* If the condition variable is a register and is MODE_INT, accept it.  */

  cond = XEXP (SET_SRC (set), 0);
  tmp = XEXP (cond, 0);
  if (REG_P (tmp) && GET_MODE_CLASS (GET_MODE (tmp)) == MODE_INT
      && (GET_MODE (tmp) != BImode
          || !targetm.small_register_classes_for_mode_p (BImode)))
    {
      *earliest = jump;

      if (reverse)
	cond = gen_rtx_fmt_ee (reverse_condition (GET_CODE (cond)),
			       GET_MODE (cond), tmp, XEXP (cond, 1));
      return cond;
    }

  /* Otherwise, fall back on canonicalize_condition to do the dirty
     work of manipulating MODE_CC values and COMPARE rtx codes.  */
  tmp = canonicalize_condition (jump, cond, reverse, earliest,
				NULL_RTX, have_cbranchcc4, true);

  /* We don't handle side-effects in the condition, like handling
     REG_INC notes and making sure no duplicate conditions are emitted.  */
  if (tmp != NULL_RTX && side_effects_p (tmp))
    return NULL_RTX;

  return tmp;
}

/* Return true if OP is ok for if-then-else processing.  */

static bool
noce_operand_ok (const_rtx op)
{
  if (side_effects_p (op))
    return false;

  /* We special-case memories, so handle any of them with
     no address side effects.  */
  if (MEM_P (op))
    return ! side_effects_p (XEXP (op, 0));

  return ! may_trap_p (op);
}

/* Return true iff basic block TEST_BB is valid for noce if-conversion.
   The condition used in this if-conversion is in COND.
   In practice, check that TEST_BB ends with a single set
   x := a and all previous computations
   in TEST_BB don't produce any values that are live after TEST_BB.
   In other words, all the insns in TEST_BB are there only
   to compute a value for x.  Add the rtx cost of the insns
   in TEST_BB to COST.  Record whether TEST_BB is a single simple
   set instruction in SIMPLE_P.  */

static bool
bb_valid_for_noce_process_p (basic_block test_bb, rtx cond,
			      unsigned int *cost, bool *simple_p)
{
  if (!test_bb)
    return false;

  rtx_insn *last_insn = last_active_insn (test_bb, false);
  rtx last_set = NULL_RTX;

  rtx cc = cc_in_cond (cond);

  if (!insn_valid_noce_process_p (last_insn, cc))
    return false;

  /* Punt on blocks ending with asm goto or jumps with other side-effects,
     last_active_insn ignores JUMP_INSNs.  */
  if (JUMP_P (BB_END (test_bb)) && !onlyjump_p (BB_END (test_bb)))
    return false;

  last_set = single_set (last_insn);

  rtx x = SET_DEST (last_set);
  rtx_insn *first_insn = first_active_insn (test_bb);
  rtx first_set = single_set (first_insn);

  if (!first_set)
    return false;

  /* We have a single simple set, that's okay.  */
  bool speed_p = optimize_bb_for_speed_p (test_bb);

  if (first_insn == last_insn)
    {
      *simple_p = noce_operand_ok (SET_DEST (first_set));
      *cost += pattern_cost (first_set, speed_p);
      return *simple_p;
    }

  rtx_insn *prev_last_insn = PREV_INSN (last_insn);
  gcc_assert (prev_last_insn);

  /* For now, disallow setting x multiple times in test_bb.  */
  if (REG_P (x) && reg_set_between_p (x, first_insn, prev_last_insn))
    return false;

  bitmap test_bb_temps = BITMAP_ALLOC (&reg_obstack);

  /* The regs that are live out of test_bb.  */
  bitmap test_bb_live_out = df_get_live_out (test_bb);

  int potential_cost = pattern_cost (last_set, speed_p);
  rtx_insn *insn;
  FOR_BB_INSNS (test_bb, insn)
    {
      if (insn != last_insn)
	{
	  if (!active_insn_p (insn))
	    continue;

	  if (!insn_valid_noce_process_p (insn, cc))
	    goto free_bitmap_and_fail;

	  rtx sset = single_set (insn);
	  gcc_assert (sset);
	  rtx dest = SET_DEST (sset);
	  if (SUBREG_P (dest))
	    dest = SUBREG_REG (dest);

	  if (contains_mem_rtx_p (SET_SRC (sset))
	      || !REG_P (dest)
	      || reg_overlap_mentioned_p (dest, cond))
	    goto free_bitmap_and_fail;

	  potential_cost += pattern_cost (sset, speed_p);
	  bitmap_set_bit (test_bb_temps, REGNO (dest));
	}
    }

  /* If any of the intermediate results in test_bb are live after test_bb
     then fail.  */
  if (bitmap_intersect_p (test_bb_live_out, test_bb_temps))
    goto free_bitmap_and_fail;

  BITMAP_FREE (test_bb_temps);
  *cost += potential_cost;
  *simple_p = false;
  return true;

 free_bitmap_and_fail:
  BITMAP_FREE (test_bb_temps);
  return false;
}

/* Helper function to emit a cmov sequence encapsulated in
   start_sequence () and end_sequence ().  If NEED_CMOV is true
   we call noce_emit_cmove to create a cmove sequence.  Otherwise emit
   a simple move.  If successful, store the first instruction of the
   sequence in TEMP_DEST and the sequence costs in SEQ_COST.  */

static rtx_insn*
try_emit_cmove_seq (struct noce_if_info *if_info, rtx temp,
		    rtx cond, rtx new_val, rtx old_val, bool need_cmov,
		    unsigned *cost, rtx *temp_dest,
		    rtx cc_cmp = NULL, rtx rev_cc_cmp = NULL)
{
  rtx_insn *seq = NULL;
  *cost = 0;

  rtx x = XEXP (cond, 0);
  rtx y = XEXP (cond, 1);
  rtx_code cond_code = GET_CODE (cond);

  start_sequence ();

  if (need_cmov)
    *temp_dest = noce_emit_cmove (if_info, temp, cond_code,
				  x, y, new_val, old_val, cc_cmp, rev_cc_cmp);
  else
    {
      *temp_dest = temp;
      if (if_info->then_else_reversed)
	noce_emit_move_insn (temp, old_val);
      else
	noce_emit_move_insn (temp, new_val);
    }

  if (*temp_dest != NULL_RTX)
    {
      seq = get_insns ();
      *cost = seq_cost (seq, if_info->speed_p);
    }

  end_sequence ();

  return seq;
}

/* We have something like:

     if (x > y)
       { i = EXPR_A; j = EXPR_B; k = EXPR_C; }

   Make it:

     tmp_i = (x > y) ? EXPR_A : i;
     tmp_j = (x > y) ? EXPR_B : j;
     tmp_k = (x > y) ? EXPR_C : k;
     i = tmp_i;
     j = tmp_j;
     k = tmp_k;

   Subsequent passes are expected to clean up the extra moves.

   Look for special cases such as writes to one register which are
   read back in another SET, as might occur in a swap idiom or
   similar.

   These look like:

   if (x > y)
     i = a;
     j = i;

   Which we want to rewrite to:

     tmp_i = (x > y) ? a : i;
     tmp_j = (x > y) ? tmp_i : j;
     i = tmp_i;
     j = tmp_j;

   We can catch these when looking at (SET x y) by keeping a list of the
   registers we would have targeted before if-conversion and looking back
   through it for an overlap with Y.  If we find one, we rewire the
   conditional set to use the temporary we introduced earlier.

   IF_INFO contains the useful information about the block structure and
   jump instructions.  */

static bool
noce_convert_multiple_sets (struct noce_if_info *if_info)
{
  basic_block test_bb = if_info->test_bb;
  basic_block then_bb = if_info->then_bb;
  basic_block join_bb = if_info->join_bb;
  rtx_insn *jump = if_info->jump;
  rtx_insn *cond_earliest;
  rtx_insn *insn;

  start_sequence ();

  /* Decompose the condition attached to the jump.  */
  rtx cond = noce_get_condition (jump, &cond_earliest, false);
  rtx x = XEXP (cond, 0);
  rtx y = XEXP (cond, 1);

  auto_delete_vec<noce_multiple_sets_info> insn_info;
  init_noce_multiple_sets_info (then_bb, insn_info);

  int last_needs_comparison = -1;

  bool ok = noce_convert_multiple_sets_1
    (if_info, insn_info, &last_needs_comparison);
  if (!ok)
      return false;

  /* If there are insns that overwrite part of the initial
     comparison, we can still omit creating temporaries for
     the last of them.
     As the second try will always create a less expensive,
     valid sequence, we do not need to compare and can discard
     the first one.  */
  if (last_needs_comparison != -1)
    {
      end_sequence ();
      start_sequence ();
      ok = noce_convert_multiple_sets_1
	(if_info, insn_info, &last_needs_comparison);
      /* Actually we should not fail anymore if we reached here,
	 but better still check.  */
      if (!ok)
	  return false;
    }

  /* We must have seen some sort of insn to insert, otherwise we were
     given an empty BB to convert, and we can't handle that.  */
  gcc_assert (!insn_info.is_empty ());

  /* Now fixup the assignments.
     PR116405: Iterate in reverse order and keep track of the targets so that
     a move does not overwrite a subsequent value when multiple instructions
     have the same target.  */
  unsigned i;
  noce_multiple_sets_info *info;
  bitmap set_targets = BITMAP_ALLOC (&reg_obstack);
  FOR_EACH_VEC_ELT_REVERSE (insn_info, i, info)
    {
      gcc_checking_assert (REG_P (info->target));

      if (info->target != info->temporary
	  && !bitmap_bit_p (set_targets, REGNO (info->target)))
	noce_emit_move_insn (info->target, info->temporary);

      bitmap_set_bit (set_targets, REGNO (info->target));
    }
  BITMAP_FREE (set_targets);

  /* Actually emit the sequence if it isn't too expensive.  */
  rtx_insn *seq = get_insns ();

  if (!targetm.noce_conversion_profitable_p (seq, if_info))
    {
      end_sequence ();
      return false;
    }

  for (insn = seq; insn; insn = NEXT_INSN (insn))
    set_used_flags (insn);

  /* Mark all our temporaries and targets as used.  */
  for (unsigned i = 0; i < insn_info.length (); i++)
    {
      set_used_flags (insn_info[i]->temporary);
      set_used_flags (insn_info[i]->target);
    }

  set_used_flags (cond);
  set_used_flags (x);
  set_used_flags (y);

  unshare_all_rtl_in_chain (seq);
  end_sequence ();

  if (!seq)
    return false;

  for (insn = seq; insn; insn = NEXT_INSN (insn))
    if (JUMP_P (insn) || CALL_P (insn)
	|| recog_memoized (insn) == -1)
      return false;

  emit_insn_before_setloc (seq, if_info->jump,
			   INSN_LOCATION (insn_info.last ()->unmodified_insn));

  /* Clean up THEN_BB and the edges in and out of it.  */
  remove_edge (find_edge (test_bb, join_bb));
  remove_edge (find_edge (then_bb, join_bb));
  redirect_edge_and_branch_force (single_succ_edge (test_bb), join_bb);
  delete_basic_block (then_bb);
  num_true_changes++;

  /* Maybe merge blocks now the jump is simple enough.  */
  if (can_merge_blocks_p (test_bb, join_bb))
    {
      merge_blocks (test_bb, join_bb);
      num_true_changes++;
    }

  num_updated_if_blocks++;
  if_info->transform_name = "noce_convert_multiple_sets";
  return true;
}

/* This goes through all relevant insns of IF_INFO->then_bb and tries to create
   conditional moves.  Information for the insns is kept in INSN_INFO.  */

static bool
noce_convert_multiple_sets_1 (struct noce_if_info *if_info,
			      auto_delete_vec<noce_multiple_sets_info> &insn_info,
			      int *last_needs_comparison)
{
  basic_block then_bb = if_info->then_bb;
  rtx_insn *jump = if_info->jump;
  rtx_insn *cond_earliest;

  /* Decompose the condition attached to the jump.  */
  rtx cond = noce_get_condition (jump, &cond_earliest, false);

  rtx cc_cmp = cond_exec_get_condition (jump);
  if (cc_cmp)
    cc_cmp = copy_rtx (cc_cmp);
  rtx rev_cc_cmp = cond_exec_get_condition (jump, /* get_reversed */ true);
  if (rev_cc_cmp)
    rev_cc_cmp = copy_rtx (rev_cc_cmp);

  rtx_insn *insn;
  int count = 0;
  bool second_try = *last_needs_comparison != -1;

  FOR_BB_INSNS (then_bb, insn)
    {
      /* Skip over non-insns.  */
      if (!active_insn_p (insn))
	continue;

      noce_multiple_sets_info *info = insn_info[count];

      rtx set = single_set (insn);
      gcc_checking_assert (set);

      rtx target = SET_DEST (set);
      rtx temp;

      rtx new_val = SET_SRC (set);

      int i, ii;
      FOR_EACH_VEC_ELT (info->rewired_src, i, ii)
	new_val = simplify_replace_rtx (new_val, insn_info[ii]->target,
					insn_info[ii]->temporary);

      rtx old_val = target;

      /* As we are transforming
	 if (x > y)
	   {
	     a = b;
	     c = d;
	   }
	 into
	   a = (x > y) ...
	   c = (x > y) ...

	 we potentially check x > y before every set.
	 Even though the check might be removed by subsequent passes, this means
	 that we cannot transform
	   if (x > y)
	     {
	       x = y;
	       ...
	     }
	 into
	   x = (x > y) ...
	   ...
	 since this would invalidate x and the following to-be-removed checks.
	 Therefore we introduce a temporary every time we are about to
	 overwrite a variable used in the check.  Costing of a sequence with
	 these is going to be inaccurate so only use temporaries when
	 needed.

	 If performing a second try, we know how many insns require a
	 temporary.  For the last of these, we can omit creating one.  */
      if (reg_overlap_mentioned_p (target, cond)
	  && (!second_try || count < *last_needs_comparison))
	temp = gen_reg_rtx (GET_MODE (target));
      else
	temp = target;

      /* We have identified swap-style idioms before.  A normal
	 set will need to be a cmov while the first instruction of a swap-style
	 idiom can be a regular move.  This helps with costing.  */
      bool need_cmov = info->need_cmov;

      /* If we had a non-canonical conditional jump (i.e. one where
	 the fallthrough is to the "else" case) we need to reverse
	 the conditional select.  */
      if (if_info->then_else_reversed)
	std::swap (old_val, new_val);

      /* Try emitting a conditional move passing the backend the
	 canonicalized comparison.  The backend is then able to
	 recognize expressions like

	   if (x > y)
	     y = x;

	 as min/max and emit an insn, accordingly.  */
      unsigned cost1 = 0, cost2 = 0;
      rtx_insn *seq, *seq1, *seq2 = NULL;
      rtx temp_dest = NULL_RTX, temp_dest1 = NULL_RTX, temp_dest2 = NULL_RTX;
      bool read_comparison = false;

      seq1 = try_emit_cmove_seq (if_info, temp, cond,
				 new_val, old_val, need_cmov,
				 &cost1, &temp_dest1);

      /* Here, we try to pass the backend a non-canonicalized cc comparison
	 as well.  This allows the backend to emit a cmov directly without
	 creating an additional compare for each.  If successful, costing
	 is easier and this sequence is usually preferred.  */
      if (cc_cmp)
	{
	  seq2 = try_emit_cmove_seq (if_info, temp, cond,
				     new_val, old_val, need_cmov,
				     &cost2, &temp_dest2, cc_cmp, rev_cc_cmp);

	  /* The if_then_else in SEQ2 may be affected when cc_cmp/rev_cc_cmp is
	     clobbered.  We can't safely use the sequence in this case.  */
	  for (rtx_insn *iter = seq2; iter; iter = NEXT_INSN (iter))
	    if (modified_in_p (cc_cmp, iter)
	      || (rev_cc_cmp && modified_in_p (rev_cc_cmp, iter)))
	      {
		seq2 = NULL;
		break;
	      }
	}

      /* The backend might have created a sequence that uses the
	 condition as a value.  Check this.  */

      /* We cannot handle anything more complex than a reg or constant.  */
      if (!REG_P (XEXP (cond, 0)) && !CONSTANT_P (XEXP (cond, 0)))
	read_comparison = true;

      if (!REG_P (XEXP (cond, 1)) && !CONSTANT_P (XEXP (cond, 1)))
	read_comparison = true;

      rtx_insn *walk = seq2;
      int if_then_else_count = 0;
      while (walk && !read_comparison)
	{
	  rtx exprs_to_check[2];
	  unsigned int exprs_count = 0;

	  rtx set = single_set (walk);
	  if (set && XEXP (set, 1)
	      && GET_CODE (XEXP (set, 1)) == IF_THEN_ELSE)
	    {
	      /* We assume that this is the cmove created by the backend that
		 naturally uses the condition.  */
	      exprs_to_check[exprs_count++] = XEXP (XEXP (set, 1), 1);
	      exprs_to_check[exprs_count++] = XEXP (XEXP (set, 1), 2);
	      if_then_else_count++;
	    }
	  else if (NONDEBUG_INSN_P (walk))
	    exprs_to_check[exprs_count++] = PATTERN (walk);

	  /* Bail if we get more than one if_then_else because the assumption
	     above may be incorrect.  */
	  if (if_then_else_count > 1)
	    {
	      read_comparison = true;
	      break;
	    }

	  for (unsigned int i = 0; i < exprs_count; i++)
	    {
	      subrtx_iterator::array_type array;
	      FOR_EACH_SUBRTX (iter, array, exprs_to_check[i], NONCONST)
		if (*iter != NULL_RTX
		    && (reg_overlap_mentioned_p (XEXP (cond, 0), *iter)
		    || reg_overlap_mentioned_p (XEXP (cond, 1), *iter)))
		  {
		    read_comparison = true;
		    break;
		  }
	    }

	  walk = NEXT_INSN (walk);
	}

      /* Check which version is less expensive.  */
      if (seq1 != NULL_RTX && (cost1 <= cost2 || seq2 == NULL_RTX))
	{
	  seq = seq1;
	  temp_dest = temp_dest1;
	  if (!second_try)
	    *last_needs_comparison = count;
	}
      else if (seq2 != NULL_RTX)
	{
	  seq = seq2;
	  temp_dest = temp_dest2;
	  if (!second_try && read_comparison)
	    *last_needs_comparison = count;
	}
      else
	{
	  /* Nothing worked, bail out.  */
	  end_sequence ();
	  return false;
	}

      /* Although we use temporaries if there is register overlap of COND and
	 TARGET, it is possible that SEQ modifies COND anyway.  For example,
	 COND may use the flags register and if INSN clobbers flags then
	 we may be unable to emit a valid sequence (e.g. in x86 that would
	 require saving and restoring the flags register).  */
      if (!second_try)
	for (rtx_insn *iter = seq; iter; iter = NEXT_INSN (iter))
	  if (modified_in_p (cond, iter))
	    {
	      end_sequence ();
	      return false;
	    }

      if (cc_cmp && seq == seq1)
	{
	  /* Check if SEQ can clobber registers mentioned in cc_cmp/rev_cc_cmp.
	     If yes, we need to use only SEQ1 from that point on.
	     Only check when we use SEQ1 since we have already tested SEQ2.  */
	  for (rtx_insn *iter = seq; iter; iter = NEXT_INSN (iter))
	    if (modified_in_p (cc_cmp, iter)
	      || (rev_cc_cmp && modified_in_p (rev_cc_cmp, iter)))
	      {
		cc_cmp = NULL_RTX;
		rev_cc_cmp = NULL_RTX;
		break;
	      }
	}

      /* End the sub sequence and emit to the main sequence.  */
      emit_insn (seq);

      /* Bookkeeping.  */
      count++;

      info->target = target;
      info->temporary = temp_dest;
      info->unmodified_insn = insn;
    }

  /* Even if we did not actually need the comparison, we want to make sure
     to try a second time in order to get rid of the temporaries.  */
  if (*last_needs_comparison == -1)
    *last_needs_comparison = 0;

  return true;
}

/* Find local swap-style idioms in BB and mark the first insn (1)
   that is only a temporary as not needing a conditional move as
   it is going to be dead afterwards anyway.

     (1) int tmp = a;
	 a = b;
	 b = tmp;

	 ifcvt
	 -->

	 tmp = a;
	 a = cond ? b : a_old;
	 b = cond ? tmp : b_old;

    Additionally, store the index of insns like (2) when a subsequent
    SET reads from their destination.

    (2) int c = a;
	int d = c;

	ifcvt
	-->

	c = cond ? a : c_old;
	d = cond ? d : c;     // Need to use c rather than c_old here.
*/

static void
init_noce_multiple_sets_info (basic_block bb,
		     auto_delete_vec<noce_multiple_sets_info> &insn_info)
{
  rtx_insn *insn;
  int count = 0;
  auto_vec<rtx> dests;
  bitmap bb_live_out = df_get_live_out (bb);

  /* Iterate over all SETs, storing the destinations in DEST.
     - If we encounter a previously changed register,
       rewire the read to the original source.
     - If we encounter a SET that writes to a destination
	that is not live after this block then the register
	does not need to be moved conditionally.  */
  FOR_BB_INSNS (bb, insn)
    {
      if (!active_insn_p (insn))
	continue;

      noce_multiple_sets_info *info = new noce_multiple_sets_info;
      info->target = NULL_RTX;
      info->temporary = NULL_RTX;
      info->unmodified_insn = NULL;
      insn_info.safe_push (info);

      rtx set = single_set (insn);
      gcc_checking_assert (set);

      rtx src = SET_SRC (set);
      rtx dest = SET_DEST (set);

      gcc_checking_assert (REG_P (dest));
      info->need_cmov = bitmap_bit_p (bb_live_out, REGNO (dest));

      /* Check if the current SET's source is the same
	 as any previously seen destination.
	 This is quadratic but the number of insns in BB
	 is bounded by PARAM_MAX_RTL_IF_CONVERSION_INSNS.  */
      for (int i = count - 1; i >= 0; --i)
	if (reg_mentioned_p (dests[i], src))
	  insn_info[count]->rewired_src.safe_push (i);

      dests.safe_push (dest);
      count++;
    }
}

/* Return true iff basic block TEST_BB is suitable for conversion to a
   series of conditional moves.  Also check that we have more than one
   set (other routines can handle a single set better than we would),
   and fewer than PARAM_MAX_RTL_IF_CONVERSION_INSNS sets.  While going
   through the insns store the sum of their potential costs in COST.  */

static bool
bb_ok_for_noce_convert_multiple_sets (basic_block test_bb, unsigned *cost)
{
  rtx_insn *insn;
  unsigned count = 0;
  unsigned param = param_max_rtl_if_conversion_insns;
  bool speed_p = optimize_bb_for_speed_p (test_bb);
  unsigned potential_cost = 0;

  FOR_BB_INSNS (test_bb, insn)
    {
      /* Skip over notes etc.  */
      if (!active_insn_p (insn))
	continue;

      /* We only handle SET insns.  */
      rtx set = single_set (insn);
      if (set == NULL_RTX)
	return false;

      rtx dest = SET_DEST (set);
      rtx src = SET_SRC (set);

      /* Do not handle anything involving memory loads/stores since it might
	 violate data-race-freedom guarantees.  Make sure we can force SRC
	 to a register as that may be needed in try_emit_cmove_seq.  */
      if (!REG_P (dest) || contains_mem_rtx_p (src)
	  || !noce_can_force_operand (src))
	return false;

      /* Destination and source must be appropriate.  */
      if (!noce_operand_ok (dest) || !noce_operand_ok (src))
	return false;

      /* We must be able to conditionally move in this mode.  */
      if (!can_conditionally_move_p (GET_MODE (dest)))
	return false;

      potential_cost += insn_cost (insn, speed_p);

      count++;
    }

  *cost += potential_cost;

  /* If we would only put out one conditional move, the other strategies
     this pass tries are better optimized and will be more appropriate.
     Some targets want to strictly limit the number of conditional moves
     that are emitted, they set this through PARAM, we need to respect
     that.  */
  return count > 1 && count <= param;
}

/* Compute average of two given costs weighted by relative probabilities
   of respective basic blocks in an IF-THEN-ELSE.  E is the IF-THEN edge.
   With P as the probability to take the IF-THEN branch, return
   P * THEN_COST + (1 - P) * ELSE_COST.  */
static unsigned
average_cost (unsigned then_cost, unsigned else_cost, edge e)
{
  return else_cost + e->probability.apply ((signed) (then_cost - else_cost));
}

/* Given a simple IF-THEN-JOIN or IF-THEN-ELSE-JOIN block, attempt to convert
   it without using conditional execution.  Return TRUE if we were successful
   at converting the block.  */

static bool
noce_process_if_block (struct noce_if_info *if_info)
{
  basic_block test_bb = if_info->test_bb;	/* test block */
  basic_block then_bb = if_info->then_bb;	/* THEN */
  basic_block else_bb = if_info->else_bb;	/* ELSE or NULL */
  basic_block join_bb = if_info->join_bb;	/* JOIN */
  rtx_insn *jump = if_info->jump;
  rtx cond = if_info->cond;
  rtx_insn *insn_a, *insn_b;
  rtx set_a, set_b;
  rtx orig_x, x, a, b;

  /* We're looking for patterns of the form

     (1) if (...) x = a; else x = b;
     (2) x = b; if (...) x = a;
     (3) if (...) x = a;   // as if with an initial x = x.
     (4) if (...) { x = a; y = b; z = c; }  // Like 3, for multiple SETS.
     The later patterns require jumps to be more expensive.
     For the if (...) x = a; else x = b; case we allow multiple insns
     inside the then and else blocks as long as their only effect is
     to calculate a value for x.
     ??? For future expansion, further expand the "multiple X" rules.  */

  /* First look for multiple SETS.  The original costs already include
     a base cost of COSTS_N_INSNS (2): one instruction for the compare
     (which we will be needing either way) and one instruction for the
     branch.  When comparing costs we want to use the branch instruction
     cost and the sets vs. the cmovs generated here.  Therefore subtract
     the costs of the compare before checking.
     ??? Actually, instead of the branch instruction costs we might want
     to use COSTS_N_INSNS (BRANCH_COST ()) as in other places.  */

  unsigned potential_cost = if_info->original_cost - COSTS_N_INSNS (1);
  unsigned old_cost = if_info->original_cost;
  if (!else_bb
      && HAVE_conditional_move
      && bb_ok_for_noce_convert_multiple_sets (then_bb, &potential_cost))
    {
      /* Temporarily set the original costs to what we estimated so
	 we can determine if the transformation is worth it.  */
      if_info->original_cost = potential_cost;
      if (noce_convert_multiple_sets (if_info))
	{
	  if (dump_file && if_info->transform_name)
	    fprintf (dump_file, "if-conversion succeeded through %s\n",
		     if_info->transform_name);
	  return true;
	}

      /* Restore the original costs.  */
      if_info->original_cost = old_cost;
    }

  bool speed_p = optimize_bb_for_speed_p (test_bb);
  unsigned int then_cost = 0, else_cost = 0;
  if (!bb_valid_for_noce_process_p (then_bb, cond, &then_cost,
				    &if_info->then_simple))
    return false;

  if (else_bb
      && !bb_valid_for_noce_process_p (else_bb, cond, &else_cost,
				       &if_info->else_simple))
    return false;

  if (speed_p)
    if_info->original_cost += average_cost (then_cost, else_cost,
					    find_edge (test_bb, then_bb));
  else
    if_info->original_cost += then_cost + else_cost;

  insn_a = last_active_insn (then_bb, false);
  set_a = single_set (insn_a);
  gcc_assert (set_a);

  x = SET_DEST (set_a);
  a = SET_SRC (set_a);

  /* Look for the other potential set.  Make sure we've got equivalent
     destinations.  */
  /* ??? This is overconservative.  Storing to two different mems is
     as easy as conditionally computing the address.  Storing to a
     single mem merely requires a scratch memory to use as one of the
     destination addresses; often the memory immediately below the
     stack pointer is available for this.  */
  set_b = NULL_RTX;
  if (else_bb)
    {
      insn_b = last_active_insn (else_bb, false);
      set_b = single_set (insn_b);
      gcc_assert (set_b);

      if (!rtx_interchangeable_p (x, SET_DEST (set_b)))
	return false;
    }
  else
    {
      insn_b = if_info->cond_earliest;
      do
	insn_b = prev_nonnote_nondebug_insn (insn_b);
      while (insn_b
	     && (BLOCK_FOR_INSN (insn_b)
		 == BLOCK_FOR_INSN (if_info->cond_earliest))
	     && !modified_in_p (x, insn_b));

      /* We're going to be moving the evaluation of B down from above
	 COND_EARLIEST to JUMP.  Make sure the relevant data is still
	 intact.  */
      if (! insn_b
	  || BLOCK_FOR_INSN (insn_b) != BLOCK_FOR_INSN (if_info->cond_earliest)
	  || !NONJUMP_INSN_P (insn_b)
	  || (set_b = single_set (insn_b)) == NULL_RTX
	  || ! rtx_interchangeable_p (x, SET_DEST (set_b))
	  || ! noce_operand_ok (SET_SRC (set_b))
	  || reg_overlap_mentioned_p (x, SET_SRC (set_b))
	  || modified_between_p (SET_SRC (set_b), insn_b, jump)
	  /* Avoid extending the lifetime of hard registers on small
	     register class machines.  */
	  || (REG_P (SET_SRC (set_b))
	      && HARD_REGISTER_P (SET_SRC (set_b))
	      && targetm.small_register_classes_for_mode_p
		   (GET_MODE (SET_SRC (set_b))))
	  /* Likewise with X.  In particular this can happen when
	     noce_get_condition looks farther back in the instruction
	     stream than one might expect.  */
	  || reg_overlap_mentioned_p (x, cond)
	  || reg_overlap_mentioned_p (x, a)
	  || modified_between_p (x, insn_b, jump))
	{
	  insn_b = NULL;
	  set_b = NULL_RTX;
	}
    }

  /* If x has side effects then only the if-then-else form is safe to
     convert.  But even in that case we would need to restore any notes
     (such as REG_INC) at then end.  That can be tricky if
     noce_emit_move_insn expands to more than one insn, so disable the
     optimization entirely for now if there are side effects.  */
  if (side_effects_p (x))
    return false;

  b = (set_b ? SET_SRC (set_b) : x);

  /* Only operate on register destinations, and even then avoid extending
     the lifetime of hard registers on small register class machines.  */
  orig_x = x;
  if_info->orig_x = orig_x;
  if (!REG_P (x)
      || (HARD_REGISTER_P (x)
	  && targetm.small_register_classes_for_mode_p (GET_MODE (x))))
    {
      if (GET_MODE (x) == BLKmode)
	return false;

      if (GET_CODE (x) == ZERO_EXTRACT
	  && (!CONST_INT_P (XEXP (x, 1))
	      || !CONST_INT_P (XEXP (x, 2))))
	return false;

      x = gen_reg_rtx (GET_MODE (GET_CODE (x) == STRICT_LOW_PART
				 ? XEXP (x, 0) : x));
    }

  /* Don't operate on sources that may trap or are volatile.  */
  if (! noce_operand_ok (a) || ! noce_operand_ok (b))
    return false;

 retry:
  /* Set up the info block for our subroutines.  */
  if_info->insn_a = insn_a;
  if_info->insn_b = insn_b;
  if_info->x = x;
  if_info->a = a;
  if_info->b = b;

  /* Try optimizations in some approximation of a useful order.  */
  /* ??? Should first look to see if X is live incoming at all.  If it
     isn't, we don't need anything but an unconditional set.  */

  /* Look and see if A and B are really the same.  Avoid creating silly
     cmove constructs that no one will fix up later.  */
  if (noce_simple_bbs (if_info)
      && rtx_interchangeable_p (a, b))
    {
      /* If we have an INSN_B, we don't have to create any new rtl.  Just
	 move the instruction that we already have.  If we don't have an
	 INSN_B, that means that A == X, and we've got a noop move.  In
	 that case don't do anything and let the code below delete INSN_A.  */
      if (insn_b && else_bb)
	{
	  rtx note;

	  if (else_bb && insn_b == BB_END (else_bb))
	    BB_END (else_bb) = PREV_INSN (insn_b);
	  reorder_insns (insn_b, insn_b, PREV_INSN (jump));

	  /* If there was a REG_EQUAL note, delete it since it may have been
	     true due to this insn being after a jump.  */
	  if ((note = find_reg_note (insn_b, REG_EQUAL, NULL_RTX)) != 0)
	    remove_note (insn_b, note);

	  insn_b = NULL;
	}
      /* If we have "x = b; if (...) x = a;", and x has side-effects, then
	 x must be executed twice.  */
      else if (insn_b && side_effects_p (orig_x))
	return false;

      x = orig_x;
      goto success;
    }

  if (!set_b && MEM_P (orig_x))
    /* We want to avoid store speculation to avoid cases like
	 if (pthread_mutex_trylock(mutex))
	   ++global_variable;
       Rather than go to much effort here, we rely on the SSA optimizers,
       which do a good enough job these days.  */
    return false;

  if (noce_try_move (if_info))
    goto success;
  if (noce_try_ifelse_collapse (if_info))
    goto success;
  if (noce_try_store_flag (if_info))
    goto success;
  if (noce_try_bitop (if_info))
    goto success;
  if (noce_try_minmax (if_info))
    goto success;
  if (noce_try_abs (if_info))
    goto success;
  if (noce_try_inverse_constants (if_info))
    goto success;
  if (!targetm.have_conditional_execution ()
      && noce_try_store_flag_constants (if_info))
    goto success;
  if (HAVE_conditional_move
      && noce_try_cmove (if_info))
    goto success;
  if (! targetm.have_conditional_execution ())
    {
      if (noce_try_addcc (if_info))
	goto success;
      if (noce_try_store_flag_mask (if_info))
	goto success;
      if (HAVE_conditional_move
          && noce_try_cond_zero_arith (if_info))
	goto success;
      if (HAVE_conditional_move
	  && noce_try_cmove_arith (if_info))
	goto success;
      if (noce_try_sign_mask (if_info))
	goto success;
    }

  if (!else_bb && set_b)
    {
      insn_b = NULL;
      set_b = NULL_RTX;
      b = orig_x;
      goto retry;
    }

  return false;

 success:
  if (dump_file && if_info->transform_name)
    fprintf (dump_file, "if-conversion succeeded through %s\n",
	     if_info->transform_name);

  /* If we used a temporary, fix it up now.  */
  if (orig_x != x)
    {
      rtx_insn *seq;

      start_sequence ();
      noce_emit_move_insn (orig_x, x);
      seq = get_insns ();
      set_used_flags (orig_x);
      unshare_all_rtl_in_chain (seq);
      end_sequence ();

      emit_insn_before_setloc (seq, BB_END (test_bb), INSN_LOCATION (insn_a));
    }

  /* The original THEN and ELSE blocks may now be removed.  The test block
     must now jump to the join block.  If the test block and the join block
     can be merged, do so.  */
  if (else_bb)
    {
      delete_basic_block (else_bb);
      num_true_changes++;
    }
  else
    remove_edge (find_edge (test_bb, join_bb));

  remove_edge (find_edge (then_bb, join_bb));
  redirect_edge_and_branch_force (single_succ_edge (test_bb), join_bb);
  delete_basic_block (then_bb);
  num_true_changes++;

  if (can_merge_blocks_p (test_bb, join_bb))
    {
      merge_blocks (test_bb, join_bb);
      num_true_changes++;
    }

  num_updated_if_blocks++;
  return true;
}

/* Check whether a block is suitable for conditional move conversion.
   Every insn must be a simple set of a register to a constant or a
   register.  For each assignment, store the value in the pointer map
   VALS, keyed indexed by register pointer, then store the register
   pointer in REGS.  COND is the condition we will test.  */

static bool
check_cond_move_block (basic_block bb,
		       hash_map<rtx, rtx> *vals,
		       vec<rtx> *regs,
		       rtx cond)
{
  rtx_insn *insn;
  rtx cc = cc_in_cond (cond);

   /* We can only handle simple jumps at the end of the basic block.
      It is almost impossible to update the CFG otherwise.  */
  insn = BB_END (bb);
  if (JUMP_P (insn) && !onlyjump_p (insn))
    return false;

  FOR_BB_INSNS (bb, insn)
    {
      rtx set, dest, src;

      if (!NONDEBUG_INSN_P (insn) || JUMP_P (insn))
	continue;
      set = single_set (insn);
      if (!set)
	return false;

      dest = SET_DEST (set);
      src = SET_SRC (set);
      if (!REG_P (dest)
	  || (HARD_REGISTER_P (dest)
	      && targetm.small_register_classes_for_mode_p (GET_MODE (dest))))
	return false;

      if (!CONSTANT_P (src) && !register_operand (src, VOIDmode))
	return false;

      if (side_effects_p (src) || side_effects_p (dest))
	return false;

      if (may_trap_p (src) || may_trap_p (dest))
	return false;

      /* Don't try to handle this if the source register was
	 modified earlier in the block.  */
      if ((REG_P (src)
	   && vals->get (src))
	  || (GET_CODE (src) == SUBREG && REG_P (SUBREG_REG (src))
	      && vals->get (SUBREG_REG (src))))
	return false;

      /* Don't try to handle this if the destination register was
	 modified earlier in the block.  */
      if (vals->get (dest))
	return false;

      /* Don't try to handle this if the condition uses the
	 destination register.  */
      if (reg_overlap_mentioned_p (dest, cond))
	return false;

      /* Don't try to handle this if the source register is modified
	 later in the block.  */
      if (!CONSTANT_P (src)
	  && modified_between_p (src, insn, NEXT_INSN (BB_END (bb))))
	return false;

      /* Skip it if the instruction to be moved might clobber CC.  */
      if (cc && set_of (cc, insn))
	return false;

      vals->put (dest, src);

      regs->safe_push (dest);
    }

  return true;
}

/* Given a basic block BB suitable for conditional move conversion,
   a condition COND, and pointer maps THEN_VALS and ELSE_VALS containing
   the register values depending on COND, emit the insns in the block as
   conditional moves.  If ELSE_BLOCK is true, THEN_BB was already
   processed.  The caller has started a sequence for the conversion.
   Return true if successful, false if something goes wrong.  */

static bool
cond_move_convert_if_block (struct noce_if_info *if_infop,
			    basic_block bb, rtx cond,
			    hash_map<rtx, rtx> *then_vals,
			    hash_map<rtx, rtx> *else_vals,
			    bool else_block_p)
{
  enum rtx_code code;
  rtx_insn *insn;
  rtx cond_arg0, cond_arg1;

  code = GET_CODE (cond);
  cond_arg0 = XEXP (cond, 0);
  cond_arg1 = XEXP (cond, 1);

  FOR_BB_INSNS (bb, insn)
    {
      rtx set, target, dest, t, e;

      /* ??? Maybe emit conditional debug insn?  */
      if (!NONDEBUG_INSN_P (insn) || JUMP_P (insn))
	continue;
      set = single_set (insn);
      gcc_assert (set && REG_P (SET_DEST (set)));

      dest = SET_DEST (set);

      rtx *then_slot = then_vals->get (dest);
      rtx *else_slot = else_vals->get (dest);
      t = then_slot ? *then_slot : NULL_RTX;
      e = else_slot ? *else_slot : NULL_RTX;

      if (else_block_p)
	{
	  /* If this register was set in the then block, we already
	     handled this case there.  */
	  if (t)
	    continue;
	  t = dest;
	  gcc_assert (e);
	}
      else
	{
	  gcc_assert (t);
	  if (!e)
	    e = dest;
	}

      if (if_infop->cond_inverted)
	std::swap (t, e);

      target = noce_emit_cmove (if_infop, dest, code, cond_arg0, cond_arg1,
				t, e);
      if (!target)
	return false;

      if (target != dest)
	noce_emit_move_insn (dest, target);
    }

  return true;
}

/* Given a simple IF-THEN-JOIN or IF-THEN-ELSE-JOIN block, attempt to convert
   it using only conditional moves.  Return TRUE if we were successful at
   converting the block.  */

static bool
cond_move_process_if_block (struct noce_if_info *if_info)
{
  basic_block test_bb = if_info->test_bb;
  basic_block then_bb = if_info->then_bb;
  basic_block else_bb = if_info->else_bb;
  basic_block join_bb = if_info->join_bb;
  rtx_insn *jump = if_info->jump;
  rtx cond = if_info->cond;
  rtx_insn *seq, *loc_insn;
  int c;
  vec<rtx> then_regs = vNULL;
  vec<rtx> else_regs = vNULL;
  bool success_p = false;
  int limit = param_max_rtl_if_conversion_insns;

  /* Build a mapping for each block to the value used for each
     register.  */
  hash_map<rtx, rtx> then_vals;
  hash_map<rtx, rtx> else_vals;

  /* Make sure the blocks are suitable.  */
  if (!check_cond_move_block (then_bb, &then_vals, &then_regs, cond)
      || (else_bb
	  && !check_cond_move_block (else_bb, &else_vals, &else_regs, cond)))
    goto done;

  /* Make sure the blocks can be used together.  If the same register
     is set in both blocks, and is not set to a constant in both
     cases, then both blocks must set it to the same register.  We
     have already verified that if it is set to a register, that the
     source register does not change after the assignment.  Also count
     the number of registers set in only one of the blocks.  */
  c = 0;
  for (rtx reg : then_regs)
    {
      rtx *then_slot = then_vals.get (reg);
      rtx *else_slot = else_vals.get (reg);

      gcc_checking_assert (then_slot);
      if (!else_slot)
	++c;
      else
	{
	  rtx then_val = *then_slot;
	  rtx else_val = *else_slot;
	  if (!CONSTANT_P (then_val) && !CONSTANT_P (else_val)
	      && !rtx_equal_p (then_val, else_val))
	    goto done;
	}
    }

  /* Finish off c for MAX_CONDITIONAL_EXECUTE.  */
  for (rtx reg : else_regs)
    {
      gcc_checking_assert (else_vals.get (reg));
      if (!then_vals.get (reg))
	++c;
    }

  /* Make sure it is reasonable to convert this block.  What matters
     is the number of assignments currently made in only one of the
     branches, since if we convert we are going to always execute
     them.  */
  if (c > MAX_CONDITIONAL_EXECUTE
      || c > limit)
    goto done;

  /* Try to emit the conditional moves.  First do the then block,
     then do anything left in the else blocks.  */
  start_sequence ();
  if (!cond_move_convert_if_block (if_info, then_bb, cond,
				   &then_vals, &else_vals, false)
      || (else_bb
	  && !cond_move_convert_if_block (if_info, else_bb, cond,
					  &then_vals, &else_vals, true)))
    {
      end_sequence ();
      goto done;
    }
  seq = end_ifcvt_sequence (if_info);
  if (!seq || !targetm.noce_conversion_profitable_p (seq, if_info))
    goto done;

  loc_insn = first_active_insn (then_bb);
  if (!loc_insn)
    {
      loc_insn = first_active_insn (else_bb);
      gcc_assert (loc_insn);
    }
  emit_insn_before_setloc (seq, jump, INSN_LOCATION (loc_insn));

  if (else_bb)
    {
      delete_basic_block (else_bb);
      num_true_changes++;
    }
  else
    remove_edge (find_edge (test_bb, join_bb));

  remove_edge (find_edge (then_bb, join_bb));
  redirect_edge_and_branch_force (single_succ_edge (test_bb), join_bb);
  delete_basic_block (then_bb);
  num_true_changes++;

  if (can_merge_blocks_p (test_bb, join_bb))
    {
      merge_blocks (test_bb, join_bb);
      num_true_changes++;
    }

  num_updated_if_blocks++;
  success_p = true;

done:
  then_regs.release ();
  else_regs.release ();
  return success_p;
}


/* Determine if a given basic block heads a simple IF-THEN-JOIN or an
   IF-THEN-ELSE-JOIN block.

   If so, we'll try to convert the insns to not require the branch,
   using only transformations that do not require conditional execution.

   Return TRUE if we were successful at converting the block.  */

static bool
noce_find_if_block (basic_block test_bb, edge then_edge, edge else_edge,
		    int pass)
{
  basic_block then_bb, else_bb, join_bb;
  bool then_else_reversed = false;
  rtx_insn *jump;
  rtx_insn *cond_earliest;
  struct noce_if_info if_info;
  bool speed_p = optimize_bb_for_speed_p (test_bb);

  /* We only ever should get here before reload.  */
  gcc_assert (!reload_completed);

  /* Recognize an IF-THEN-ELSE-JOIN block.  */
  if (single_pred_p (then_edge->dest)
      && single_succ_p (then_edge->dest)
      && single_pred_p (else_edge->dest)
      && single_succ_p (else_edge->dest)
      && single_succ (then_edge->dest) == single_succ (else_edge->dest))
    {
      then_bb = then_edge->dest;
      else_bb = else_edge->dest;
      join_bb = single_succ (then_bb);
    }
  /* Recognize an IF-THEN-JOIN block.  */
  else if (single_pred_p (then_edge->dest)
	   && single_succ_p (then_edge->dest)
	   && single_succ (then_edge->dest) == else_edge->dest)
    {
      then_bb = then_edge->dest;
      else_bb = NULL_BLOCK;
      join_bb = else_edge->dest;
    }
  /* Recognize an IF-ELSE-JOIN block.  We can have those because the order
     of basic blocks in cfglayout mode does not matter, so the fallthrough
     edge can go to any basic block (and not just to bb->next_bb, like in
     cfgrtl mode).  */
  else if (single_pred_p (else_edge->dest)
	   && single_succ_p (else_edge->dest)
	   && single_succ (else_edge->dest) == then_edge->dest)
    {
      /* The noce transformations do not apply to IF-ELSE-JOIN blocks.
	 To make this work, we have to invert the THEN and ELSE blocks
	 and reverse the jump condition.  */
      then_bb = else_edge->dest;
      else_bb = NULL_BLOCK;
      join_bb = single_succ (then_bb);
      then_else_reversed = true;
    }
  else
    /* Not a form we can handle.  */
    return false;

  /* The edges of the THEN and ELSE blocks cannot have complex edges.  */
  if (single_succ_edge (then_bb)->flags & EDGE_COMPLEX)
    return false;
  if (else_bb
      && single_succ_edge (else_bb)->flags & EDGE_COMPLEX)
    return false;

  num_possible_if_blocks++;

  if (dump_file)
    {
      fprintf (dump_file,
	       "\nIF-THEN%s-JOIN block found, pass %d, test %d, then %d",
	       (else_bb) ? "-ELSE" : "",
	       pass, test_bb->index, then_bb->index);

      if (else_bb)
	fprintf (dump_file, ", else %d", else_bb->index);

      fprintf (dump_file, ", join %d\n", join_bb->index);
    }

  /* If the conditional jump is more than just a conditional
     jump, then we cannot do if-conversion on this block.  */
  jump = BB_END (test_bb);
  if (! onlyjump_p (jump))
    return false;

  /* Initialize an IF_INFO struct to pass around.  */
  memset (&if_info, 0, sizeof if_info);
  if_info.test_bb = test_bb;
  if_info.then_bb = then_bb;
  if_info.else_bb = else_bb;
  if_info.join_bb = join_bb;
  if_info.cond = noce_get_condition (jump, &cond_earliest,
				     then_else_reversed);
  rtx_insn *rev_cond_earliest;
  if_info.rev_cond = noce_get_condition (jump, &rev_cond_earliest,
					 !then_else_reversed);
  if (!if_info.cond && !if_info.rev_cond)
    return false;
  if (!if_info.cond)
    {
      std::swap (if_info.cond, if_info.rev_cond);
      std::swap (cond_earliest, rev_cond_earliest);
      if_info.cond_inverted = true;
    }
  /* We must be comparing objects whose modes imply the size.  */
  if (GET_MODE (XEXP (if_info.cond, 0)) == BLKmode)
    return false;
  gcc_assert (if_info.rev_cond == NULL_RTX
	      || rev_cond_earliest == cond_earliest);
  if_info.cond_earliest = cond_earliest;
  if_info.jump = jump;
  if_info.then_else_reversed = then_else_reversed;
  if_info.speed_p = speed_p;
  if_info.max_seq_cost
    = targetm.max_noce_ifcvt_seq_cost (then_edge);
  /* We'll add in the cost of THEN_BB and ELSE_BB later, when we check
     that they are valid to transform.  We can't easily get back to the insn
     for COND (and it may not exist if we had to canonicalize to get COND),
     and jump_insns are always given a cost of 1 by seq_cost, so treat
     both instructions as having cost COSTS_N_INSNS (1).  */
  if_info.original_cost = COSTS_N_INSNS (2);


  /* Do the real work.  */

  /* ??? noce_process_if_block has not yet been updated to handle
     inverted conditions.  */
  if (!if_info.cond_inverted && noce_process_if_block (&if_info))
    return true;

  if (HAVE_conditional_move
      && cond_move_process_if_block (&if_info))
    return true;

  return false;
}


/* Merge the blocks and mark for local life update.  */

static void
merge_if_block (struct ce_if_block * ce_info)
{
  basic_block test_bb = ce_info->test_bb;	/* last test block */
  basic_block then_bb = ce_info->then_bb;	/* THEN */
  basic_block else_bb = ce_info->else_bb;	/* ELSE or NULL */
  basic_block join_bb = ce_info->join_bb;	/* join block */
  basic_block combo_bb;

  /* All block merging is done into the lower block numbers.  */

  combo_bb = test_bb;
  df_set_bb_dirty (test_bb);

  /* Merge any basic blocks to handle && and || subtests.  Each of
     the blocks are on the fallthru path from the predecessor block.  */
  if (ce_info->num_multiple_test_blocks > 0)
    {
      basic_block bb = test_bb;
      basic_block last_test_bb = ce_info->last_test_bb;
      basic_block fallthru = block_fallthru (bb);

      do
	{
	  bb = fallthru;
	  fallthru = block_fallthru (bb);
	  merge_blocks (combo_bb, bb);
	  num_true_changes++;
	}
      while (bb != last_test_bb);
    }

  /* Merge TEST block into THEN block.  Normally the THEN block won't have a
     label, but it might if there were || tests.  That label's count should be
     zero, and it normally should be removed.  */

  if (then_bb)
    {
      /* If THEN_BB has no successors, then there's a BARRIER after it.
	 If COMBO_BB has more than one successor (THEN_BB), then that BARRIER
	 is no longer needed, and in fact it is incorrect to leave it in
	 the insn stream.  */
      if (EDGE_COUNT (then_bb->succs) == 0
	  && EDGE_COUNT (combo_bb->succs) > 1)
	{
	  rtx_insn *end = NEXT_INSN (BB_END (then_bb));
	  while (end && NOTE_P (end) && !NOTE_INSN_BASIC_BLOCK_P (end))
	    end = NEXT_INSN (end);

	  if (end && BARRIER_P (end))
	    delete_insn (end);
	}
      merge_blocks (combo_bb, then_bb);
      num_true_changes++;
    }

  /* The ELSE block, if it existed, had a label.  That label count
     will almost always be zero, but odd things can happen when labels
     get their addresses taken.  */
  if (else_bb)
    {
      /* If ELSE_BB has no successors, then there's a BARRIER after it.
	 If COMBO_BB has more than one successor (ELSE_BB), then that BARRIER
	 is no longer needed, and in fact it is incorrect to leave it in
	 the insn stream.  */
      if (EDGE_COUNT (else_bb->succs) == 0
	  && EDGE_COUNT (combo_bb->succs) > 1)
	{
	  rtx_insn *end = NEXT_INSN (BB_END (else_bb));
	  while (end && NOTE_P (end) && !NOTE_INSN_BASIC_BLOCK_P (end))
	    end = NEXT_INSN (end);

	  if (end && BARRIER_P (end))
	    delete_insn (end);
	}
      merge_blocks (combo_bb, else_bb);
      num_true_changes++;
    }

  /* If there was no join block reported, that means it was not adjacent
     to the others, and so we cannot merge them.  */

  if (! join_bb)
    {
      rtx_insn *last = BB_END (combo_bb);

      /* The outgoing edge for the current COMBO block should already
	 be correct.  Verify this.  */
      if (EDGE_COUNT (combo_bb->succs) == 0)
	gcc_assert (find_reg_note (last, REG_NORETURN, NULL)
		    || (NONJUMP_INSN_P (last)
			&& GET_CODE (PATTERN (last)) == TRAP_IF
			&& (TRAP_CONDITION (PATTERN (last))
			    == const_true_rtx)));

      else
      /* There should still be something at the end of the THEN or ELSE
         blocks taking us to our final destination.  */
	gcc_assert (JUMP_P (last)
		    || (EDGE_SUCC (combo_bb, 0)->dest
			== EXIT_BLOCK_PTR_FOR_FN (cfun)
			&& CALL_P (last)
			&& SIBLING_CALL_P (last))
		    || ((EDGE_SUCC (combo_bb, 0)->flags & EDGE_EH)
			&& can_throw_internal (last)));
    }

  /* The JOIN block may have had quite a number of other predecessors too.
     Since we've already merged the TEST, THEN and ELSE blocks, we should
     have only one remaining edge from our if-then-else diamond.  If there
     is more than one remaining edge, it must come from elsewhere.  There
     may be zero incoming edges if the THEN block didn't actually join
     back up (as with a call to a non-return function).  */
  else if (EDGE_COUNT (join_bb->preds) < 2
	   && join_bb != EXIT_BLOCK_PTR_FOR_FN (cfun))
    {
      /* We can merge the JOIN cleanly and update the dataflow try
	 again on this pass.*/
      merge_blocks (combo_bb, join_bb);
      num_true_changes++;
    }
  else
    {
      /* We cannot merge the JOIN.  */

      /* The outgoing edge for the current COMBO block should already
	 be correct.  Verify this.  */
      gcc_assert (single_succ_p (combo_bb)
		  && single_succ (combo_bb) == join_bb);

      /* Remove the jump and cruft from the end of the COMBO block.  */
      if (join_bb != EXIT_BLOCK_PTR_FOR_FN (cfun))
	tidy_fallthru_edge (single_succ_edge (combo_bb));
    }

  num_updated_if_blocks++;
}

/* Find a block ending in a simple IF condition and try to transform it
   in some way.  When converting a multi-block condition, put the new code
   in the first such block and delete the rest.  Return a pointer to this
   first block if some transformation was done.  Return NULL otherwise.  */

static basic_block
find_if_header (basic_block test_bb, int pass)
{
  ce_if_block ce_info;
  edge then_edge;
  edge else_edge;

  /* The kind of block we're looking for has exactly two successors.  */
  if (EDGE_COUNT (test_bb->succs) != 2)
    return NULL;

  then_edge = EDGE_SUCC (test_bb, 0);
  else_edge = EDGE_SUCC (test_bb, 1);

  if (df_get_bb_dirty (then_edge->dest))
    return NULL;
  if (df_get_bb_dirty (else_edge->dest))
    return NULL;

  /* Neither edge should be abnormal.  */
  if ((then_edge->flags & EDGE_COMPLEX)
      || (else_edge->flags & EDGE_COMPLEX))
    return NULL;

  /* Nor exit the loop.  */
  if ((then_edge->flags & EDGE_LOOP_EXIT)
      || (else_edge->flags & EDGE_LOOP_EXIT))
    return NULL;

  /* The THEN edge is canonically the one that falls through.  */
  if (then_edge->flags & EDGE_FALLTHRU)
    ;
  else if (else_edge->flags & EDGE_FALLTHRU)
    std::swap (then_edge, else_edge);
  else
    /* Otherwise this must be a multiway branch of some sort.  */
    return NULL;

  memset (&ce_info, 0, sizeof (ce_info));
  ce_info.test_bb = test_bb;
  ce_info.then_bb = then_edge->dest;
  ce_info.else_bb = else_edge->dest;
  ce_info.pass = pass;

#ifdef IFCVT_MACHDEP_INIT
  IFCVT_MACHDEP_INIT (&ce_info);
#endif

  if (!reload_completed
      && noce_find_if_block (test_bb, then_edge, else_edge, pass))
    goto success;

  if (reload_completed
      && targetm.have_conditional_execution ()
      && cond_exec_find_if_block (&ce_info))
    goto success;

  if (targetm.have_trap ()
      && optab_handler (ctrap_optab, word_mode) != CODE_FOR_nothing
      && find_cond_trap (test_bb, then_edge, else_edge))
    goto success;

  if (dom_info_state (CDI_POST_DOMINATORS) >= DOM_NO_FAST_QUERY
      && (reload_completed || !targetm.have_conditional_execution ()))
    {
      if (find_if_case_1 (test_bb, then_edge, else_edge))
	goto success;
      if (find_if_case_2 (test_bb, then_edge, else_edge))
	goto success;
    }

  return NULL;

 success:
  if (dump_file)
    fprintf (dump_file, "Conversion succeeded on pass %d.\n", pass);
  /* Set this so we continue looking.  */
  cond_exec_changed_p = true;
  return ce_info.test_bb;
}

/* Return true if a block has two edges, one of which falls through to the next
   block, and the other jumps to a specific block, so that we can tell if the
   block is part of an && test or an || test.  Returns either -1 or the number
   of non-note, non-jump, non-USE/CLOBBER insns in the block.  */

static int
block_jumps_and_fallthru (basic_block cur_bb, basic_block target_bb)
{
  edge cur_edge;
  bool fallthru_p = false;
  bool jump_p = false;
  rtx_insn *insn;
  rtx_insn *end;
  int n_insns = 0;
  edge_iterator ei;

  if (!cur_bb || !target_bb)
    return -1;

  /* If no edges, obviously it doesn't jump or fallthru.  */
  if (EDGE_COUNT (cur_bb->succs) == 0)
    return 0;

  FOR_EACH_EDGE (cur_edge, ei, cur_bb->succs)
    {
      if (cur_edge->flags & EDGE_COMPLEX)
	/* Anything complex isn't what we want.  */
	return -1;

      else if (cur_edge->flags & EDGE_FALLTHRU)
	fallthru_p = true;

      else if (cur_edge->dest == target_bb)
	jump_p = true;

      else
	return -1;
    }

  if ((jump_p & fallthru_p) == 0)
    return -1;

  /* Don't allow calls in the block, since this is used to group && and ||
     together for conditional execution support.  ??? we should support
     conditional execution support across calls for IA-64 some day, but
     for now it makes the code simpler.  */
  end = BB_END (cur_bb);
  insn = BB_HEAD (cur_bb);

  while (insn != NULL_RTX)
    {
      if (CALL_P (insn))
	return -1;

      if (INSN_P (insn)
	  && !JUMP_P (insn)
	  && !DEBUG_INSN_P (insn)
	  && GET_CODE (PATTERN (insn)) != USE
	  && GET_CODE (PATTERN (insn)) != CLOBBER)
	n_insns++;

      if (insn == end)
	break;

      insn = NEXT_INSN (insn);
    }

  return n_insns;
}

/* Determine if a given basic block heads a simple IF-THEN or IF-THEN-ELSE
   block.  If so, we'll try to convert the insns to not require the branch.
   Return TRUE if we were successful at converting the block.  */

static bool
cond_exec_find_if_block (struct ce_if_block * ce_info)
{
  basic_block test_bb = ce_info->test_bb;
  basic_block then_bb = ce_info->then_bb;
  basic_block else_bb = ce_info->else_bb;
  basic_block join_bb = NULL_BLOCK;
  edge cur_edge;
  basic_block next;
  edge_iterator ei;

  ce_info->last_test_bb = test_bb;

  /* We only ever should get here after reload,
     and if we have conditional execution.  */
  gcc_assert (reload_completed && targetm.have_conditional_execution ());

  /* Discover if any fall through predecessors of the current test basic block
     were && tests (which jump to the else block) or || tests (which jump to
     the then block).  */
  if (single_pred_p (test_bb)
      && single_pred_edge (test_bb)->flags == EDGE_FALLTHRU)
    {
      basic_block bb = single_pred (test_bb);
      basic_block target_bb;
      int max_insns = MAX_CONDITIONAL_EXECUTE;
      int n_insns;

      /* Determine if the preceding block is an && or || block.  */
      if ((n_insns = block_jumps_and_fallthru (bb, else_bb)) >= 0)
	{
	  ce_info->and_and_p = true;
	  target_bb = else_bb;
	}
      else if ((n_insns = block_jumps_and_fallthru (bb, then_bb)) >= 0)
	{
	  ce_info->and_and_p = false;
	  target_bb = then_bb;
	}
      else
	target_bb = NULL_BLOCK;

      if (target_bb && n_insns <= max_insns)
	{
	  int total_insns = 0;
	  int blocks = 0;

	  ce_info->last_test_bb = test_bb;

	  /* Found at least one && or || block, look for more.  */
	  do
	    {
	      ce_info->test_bb = test_bb = bb;
	      total_insns += n_insns;
	      blocks++;

	      if (!single_pred_p (bb))
		break;

	      bb = single_pred (bb);
	      n_insns = block_jumps_and_fallthru (bb, target_bb);
	    }
	  while (n_insns >= 0 && (total_insns + n_insns) <= max_insns);

	  ce_info->num_multiple_test_blocks = blocks;
	  ce_info->num_multiple_test_insns = total_insns;

	  if (ce_info->and_and_p)
	    ce_info->num_and_and_blocks = blocks;
	  else
	    ce_info->num_or_or_blocks = blocks;
	}
    }

  /* The THEN block of an IF-THEN combo must have exactly one predecessor,
     other than any || blocks which jump to the THEN block.  */
  if ((EDGE_COUNT (then_bb->preds) - ce_info->num_or_or_blocks) != 1)
    return false;

  /* The edges of the THEN and ELSE blocks cannot have complex edges.  */
  FOR_EACH_EDGE (cur_edge, ei, then_bb->preds)
    {
      if (cur_edge->flags & EDGE_COMPLEX)
	return false;
    }

  FOR_EACH_EDGE (cur_edge, ei, else_bb->preds)
    {
      if (cur_edge->flags & EDGE_COMPLEX)
	return false;
    }

  /* The THEN block of an IF-THEN combo must have zero or one successors.  */
  if (EDGE_COUNT (then_bb->succs) > 0
      && (!single_succ_p (then_bb)
          || (single_succ_edge (then_bb)->flags & EDGE_COMPLEX)
	  || (epilogue_completed
	      && tablejump_p (BB_END (then_bb), NULL, NULL))))
    return false;

  /* If the THEN block has no successors, conditional execution can still
     make a conditional call.  Don't do this unless the ELSE block has
     only one incoming edge -- the CFG manipulation is too ugly otherwise.
     Check for the last insn of the THEN block being an indirect jump, which
     is listed as not having any successors, but confuses the rest of the CE
     code processing.  ??? we should fix this in the future.  */
  if (EDGE_COUNT (then_bb->succs) == 0)
    {
      if (single_pred_p (else_bb) && else_bb != EXIT_BLOCK_PTR_FOR_FN (cfun))
	{
	  rtx_insn *last_insn = BB_END (then_bb);

	  while (last_insn
		 && NOTE_P (last_insn)
		 && last_insn != BB_HEAD (then_bb))
	    last_insn = PREV_INSN (last_insn);

	  if (last_insn
	      && JUMP_P (last_insn)
	      && ! simplejump_p (last_insn))
	    return false;

	  join_bb = else_bb;
	  else_bb = NULL_BLOCK;
	}
      else
	return false;
    }

  /* If the THEN block's successor is the other edge out of the TEST block,
     then we have an IF-THEN combo without an ELSE.  */
  else if (single_succ (then_bb) == else_bb)
    {
      join_bb = else_bb;
      else_bb = NULL_BLOCK;
    }

  /* If the THEN and ELSE block meet in a subsequent block, and the ELSE
     has exactly one predecessor and one successor, and the outgoing edge
     is not complex, then we have an IF-THEN-ELSE combo.  */
  else if (single_succ_p (else_bb)
	   && single_succ (then_bb) == single_succ (else_bb)
	   && single_pred_p (else_bb)
	   && !(single_succ_edge (else_bb)->flags & EDGE_COMPLEX)
	   && !(epilogue_completed
		&& tablejump_p (BB_END (else_bb), NULL, NULL)))
    join_bb = single_succ (else_bb);

  /* Otherwise it is not an IF-THEN or IF-THEN-ELSE combination.  */
  else
    return false;

  num_possible_if_blocks++;

  if (dump_file)
    {
      fprintf (dump_file,
	       "\nIF-THEN%s block found, pass %d, start block %d "
	       "[insn %d], then %d [%d]",
	       (else_bb) ? "-ELSE" : "",
	       ce_info->pass,
	       test_bb->index,
	       BB_HEAD (test_bb) ? (int)INSN_UID (BB_HEAD (test_bb)) : -1,
	       then_bb->index,
	       BB_HEAD (then_bb) ? (int)INSN_UID (BB_HEAD (then_bb)) : -1);

      if (else_bb)
	fprintf (dump_file, ", else %d [%d]",
		 else_bb->index,
		 BB_HEAD (else_bb) ? (int)INSN_UID (BB_HEAD (else_bb)) : -1);

      fprintf (dump_file, ", join %d [%d]",
	       join_bb->index,
	       BB_HEAD (join_bb) ? (int)INSN_UID (BB_HEAD (join_bb)) : -1);

      if (ce_info->num_multiple_test_blocks > 0)
	fprintf (dump_file, ", %d %s block%s last test %d [%d]",
		 ce_info->num_multiple_test_blocks,
		 (ce_info->and_and_p) ? "&&" : "||",
		 (ce_info->num_multiple_test_blocks == 1) ? "" : "s",
		 ce_info->last_test_bb->index,
		 ((BB_HEAD (ce_info->last_test_bb))
		  ? (int)INSN_UID (BB_HEAD (ce_info->last_test_bb))
		  : -1));

      fputc ('\n', dump_file);
    }

  /* Make sure IF, THEN, and ELSE, blocks are adjacent.  Actually, we get the
     first condition for free, since we've already asserted that there's a
     fallthru edge from IF to THEN.  Likewise for the && and || blocks, since
     we checked the FALLTHRU flag, those are already adjacent to the last IF
     block.  */
  /* ??? As an enhancement, move the ELSE block.  Have to deal with
     BLOCK notes, if by no other means than backing out the merge if they
     exist.  Sticky enough I don't want to think about it now.  */
  next = then_bb;
  if (else_bb && (next = next->next_bb) != else_bb)
    return false;
  if ((next = next->next_bb) != join_bb
      && join_bb != EXIT_BLOCK_PTR_FOR_FN (cfun))
    {
      if (else_bb)
	join_bb = NULL;
      else
	return false;
    }

  /* Do the real work.  */

  ce_info->else_bb = else_bb;
  ce_info->join_bb = join_bb;

  /* If we have && and || tests, try to first handle combining the && and ||
     tests into the conditional code, and if that fails, go back and handle
     it without the && and ||, which at present handles the && case if there
     was no ELSE block.  */
  if (cond_exec_process_if_block (ce_info, true))
    return true;

  if (ce_info->num_multiple_test_blocks)
    {
      cancel_changes (0);

      if (cond_exec_process_if_block (ce_info, false))
	return true;
    }

  return false;
}

/* Convert a branch over a trap, or a branch
   to a trap, into a conditional trap.  */

static bool
find_cond_trap (basic_block test_bb, edge then_edge, edge else_edge)
{
  basic_block then_bb = then_edge->dest;
  basic_block else_bb = else_edge->dest;
  basic_block other_bb, trap_bb;
  rtx_insn *trap, *jump;
  rtx cond;
  rtx_insn *cond_earliest;

  /* Locate the block with the trap instruction.  */
  /* ??? While we look for no successors, we really ought to allow
     EH successors.  Need to fix merge_if_block for that to work.  */
  if ((trap = block_has_only_trap (then_bb)) != NULL)
    trap_bb = then_bb, other_bb = else_bb;
  else if ((trap = block_has_only_trap (else_bb)) != NULL)
    trap_bb = else_bb, other_bb = then_bb;
  else
    return false;

  if (dump_file)
    {
      fprintf (dump_file, "\nTRAP-IF block found, start %d, trap %d\n",
	       test_bb->index, trap_bb->index);
    }

  /* If this is not a standard conditional jump, we can't parse it.  */
  jump = BB_END (test_bb);
  cond = noce_get_condition (jump, &cond_earliest, then_bb == trap_bb);
  if (! cond)
    return false;

  /* If the conditional jump is more than just a conditional jump, then
     we cannot do if-conversion on this block.  Give up for returnjump_p,
     changing a conditional return followed by unconditional trap for
     conditional trap followed by unconditional return is likely not
     beneficial and harder to handle.  */
  if (! onlyjump_p (jump) || returnjump_p (jump))
    return false;

  /* We must be comparing objects whose modes imply the size.  */
  if (GET_MODE (XEXP (cond, 0)) == BLKmode)
    return false;

  /* Attempt to generate the conditional trap.  */
  rtx_insn *seq = gen_cond_trap (GET_CODE (cond), copy_rtx (XEXP (cond, 0)),
				 copy_rtx (XEXP (cond, 1)),
				 TRAP_CODE (PATTERN (trap)));
  if (seq == NULL)
    return false;

  /* If that results in an invalid insn, back out.  */
  for (rtx_insn *x = seq; x; x = NEXT_INSN (x))
    if (reload_completed
	? !valid_insn_p (x)
	: recog_memoized (x) < 0)
      return false;

  /* Emit the new insns before cond_earliest.  */
  emit_insn_before_setloc (seq, cond_earliest, INSN_LOCATION (trap));

  /* Delete the trap block if possible.  */
  remove_edge (trap_bb == then_bb ? then_edge : else_edge);
  df_set_bb_dirty (test_bb);
  df_set_bb_dirty (then_bb);
  df_set_bb_dirty (else_bb);

  if (EDGE_COUNT (trap_bb->preds) == 0)
    {
      delete_basic_block (trap_bb);
      num_true_changes++;
    }

  /* Wire together the blocks again.  */
  if (current_ir_type () == IR_RTL_CFGLAYOUT)
    single_succ_edge (test_bb)->flags |= EDGE_FALLTHRU;
  else if (trap_bb == then_bb)
    {
      rtx lab = JUMP_LABEL (jump);
      rtx_insn *seq = targetm.gen_jump (lab);
      rtx_jump_insn *newjump = emit_jump_insn_after (seq, jump);
      LABEL_NUSES (lab) += 1;
      JUMP_LABEL (newjump) = lab;
      emit_barrier_after (newjump);
    }
  delete_insn (jump);

  if (can_merge_blocks_p (test_bb, other_bb))
    {
      merge_blocks (test_bb, other_bb);
      num_true_changes++;
    }

  num_updated_if_blocks++;
  return true;
}

/* Subroutine of find_cond_trap: if BB contains only a trap insn,
   return it.  */

static rtx_insn *
block_has_only_trap (basic_block bb)
{
  rtx_insn *trap;

  /* We're not the exit block.  */
  if (bb == EXIT_BLOCK_PTR_FOR_FN (cfun))
    return NULL;

  /* The block must have no successors.  */
  if (EDGE_COUNT (bb->succs) > 0)
    return NULL;

  /* The only instruction in the THEN block must be the trap.  */
  trap = first_active_insn (bb);
  if (! (trap == BB_END (bb)
	 && GET_CODE (PATTERN (trap)) == TRAP_IF
         && TRAP_CONDITION (PATTERN (trap)) == const_true_rtx))
    return NULL;

  return trap;
}

/* Look for IF-THEN-ELSE cases in which one of THEN or ELSE is
   transformable, but not necessarily the other.  There need be no
   JOIN block.

   Return TRUE if we were successful at converting the block.

   Cases we'd like to look at:

   (1)
	if (test) goto over; // x not live
	x = a;
	goto label;
	over:

   becomes

	x = a;
	if (! test) goto label;

   (2)
	if (test) goto E; // x not live
	x = big();
	goto L;
	E:
	x = b;
	goto M;

   becomes

	x = b;
	if (test) goto M;
	x = big();
	goto L;

   (3) // This one's really only interesting for targets that can do
       // multiway branching, e.g. IA-64 BBB bundles.  For other targets
       // it results in multiple branches on a cache line, which often
       // does not sit well with predictors.

	if (test1) goto E; // predicted not taken
	x = a;
	if (test2) goto F;
	...
	E:
	x = b;
	J:

   becomes

	x = a;
	if (test1) goto E;
	if (test2) goto F;

   Notes:

   (A) Don't do (2) if the branch is predicted against the block we're
   eliminating.  Do it anyway if we can eliminate a branch; this requires
   that the sole successor of the eliminated block postdominate the other
   side of the if.

   (B) With CE, on (3) we can steal from both sides of the if, creating

	if (test1) x = a;
	if (!test1) x = b;
	if (test1) goto J;
	if (test2) goto F;
	...
	J:

   Again, this is most useful if J postdominates.

   (C) CE substitutes for helpful life information.

   (D) These heuristics need a lot of work.  */

/* Tests for case 1 above.  */

static bool
find_if_case_1 (basic_block test_bb, edge then_edge, edge else_edge)
{
  basic_block then_bb = then_edge->dest;
  basic_block else_bb = else_edge->dest;
  basic_block new_bb;
  int then_bb_index;
  profile_probability then_prob;
  rtx else_target = NULL_RTX;

  /* If we are partitioning hot/cold basic blocks, we don't want to
     mess up unconditional or indirect jumps that cross between hot
     and cold sections.

     Basic block partitioning may result in some jumps that appear to
     be optimizable (or blocks that appear to be mergeable), but which really
     must be left untouched (they are required to make it safely across
     partition boundaries).  See  the comments at the top of
     bb-reorder.cc:partition_hot_cold_basic_blocks for complete details.  */

  if ((BB_END (then_bb)
       && JUMP_P (BB_END (then_bb))
       && CROSSING_JUMP_P (BB_END (then_bb)))
      || (JUMP_P (BB_END (test_bb))
	  && CROSSING_JUMP_P (BB_END (test_bb)))
      || (BB_END (else_bb)
	  && JUMP_P (BB_END (else_bb))
	  && CROSSING_JUMP_P (BB_END (else_bb))))
    return false;

  /* Verify test_bb ends in a conditional jump with no other side-effects.  */
  if (!onlyjump_p (BB_END (test_bb)))
    return false;

  /* THEN has one successor.  */
  if (!single_succ_p (then_bb))
    return false;

  /* THEN does not fall through, but is not strange either.  */
  if (single_succ_edge (then_bb)->flags & (EDGE_COMPLEX | EDGE_FALLTHRU))
    return false;

  /* THEN has one predecessor.  */
  if (!single_pred_p (then_bb))
    return false;

  /* THEN must do something.  */
  if (forwarder_block_p (then_bb))
    return false;

  num_possible_if_blocks++;
  if (dump_file)
    fprintf (dump_file,
	     "\nIF-CASE-1 found, start %d, then %d\n",
	     test_bb->index, then_bb->index);

  then_prob = then_edge->probability.invert ();

  /* We're speculating from the THEN path, we want to make sure the cost
     of speculation is within reason.  */
  if (! cheap_bb_rtx_cost_p (then_bb, then_prob,
	COSTS_N_INSNS (BRANCH_COST (optimize_bb_for_speed_p (then_edge->src),
				    predictable_edge_p (then_edge)))))
    return false;

  if (else_bb == EXIT_BLOCK_PTR_FOR_FN (cfun))
    {
      rtx_insn *jump = BB_END (else_edge->src);
      gcc_assert (JUMP_P (jump));
      else_target = JUMP_LABEL (jump);
    }

  /* Registers set are dead, or are predicable.  */
  if (! dead_or_predicable (test_bb, then_bb, else_bb,
			    single_succ_edge (then_bb), true))
    return false;

  /* Conversion went ok, including moving the insns and fixing up the
     jump.  Adjust the CFG to match.  */

  /* We can avoid creating a new basic block if then_bb is immediately
     followed by else_bb, i.e. deleting then_bb allows test_bb to fall
     through to else_bb.  */

  if (then_bb->next_bb == else_bb
      && then_bb->prev_bb == test_bb
      && else_bb != EXIT_BLOCK_PTR_FOR_FN (cfun))
    {
      redirect_edge_succ (FALLTHRU_EDGE (test_bb), else_bb);
      new_bb = 0;
    }
  else if (else_bb == EXIT_BLOCK_PTR_FOR_FN (cfun))
    new_bb = force_nonfallthru_and_redirect (FALLTHRU_EDGE (test_bb),
					     else_bb, else_target);
  else
    new_bb = redirect_edge_and_branch_force (FALLTHRU_EDGE (test_bb),
					     else_bb);

  df_set_bb_dirty (test_bb);
  df_set_bb_dirty (else_bb);

  then_bb_index = then_bb->index;
  delete_basic_block (then_bb);

  /* Make rest of code believe that the newly created block is the THEN_BB
     block we removed.  */
  if (new_bb)
    {
      df_bb_replace (then_bb_index, new_bb);
      /* This should have been done above via force_nonfallthru_and_redirect
         (possibly called from redirect_edge_and_branch_force).  */
      gcc_checking_assert (BB_PARTITION (new_bb) == BB_PARTITION (test_bb));
    }

  num_true_changes++;
  num_updated_if_blocks++;
  return true;
}

/* Test for case 2 above.  */

static bool
find_if_case_2 (basic_block test_bb, edge then_edge, edge else_edge)
{
  basic_block then_bb = then_edge->dest;
  basic_block else_bb = else_edge->dest;
  edge else_succ;
  profile_probability then_prob, else_prob;

  /* We do not want to speculate (empty) loop latches.  */
  if (current_loops
      && else_bb->loop_father->latch == else_bb)
    return false;

  /* If we are partitioning hot/cold basic blocks, we don't want to
     mess up unconditional or indirect jumps that cross between hot
     and cold sections.

     Basic block partitioning may result in some jumps that appear to
     be optimizable (or blocks that appear to be mergeable), but which really
     must be left untouched (they are required to make it safely across
     partition boundaries).  See  the comments at the top of
     bb-reorder.cc:partition_hot_cold_basic_blocks for complete details.  */

  if ((BB_END (then_bb)
       && JUMP_P (BB_END (then_bb))
       && CROSSING_JUMP_P (BB_END (then_bb)))
      || (JUMP_P (BB_END (test_bb))
	  && CROSSING_JUMP_P (BB_END (test_bb)))
      || (BB_END (else_bb)
	  && JUMP_P (BB_END (else_bb))
	  && CROSSING_JUMP_P (BB_END (else_bb))))
    return false;

  /* Verify test_bb ends in a conditional jump with no other side-effects.  */
  if (!onlyjump_p (BB_END (test_bb)))
    return false;

  /* ELSE has one successor.  */
  if (!single_succ_p (else_bb))
    return false;
  else
    else_succ = single_succ_edge (else_bb);

  /* ELSE outgoing edge is not complex.  */
  if (else_succ->flags & EDGE_COMPLEX)
    return false;

  /* ELSE has one predecessor.  */
  if (!single_pred_p (else_bb))
    return false;

  /* THEN is not EXIT.  */
  if (then_bb->index < NUM_FIXED_BLOCKS)
    return false;

  else_prob = else_edge->probability;
  then_prob = else_prob.invert ();

  /* ELSE is predicted or SUCC(ELSE) postdominates THEN.  */
  if (else_prob > then_prob)
    ;
  else if (else_succ->dest->index < NUM_FIXED_BLOCKS
	   || dominated_by_p (CDI_POST_DOMINATORS, then_bb,
			      else_succ->dest))
    ;
  else
    return false;

  num_possible_if_blocks++;
  if (dump_file)
    fprintf (dump_file,
	     "\nIF-CASE-2 found, start %d, else %d\n",
	     test_bb->index, else_bb->index);

  /* We're speculating from the ELSE path, we want to make sure the cost
     of speculation is within reason.  */
  if (! cheap_bb_rtx_cost_p (else_bb, else_prob,
	COSTS_N_INSNS (BRANCH_COST (optimize_bb_for_speed_p (else_edge->src),
				    predictable_edge_p (else_edge)))))
    return false;

  /* Registers set are dead, or are predicable.  */
  if (! dead_or_predicable (test_bb, else_bb, then_bb, else_succ, false))
    return false;

  /* Conversion went ok, including moving the insns and fixing up the
     jump.  Adjust the CFG to match.  */

  df_set_bb_dirty (test_bb);
  df_set_bb_dirty (then_bb);
  delete_basic_block (else_bb);

  num_true_changes++;
  num_updated_if_blocks++;

  /* ??? We may now fallthru from one of THEN's successors into a join
     block.  Rerun cleanup_cfg?  Examine things manually?  Wait?  */

  return true;
}

/* Used by the code above to perform the actual rtl transformations.
   Return TRUE if successful.

   TEST_BB is the block containing the conditional branch.  MERGE_BB
   is the block containing the code to manipulate.  DEST_EDGE is an
   edge representing a jump to the join block; after the conversion,
   TEST_BB should be branching to its destination.
   REVERSEP is true if the sense of the branch should be reversed.  */

static bool
dead_or_predicable (basic_block test_bb, basic_block merge_bb,
		    basic_block other_bb, edge dest_edge, bool reversep)
{
  basic_block new_dest = dest_edge->dest;
  rtx_insn *head, *end, *jump;
  rtx_insn *earliest = NULL;
  rtx old_dest;
  bitmap merge_set = NULL;
  /* Number of pending changes.  */
  int n_validated_changes = 0;
  rtx new_dest_label = NULL_RTX;

  jump = BB_END (test_bb);

  /* Find the extent of the real code in the merge block.  */
  head = BB_HEAD (merge_bb);
  end = BB_END (merge_bb);

  while (DEBUG_INSN_P (end) && end != head)
    end = PREV_INSN (end);

  /* If merge_bb ends with a tablejump, predicating/moving insn's
     into test_bb and then deleting merge_bb will result in the jumptable
     that follows merge_bb being removed along with merge_bb and then we
     get an unresolved reference to the jumptable.  */
  if (tablejump_p (end, NULL, NULL))
    return false;

  if (LABEL_P (head))
    head = NEXT_INSN (head);
  while (DEBUG_INSN_P (head) && head != end)
    head = NEXT_INSN (head);
  if (NOTE_P (head))
    {
      if (head == end)
	{
	  head = end = NULL;
	  goto no_body;
	}
      head = NEXT_INSN (head);
      while (DEBUG_INSN_P (head) && head != end)
	head = NEXT_INSN (head);
    }

  if (JUMP_P (end))
    {
      if (!onlyjump_p (end))
	return false;
      if (head == end)
	{
	  head = end = NULL;
	  goto no_body;
	}
      end = PREV_INSN (end);
      while (DEBUG_INSN_P (end) && end != head)
	end = PREV_INSN (end);
    }

  /* Don't move frame-related insn across the conditional branch.  This
     can lead to one of the paths of the branch having wrong unwind info.  */
  if (epilogue_completed)
    {
      rtx_insn *insn = head;
      while (1)
	{
	  if (INSN_P (insn) && RTX_FRAME_RELATED_P (insn))
	    return false;
	  if (insn == end)
	    break;
	  insn = NEXT_INSN (insn);
	}
    }

  /* Disable handling dead code by conditional execution if the machine needs
     to do anything funny with the tests, etc.  */
#ifndef IFCVT_MODIFY_TESTS
  if (targetm.have_conditional_execution ())
    {
      /* In the conditional execution case, we have things easy.  We know
	 the condition is reversible.  We don't have to check life info
	 because we're going to conditionally execute the code anyway.
	 All that's left is making sure the insns involved can actually
	 be predicated.  */

      rtx cond;

      /* If the conditional jump is more than just a conditional jump,
	 then we cannot do conditional execution conversion on this block.  */
      if (!onlyjump_p (jump))
	goto nce;

      cond = cond_exec_get_condition (jump);
      if (! cond)
	goto nce;

      rtx note = find_reg_note (jump, REG_BR_PROB, NULL_RTX);
      profile_probability prob_val
	  = (note ? profile_probability::from_reg_br_prob_note (XINT (note, 0))
	     : profile_probability::uninitialized ());

      if (reversep)
	{
	  enum rtx_code rev = reversed_comparison_code (cond, jump);
	  if (rev == UNKNOWN)
	    return false;
	  cond = gen_rtx_fmt_ee (rev, GET_MODE (cond), XEXP (cond, 0),
			         XEXP (cond, 1));
	  prob_val = prob_val.invert ();
	}

      if (cond_exec_process_insns (NULL, head, end, cond, prob_val, false)
	  && verify_changes (0))
	n_validated_changes = num_validated_changes ();
      else
	cancel_changes (0);

      earliest = jump;
    }
 nce:
#endif

  /* If we allocated new pseudos (e.g. in the conditional move
     expander called from noce_emit_cmove), we must resize the
     array first.  */
  if (max_regno < max_reg_num ())
    max_regno = max_reg_num ();

  /* Try the NCE path if the CE path did not result in any changes.  */
  if (n_validated_changes == 0)
    {
      rtx cond;
      rtx_insn *insn;
      regset live;
      bool success;

      /* In the non-conditional execution case, we have to verify that there
	 are no trapping operations, no calls, no references to memory, and
	 that any registers modified are dead at the branch site.  */

      if (!any_condjump_p (jump))
	return false;

      /* Find the extent of the conditional.  */
      cond = noce_get_condition (jump, &earliest, false);
      if (!cond)
	return false;

      live = BITMAP_ALLOC (&reg_obstack);
      simulate_backwards_to_point (merge_bb, live, end);
      success = can_move_insns_across (head, end, earliest, jump,
				       merge_bb, live,
				       df_get_live_in (other_bb), NULL);
      BITMAP_FREE (live);
      if (!success)
	return false;

      /* Collect the set of registers set in MERGE_BB.  */
      merge_set = BITMAP_ALLOC (&reg_obstack);

      FOR_BB_INSNS (merge_bb, insn)
	if (NONDEBUG_INSN_P (insn))
	  df_simulate_find_defs (insn, merge_set);

      /* If shrink-wrapping, disable this optimization when test_bb is
	 the first basic block and merge_bb exits.  The idea is to not
	 move code setting up a return register as that may clobber a
	 register used to pass function parameters, which then must be
	 saved in caller-saved regs.  A caller-saved reg requires the
	 prologue, killing a shrink-wrap opportunity.  */
      if ((SHRINK_WRAPPING_ENABLED && !epilogue_completed)
	  && ENTRY_BLOCK_PTR_FOR_FN (cfun)->next_bb == test_bb
	  && single_succ_p (new_dest)
	  && single_succ (new_dest) == EXIT_BLOCK_PTR_FOR_FN (cfun)
	  && bitmap_intersect_p (df_get_live_in (new_dest), merge_set))
	{
	  regset return_regs;
	  unsigned int i;

	  return_regs = BITMAP_ALLOC (&reg_obstack);

	  /* Start off with the intersection of regs used to pass
	     params and regs used to return values.  */
	  for (i = 0; i < FIRST_PSEUDO_REGISTER; i++)
	    if (FUNCTION_ARG_REGNO_P (i)
		&& targetm.calls.function_value_regno_p (i))
	      bitmap_set_bit (return_regs, INCOMING_REGNO (i));

	  bitmap_and_into (return_regs,
			   df_get_live_out (ENTRY_BLOCK_PTR_FOR_FN (cfun)));
	  bitmap_and_into (return_regs,
			   df_get_live_in (EXIT_BLOCK_PTR_FOR_FN (cfun)));
	  if (!bitmap_empty_p (return_regs))
	    {
	      FOR_BB_INSNS_REVERSE (new_dest, insn)
		if (NONDEBUG_INSN_P (insn))
		  {
		    df_ref def;

		    /* If this insn sets any reg in return_regs, add all
		       reg uses to the set of regs we're interested in.  */
		    FOR_EACH_INSN_DEF (def, insn)
		      if (bitmap_bit_p (return_regs, DF_REF_REGNO (def)))
			{
			  df_simulate_uses (insn, return_regs);
			  break;
			}
		  }
	      if (bitmap_intersect_p (merge_set, return_regs))
		{
		  BITMAP_FREE (return_regs);
		  BITMAP_FREE (merge_set);
		  return false;
		}
	    }
	  BITMAP_FREE (return_regs);
	}
    }

 no_body:
  /* We don't want to use normal invert_jump or redirect_jump because
     we don't want to delete_insn called.  Also, we want to do our own
     change group management.  */

  old_dest = JUMP_LABEL (jump);
  if (other_bb != new_dest)
    {
      if (!any_condjump_p (jump))
	goto cancel;

      if (JUMP_P (BB_END (dest_edge->src)))
	new_dest_label = JUMP_LABEL (BB_END (dest_edge->src));
      else if (new_dest == EXIT_BLOCK_PTR_FOR_FN (cfun))
	new_dest_label = ret_rtx;
      else
	new_dest_label = block_label (new_dest);

      rtx_jump_insn *jump_insn = as_a <rtx_jump_insn *> (jump);
      if (reversep
	  ? ! invert_jump_1 (jump_insn, new_dest_label)
	  : ! redirect_jump_1 (jump_insn, new_dest_label))
	goto cancel;
    }

  if (verify_changes (n_validated_changes))
    confirm_change_group ();
  else
    goto cancel;

  if (other_bb != new_dest)
    {
      redirect_jump_2 (as_a <rtx_jump_insn *> (jump), old_dest, new_dest_label,
		       0, reversep);

      redirect_edge_succ (BRANCH_EDGE (test_bb), new_dest);
      if (reversep)
	{
	  std::swap (BRANCH_EDGE (test_bb)->probability,
		     FALLTHRU_EDGE (test_bb)->probability);
	  update_br_prob_note (test_bb);
	}
    }

  /* Move the insns out of MERGE_BB to before the branch.  */
  if (head != NULL)
    {
      rtx_insn *insn;

      if (end == BB_END (merge_bb))
	BB_END (merge_bb) = PREV_INSN (head);

      /* PR 21767: when moving insns above a conditional branch, the REG_EQUAL
	 notes being moved might become invalid.  */
      insn = head;
      do
	{
	  rtx note;

	  if (! INSN_P (insn))
	    continue;
	  note = find_reg_note (insn, REG_EQUAL, NULL_RTX);
	  if (! note)
	    continue;
	  remove_note (insn, note);
	} while (insn != end && (insn = NEXT_INSN (insn)));

      /* PR46315: when moving insns above a conditional branch, the REG_EQUAL
	 notes referring to the registers being set might become invalid.  */
      if (merge_set)
	{
	  unsigned i;
	  bitmap_iterator bi;

	  EXECUTE_IF_SET_IN_BITMAP (merge_set, 0, i, bi)
	    remove_reg_equal_equiv_notes_for_regno (i);

	  BITMAP_FREE (merge_set);
	}

      reorder_insns (head, end, PREV_INSN (earliest));
    }

  /* Remove the jump and edge if we can.  */
  if (other_bb == new_dest)
    {
      delete_insn (jump);
      remove_edge (BRANCH_EDGE (test_bb));
      /* ??? Can't merge blocks here, as then_bb is still in use.
	 At minimum, the merge will get done just before bb-reorder.  */
    }

  return true;

 cancel:
  cancel_changes (0);

  if (merge_set)
    BITMAP_FREE (merge_set);

  return false;
}

/* Main entry point for all if-conversion.  AFTER_COMBINE is true if
   we are after combine pass.  */

static void
if_convert (bool after_combine)
{
  basic_block bb;
  int pass;

  if (optimize == 1)
    {
      df_live_add_problem ();
      df_live_set_all_dirty ();
    }

  /* Record whether we are after combine pass.  */
  ifcvt_after_combine = after_combine;
  have_cbranchcc4 = (direct_optab_handler (cbranch_optab, CCmode)
		     != CODE_FOR_nothing);
  num_possible_if_blocks = 0;
  num_updated_if_blocks = 0;
  num_true_changes = 0;

  loop_optimizer_init (AVOID_CFG_MODIFICATIONS);
  mark_loop_exit_edges ();
  loop_optimizer_finalize ();
  free_dominance_info (CDI_DOMINATORS);

  /* Compute postdominators.  */
  calculate_dominance_info (CDI_POST_DOMINATORS);

  df_set_flags (DF_LR_RUN_DCE);

  /* Go through each of the basic blocks looking for things to convert.  If we
     have conditional execution, we make multiple passes to allow us to handle
     IF-THEN{-ELSE} blocks within other IF-THEN{-ELSE} blocks.  */
  pass = 0;
  do
    {
      df_analyze ();
      /* Only need to do dce on the first pass.  */
      df_clear_flags (DF_LR_RUN_DCE);
      cond_exec_changed_p = false;
      pass++;

#ifdef IFCVT_MULTIPLE_DUMPS
      if (dump_file && pass > 1)
	fprintf (dump_file, "\n\n========== Pass %d ==========\n", pass);
#endif

      FOR_EACH_BB_FN (bb, cfun)
	{
          basic_block new_bb;
          while (!df_get_bb_dirty (bb)
                 && (new_bb = find_if_header (bb, pass)) != NULL)
            bb = new_bb;
	}

#ifdef IFCVT_MULTIPLE_DUMPS
      if (dump_file && cond_exec_changed_p)
	print_rtl_with_bb (dump_file, get_insns (), dump_flags);
#endif
    }
  while (cond_exec_changed_p);

#ifdef IFCVT_MULTIPLE_DUMPS
  if (dump_file)
    fprintf (dump_file, "\n\n========== no more changes\n");
#endif

  free_dominance_info (CDI_POST_DOMINATORS);

  if (dump_file)
    fflush (dump_file);

  clear_aux_for_blocks ();

  /* If we allocated new pseudos, we must resize the array for sched1.  */
  if (max_regno < max_reg_num ())
    max_regno = max_reg_num ();

  /* Write the final stats.  */
  if (dump_file && num_possible_if_blocks > 0)
    {
      fprintf (dump_file,
	       "\n%d possible IF blocks searched.\n",
	       num_possible_if_blocks);
      fprintf (dump_file,
	       "%d IF blocks converted.\n",
	       num_updated_if_blocks);
      fprintf (dump_file,
	       "%d true changes made.\n\n\n",
	       num_true_changes);
    }

  if (optimize == 1)
    df_remove_problem (df_live);

  /* Some non-cold blocks may now be only reachable from cold blocks.
     Fix that up.  */
  fixup_partitions ();

  checking_verify_flow_info ();
}

/* If-conversion and CFG cleanup.  */
static void
rest_of_handle_if_conversion (void)
{
  int flags = 0;

  if (flag_if_conversion)
    {
      if (dump_file)
	{
	  dump_reg_info (dump_file);
	  dump_flow_info (dump_file, dump_flags);
	}
      cleanup_cfg (CLEANUP_EXPENSIVE);
      if_convert (false);
      if (num_updated_if_blocks)
	/* Get rid of any dead CC-related instructions.  */
	flags |= CLEANUP_FORCE_FAST_DCE;
    }

  cleanup_cfg (flags);
}

namespace {

const pass_data pass_data_rtl_ifcvt =
{
  RTL_PASS, /* type */
  "ce1", /* name */
  OPTGROUP_NONE, /* optinfo_flags */
  TV_IFCVT, /* tv_id */
  0, /* properties_required */
  0, /* properties_provided */
  0, /* properties_destroyed */
  0, /* todo_flags_start */
  TODO_df_finish, /* todo_flags_finish */
};

class pass_rtl_ifcvt : public rtl_opt_pass
{
public:
  pass_rtl_ifcvt (gcc::context *ctxt)
    : rtl_opt_pass (pass_data_rtl_ifcvt, ctxt)
  {}

  /* opt_pass methods: */
  bool gate (function *) final override
    {
      return (optimize > 0) && dbg_cnt (if_conversion);
    }

  unsigned int execute (function *) final override
    {
      rest_of_handle_if_conversion ();
      return 0;
    }

}; // class pass_rtl_ifcvt

} // anon namespace

rtl_opt_pass *
make_pass_rtl_ifcvt (gcc::context *ctxt)
{
  return new pass_rtl_ifcvt (ctxt);
}


/* Rerun if-conversion, as combine may have simplified things enough
   to now meet sequence length restrictions.  */

namespace {

const pass_data pass_data_if_after_combine =
{
  RTL_PASS, /* type */
  "ce2", /* name */
  OPTGROUP_NONE, /* optinfo_flags */
  TV_IFCVT, /* tv_id */
  0, /* properties_required */
  0, /* properties_provided */
  0, /* properties_destroyed */
  0, /* todo_flags_start */
  TODO_df_finish, /* todo_flags_finish */
};

class pass_if_after_combine : public rtl_opt_pass
{
public:
  pass_if_after_combine (gcc::context *ctxt)
    : rtl_opt_pass (pass_data_if_after_combine, ctxt)
  {}

  /* opt_pass methods: */
  bool gate (function *) final override
    {
      return optimize > 0 && flag_if_conversion
	&& dbg_cnt (if_after_combine);
    }

  unsigned int execute (function *) final override
    {
      if_convert (true);
      return 0;
    }

}; // class pass_if_after_combine

} // anon namespace

rtl_opt_pass *
make_pass_if_after_combine (gcc::context *ctxt)
{
  return new pass_if_after_combine (ctxt);
}


namespace {

const pass_data pass_data_if_after_reload =
{
  RTL_PASS, /* type */
  "ce3", /* name */
  OPTGROUP_NONE, /* optinfo_flags */
  TV_IFCVT2, /* tv_id */
  0, /* properties_required */
  0, /* properties_provided */
  0, /* properties_destroyed */
  0, /* todo_flags_start */
  TODO_df_finish, /* todo_flags_finish */
};

class pass_if_after_reload : public rtl_opt_pass
{
public:
  pass_if_after_reload (gcc::context *ctxt)
    : rtl_opt_pass (pass_data_if_after_reload, ctxt)
  {}

  /* opt_pass methods: */
  bool gate (function *) final override
    {
      return optimize > 0 && flag_if_conversion2
	&& dbg_cnt (if_after_reload);
    }

  unsigned int execute (function *) final override
    {
      if_convert (true);
      return 0;
    }

}; // class pass_if_after_reload

} // anon namespace

rtl_opt_pass *
make_pass_if_after_reload (gcc::context *ctxt)
{
  return new pass_if_after_reload (ctxt);
}
