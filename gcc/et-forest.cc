/* ET-trees data structure implementation.
   Contributed by Pavel Nejedly
   Copyright (C) 2002-2025 Free Software Foundation, Inc.

This file is part of the libiberty library.
Libiberty is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 3 of the License, or (at your option) any later version.

Libiberty is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public
License along with libiberty; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.

  The ET-forest structure is described in:
    D. D. Sleator and R. E. Tarjan. A data structure for dynamic trees.
    J.  G'omput. System Sci., 26(3):362 381, 1983.
*/

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "alloc-pool.h"
#include "et-forest.h"
#include "selftest.h"

/* We do not enable this with CHECKING_P, since it is awfully slow.  */
#undef DEBUG_ET

#ifdef DEBUG_ET
#include "backend.h"
#include "hard-reg-set.h"
#endif

/* The occurrence of a node in the et tree.  */
struct et_occ
{
  struct et_node *of;		/* The node.  */

  struct et_occ *parent;	/* Parent in the splay-tree.  */
  struct et_occ *prev;		/* Left son in the splay-tree.  */
  struct et_occ *next;		/* Right son in the splay-tree.  */

  int depth;			/* The depth of the node is the sum of depth
				   fields on the path to the root.  */
  int min;			/* The minimum value of the depth in the subtree
				   is obtained by adding sum of depth fields
				   on the path to the root.  */
  struct et_occ *min_occ;	/* The occurrence in the subtree with the minimal
				   depth.  */
};

static object_allocator<et_node> et_nodes ("et_nodes pool");
static object_allocator<et_occ> et_occurrences ("et_occ pool");

/* Changes depth of OCC to D.  */

static inline void
set_depth (struct et_occ *occ, int d)
{
  if (!occ)
    return;

  occ->min += d - occ->depth;
  occ->depth = d;
}

/* Adds D to the depth of OCC.  */

static inline void
set_depth_add (struct et_occ *occ, int d)
{
  if (!occ)
    return;

  occ->min += d;
  occ->depth += d;
}

/* Sets prev field of OCC to P.  */

static inline void
set_prev (struct et_occ *occ, struct et_occ *t)
{
#ifdef DEBUG_ET
  gcc_assert (occ != t);
#endif

  occ->prev = t;
  if (t)
    t->parent = occ;
}

/* Sets next field of OCC to P.  */

static inline void
set_next (struct et_occ *occ, struct et_occ *t)
{
#ifdef DEBUG_ET
  gcc_assert (occ != t);
#endif

  occ->next = t;
  if (t)
    t->parent = occ;
}

/* Recompute minimum for occurrence OCC.  */

static inline void
et_recomp_min (struct et_occ *occ)
{
  struct et_occ *mson = occ->prev;

  if (!mson
      || (occ->next
	  && mson->min > occ->next->min))
      mson = occ->next;

  if (mson && mson->min < 0)
    {
      occ->min = mson->min + occ->depth;
      occ->min_occ = mson->min_occ;
    }
  else
    {
      occ->min = occ->depth;
      occ->min_occ = occ;
    }
}

#ifdef DEBUG_ET
/* Checks whether neighborhood of OCC seems sane.  */

static void
et_check_occ_sanity (struct et_occ *occ)
{
  if (!occ)
    return;

  gcc_assert (occ->parent != occ);
  gcc_assert (occ->prev != occ);
  gcc_assert (occ->next != occ);
  gcc_assert (!occ->next || occ->next != occ->prev);

  if (occ->next)
    {
      gcc_assert (occ->next != occ->parent);
      gcc_assert (occ->next->parent == occ);
    }

  if (occ->prev)
    {
      gcc_assert (occ->prev != occ->parent);
      gcc_assert (occ->prev->parent == occ);
    }

  gcc_assert (!occ->parent
	      || occ->parent->prev == occ
	      || occ->parent->next == occ);
}

/* Checks whether tree rooted at OCC is sane.  */

static void
et_check_sanity (struct et_occ *occ)
{
  et_check_occ_sanity (occ);
  if (occ->prev)
    et_check_sanity (occ->prev);
  if (occ->next)
    et_check_sanity (occ->next);
}

/* Checks whether tree containing OCC is sane.  */

static void
et_check_tree_sanity (struct et_occ *occ)
{
  while (occ->parent)
    occ = occ->parent;

  et_check_sanity (occ);
}

/* For recording the paths.  */

/* An ad-hoc constant; if the function has more blocks, this won't work,
   but since it is used for debugging only, it does not matter.  */
#define MAX_NODES 100000

static int len;
static void *datas[MAX_NODES];
static int depths[MAX_NODES];

/* Records the path represented by OCC, with depth incremented by DEPTH.  */

static int
record_path_before_1 (struct et_occ *occ, int depth)
{
  int mn, m;

  depth += occ->depth;
  mn = depth;

  if (occ->prev)
    {
      m = record_path_before_1 (occ->prev, depth);
      if (m < mn)
	mn = m;
    }

  fprintf (stderr, "%d (%d); ", ((basic_block) occ->of->data)->index, depth);

  gcc_assert (len < MAX_NODES);

  depths[len] = depth;
  datas[len] = occ->of;
  len++;

  if (occ->next)
    {
      m = record_path_before_1 (occ->next, depth);
      if (m < mn)
	mn = m;
    }

  gcc_assert (mn == occ->min + depth - occ->depth);

  return mn;
}

/* Records the path represented by a tree containing OCC.  */

static void
record_path_before (struct et_occ *occ)
{
  while (occ->parent)
    occ = occ->parent;

  len = 0;
  record_path_before_1 (occ, 0);
  fprintf (stderr, "\n");
}

/* Checks whether the path represented by OCC, with depth incremented by DEPTH,
   was not changed since the last recording.  */

static int
check_path_after_1 (struct et_occ *occ, int depth)
{
  int mn, m;

  depth += occ->depth;
  mn = depth;

  if (occ->next)
    {
      m = check_path_after_1 (occ->next, depth);
      if (m < mn)
	mn =  m;
    }

  len--;
  gcc_assert (depths[len] == depth && datas[len] == occ->of);

  if (occ->prev)
    {
      m = check_path_after_1 (occ->prev, depth);
      if (m < mn)
	mn =  m;
    }

  gcc_assert (mn == occ->min + depth - occ->depth);

  return mn;
}

/* Checks whether the path represented by a tree containing OCC was
   not changed since the last recording.  */

static void
check_path_after (struct et_occ *occ)
{
  while (occ->parent)
    occ = occ->parent;

  check_path_after_1 (occ, 0);
  gcc_assert (!len);
}

#endif

/* Splay the occurrence OCC to the root of the tree.  */

static void
et_splay (struct et_occ *occ)
{
  struct et_occ *f, *gf, *ggf;
  int occ_depth, f_depth, gf_depth;

#ifdef DEBUG_ET
  record_path_before (occ);
  et_check_tree_sanity (occ);
#endif

  while (occ->parent)
    {
      occ_depth = occ->depth;

      f = occ->parent;
      f_depth = f->depth;

      gf = f->parent;

      if (!gf)
	{
	  set_depth_add (occ, f_depth);
	  occ->min_occ = f->min_occ;
	  occ->min = f->min;

	  if (f->prev == occ)
	    {
	      /* zig */
	      set_prev (f, occ->next);
	      set_next (occ, f);
	      set_depth_add (f->prev, occ_depth);
	    }
	  else
	    {
	      /* zag */
	      set_next (f, occ->prev);
	      set_prev (occ, f);
	      set_depth_add (f->next, occ_depth);
	    }
	  set_depth (f, -occ_depth);
	  occ->parent = NULL;

	  et_recomp_min (f);
#ifdef DEBUG_ET
	  et_check_tree_sanity (occ);
	  check_path_after (occ);
#endif
	  return;
	}

      gf_depth = gf->depth;

      set_depth_add (occ, f_depth + gf_depth);
      occ->min_occ = gf->min_occ;
      occ->min = gf->min;

      ggf = gf->parent;

      if (gf->prev == f)
	{
	  if (f->prev == occ)
	    {
	      /* zig zig */
	      set_prev (gf, f->next);
	      set_prev (f, occ->next);
	      set_next (occ, f);
	      set_next (f, gf);

	      set_depth (f, -occ_depth);
	      set_depth_add (f->prev, occ_depth);
	      set_depth (gf, -f_depth);
	      set_depth_add (gf->prev, f_depth);
	    }
	  else
	    {
	      /* zag zig */
	      set_prev (gf, occ->next);
	      set_next (f, occ->prev);
	      set_prev (occ, f);
	      set_next (occ, gf);

	      set_depth (f, -occ_depth);
	      set_depth_add (f->next, occ_depth);
	      set_depth (gf, -occ_depth - f_depth);
	      set_depth_add (gf->prev, occ_depth + f_depth);
	    }
	}
      else
	{
	  if (f->prev == occ)
	    {
	      /* zig zag */
	      set_next (gf, occ->prev);
	      set_prev (f, occ->next);
	      set_prev (occ, gf);
	      set_next (occ, f);

	      set_depth (f, -occ_depth);
	      set_depth_add (f->prev, occ_depth);
	      set_depth (gf, -occ_depth - f_depth);
	      set_depth_add (gf->next, occ_depth + f_depth);
	    }
	  else
	    {
	      /* zag zag */
	      set_next (gf, f->prev);
	      set_next (f, occ->prev);
	      set_prev (occ, f);
	      set_prev (f, gf);

	      set_depth (f, -occ_depth);
	      set_depth_add (f->next, occ_depth);
	      set_depth (gf, -f_depth);
	      set_depth_add (gf->next, f_depth);
	    }
	}

      occ->parent = ggf;
      if (ggf)
	{
	  if (ggf->prev == gf)
	    ggf->prev = occ;
	  else
	    ggf->next = occ;
	}

      et_recomp_min (gf);
      et_recomp_min (f);
#ifdef DEBUG_ET
      et_check_tree_sanity (occ);
#endif
    }

#ifdef DEBUG_ET
  et_check_sanity (occ);
  check_path_after (occ);
#endif
}

/* Create a new et tree occurrence of NODE.  */

static struct et_occ *
et_new_occ (struct et_node *node)
{
  et_occ *nw = et_occurrences.allocate ();

  nw->of = node;
  nw->parent = NULL;
  nw->prev = NULL;
  nw->next = NULL;

  nw->depth = 0;
  nw->min_occ = nw;
  nw->min = 0;

  return nw;
}

/* Create a new et tree containing DATA.  */

struct et_node *
et_new_tree (void *data)
{
  et_node *nw = et_nodes.allocate ();

  nw->data = data;
  nw->father = NULL;
  nw->left = NULL;
  nw->right = NULL;
  nw->son = NULL;

  nw->rightmost_occ = et_new_occ (nw);
  nw->parent_occ = NULL;

  return nw;
}

/* Releases et tree T.  */

void
et_free_tree (struct et_node *t)
{
  while (t->son)
    et_split (t->son);

  if (t->father)
    et_split (t);

  et_occurrences.remove (t->rightmost_occ);
  et_nodes.remove (t);
}

/* Releases et tree T without maintaining other nodes.  */

void
et_free_tree_force (struct et_node *t)
{
  et_occurrences.remove (t->rightmost_occ);
  if (t->parent_occ)
    et_occurrences.remove (t->parent_occ);
  et_nodes.remove (t);
}

/* Release the alloc pools, if they are empty.  */

void
et_free_pools (void)
{
  et_occurrences.release_if_empty ();
  et_nodes.release_if_empty ();
}

/* Sets father of et tree T to FATHER.  */

void
et_set_father (struct et_node *t, struct et_node *father)
{
  struct et_node *left, *right;
  struct et_occ *rmost, *left_part, *new_f_occ, *p;

  /* Update the path represented in the splay tree.  */
  new_f_occ = et_new_occ (father);

  rmost = father->rightmost_occ;
  et_splay (rmost);

  left_part = rmost->prev;

  p = t->rightmost_occ;
  et_splay (p);

  set_prev (new_f_occ, left_part);
  set_next (new_f_occ, p);

  p->depth++;
  p->min++;
  et_recomp_min (new_f_occ);

  set_prev (rmost, new_f_occ);

  if (new_f_occ->min + rmost->depth < rmost->min)
    {
      rmost->min = new_f_occ->min + rmost->depth;
      rmost->min_occ = new_f_occ->min_occ;
    }

  t->parent_occ = new_f_occ;

  /* Update the tree.  */
  t->father = father;
  right = father->son;
  if (right)
    left = right->left;
  else
    left = right = t;

  left->right = t;
  right->left = t;
  t->left = left;
  t->right = right;

  father->son = t;

#ifdef DEBUG_ET
  et_check_tree_sanity (rmost);
  record_path_before (rmost);
#endif
}

/* Splits the edge from T to its father.  */

void
et_split (struct et_node *t)
{
  struct et_node *father = t->father;
  struct et_occ *r, *l, *rmost, *p_occ;

  /* Update the path represented by the splay tree.  */
  rmost = t->rightmost_occ;
  et_splay (rmost);

  for (r = rmost->next; r->prev; r = r->prev)
    continue;
  et_splay (r);

  r->prev->parent = NULL;
  p_occ = t->parent_occ;
  et_splay (p_occ);
  t->parent_occ = NULL;

  l = p_occ->prev;
  p_occ->next->parent = NULL;

  set_prev (r, l);

  et_recomp_min (r);

  et_splay (rmost);
  rmost->depth = 0;
  rmost->min = 0;

  et_occurrences.remove (p_occ);

  /* Update the tree.  */
  if (father->son == t)
    father->son = t->right;
  if (father->son == t)
    father->son = NULL;
  else
    {
      t->left->right = t->right;
      t->right->left = t->left;
    }
  t->left = t->right = NULL;
  t->father = NULL;

#ifdef DEBUG_ET
  et_check_tree_sanity (rmost);
  record_path_before (rmost);

  et_check_tree_sanity (r);
  record_path_before (r);
#endif
}

/* Finds the nearest common ancestor of the nodes N1 and N2.  */

struct et_node *
et_nca (struct et_node *n1, struct et_node *n2)
{
  struct et_occ *o1 = n1->rightmost_occ, *o2 = n2->rightmost_occ, *om;
  struct et_occ *l, *r, *ret;
  int mn;

  if (n1 == n2)
    return n1;

  et_splay (o1);
  l = o1->prev;
  r = o1->next;
  if (l)
    l->parent = NULL;
  if (r)
    r->parent = NULL;
  et_splay (o2);

  if (l == o2 || (l && l->parent != NULL))
    {
      ret = o2->next;

      set_prev (o1, o2);
      if (r)
	r->parent = o1;
    }
  else if (r == o2 || (r && r->parent != NULL))
    {
      ret = o2->prev;

      set_next (o1, o2);
      if (l)
	l->parent = o1;
    }
  else
    {
      /* O1 and O2 are in different components of the forest.  */
      if (l)
	l->parent = o1;
      if (r)
	r->parent = o1;
      return NULL;
    }

  if (o2->depth > 0)
    {
      om = o1;
      mn = o1->depth;
    }
  else
    {
      om = o2;
      mn = o2->depth + o1->depth;
    }

#ifdef DEBUG_ET
  et_check_tree_sanity (o2);
#endif

  if (ret && ret->min + o1->depth + o2->depth < mn)
    return ret->min_occ->of;
  else
    return om->of;
}

/* Checks whether the node UP is an ancestor of the node DOWN.  */

bool
et_below (struct et_node *down, struct et_node *up)
{
  struct et_occ *u = up->rightmost_occ, *d = down->rightmost_occ;
  struct et_occ *l, *r;

  if (up == down)
    return true;

  et_splay (u);
  l = u->prev;
  r = u->next;

  if (!l)
    return false;

  l->parent = NULL;

  if (r)
    r->parent = NULL;

  et_splay (d);

  if (l == d || l->parent != NULL)
    {
      if (r)
	r->parent = u;
      set_prev (u, d);
#ifdef DEBUG_ET
      et_check_tree_sanity (u);
#endif
    }
  else
    {
      l->parent = u;

      /* In case O1 and O2 are in two different trees, we must just restore the
	 original state.  */
      if (r && r->parent != NULL)
	set_next (u, d);
      else
	set_next (u, r);

#ifdef DEBUG_ET
      et_check_tree_sanity (u);
#endif
      return false;
    }

  if (d->depth <= 0)
    return false;

  return !d->next || d->next->min + d->depth >= 0;
}

/* Returns the root of the tree that contains NODE.  */

struct et_node *
et_root (struct et_node *node)
{
  struct et_occ *occ = node->rightmost_occ, *r;

  /* The root of the tree corresponds to the rightmost occurrence in the
     represented path.  */
  et_splay (occ);
  for (r = occ; r->next; r = r->next)
    continue;
  et_splay (r);

  return r->of;
}

#if CHECKING_P

namespace selftest {

/* Selftests for et-forest.cc.  */

/* Perform sanity checks for a tree consisting of a single node.  */

static void
test_single_node ()
{
  void *test_data = (void *)0xcafebabe;

  et_node *n = et_new_tree (test_data);
  ASSERT_EQ (n->data, test_data);
  ASSERT_EQ (n, et_root (n));
  et_free_tree (n);
}

/* Test of this tree:
       a
      / \
     /   \
    b     c
   / \    |
  d   e   f.  */

static void
test_simple_tree ()
{
  et_node *a = et_new_tree (NULL);
  et_node *b = et_new_tree (NULL);
  et_node *c = et_new_tree (NULL);
  et_node *d = et_new_tree (NULL);
  et_node *e = et_new_tree (NULL);
  et_node *f = et_new_tree (NULL);

  et_set_father (b, a);
  et_set_father (c, a);
  et_set_father (d, b);
  et_set_father (e, b);
  et_set_father (f, c);

  ASSERT_TRUE (et_below (a, a));
  ASSERT_TRUE (et_below (b, a));
  ASSERT_TRUE (et_below (c, a));
  ASSERT_TRUE (et_below (d, a));
  ASSERT_TRUE (et_below (e, a));
  ASSERT_TRUE (et_below (f, a));

  ASSERT_FALSE (et_below (a, b));
  ASSERT_TRUE (et_below (b, b));
  ASSERT_FALSE (et_below (c, b));
  ASSERT_TRUE (et_below (d, b));
  ASSERT_TRUE (et_below (e, b));
  ASSERT_FALSE (et_below (f, b));

  ASSERT_FALSE (et_below (a, c));
  ASSERT_FALSE (et_below (b, c));
  ASSERT_TRUE (et_below (c, c));
  ASSERT_FALSE (et_below (d, c));
  ASSERT_FALSE (et_below (e, c));
  ASSERT_TRUE (et_below (f, c));

  ASSERT_FALSE (et_below (a, d));
  ASSERT_FALSE (et_below (b, d));
  ASSERT_FALSE (et_below (c, d));
  ASSERT_TRUE (et_below (d, d));
  ASSERT_FALSE (et_below (e, d));
  ASSERT_FALSE (et_below (f, d));

  ASSERT_FALSE (et_below (a, e));
  ASSERT_FALSE (et_below (b, e));
  ASSERT_FALSE (et_below (c, e));
  ASSERT_FALSE (et_below (d, e));
  ASSERT_TRUE (et_below (e, e));
  ASSERT_FALSE (et_below (f, e));

  ASSERT_FALSE (et_below (a, f));
  ASSERT_FALSE (et_below (b, f));
  ASSERT_FALSE (et_below (c, f));
  ASSERT_FALSE (et_below (d, f));
  ASSERT_FALSE (et_below (e, f));
  ASSERT_TRUE (et_below (f, f));

  et_free_tree_force (a);
}

/* Verify that two disconnected nodes are unrelated.  */

static void
test_disconnected_nodes ()
{
  et_node *a = et_new_tree (NULL);
  et_node *b = et_new_tree (NULL);

  ASSERT_FALSE (et_below (a, b));
  ASSERT_FALSE (et_below (b, a));

  et_free_tree (a);
  et_free_tree (b);
}

/* Run all of the selftests within this file.  */

void
et_forest_cc_tests ()
{
  test_single_node ();
  test_simple_tree ();
  test_disconnected_nodes ();
}

} // namespace selftest

#endif /* CHECKING_P */
