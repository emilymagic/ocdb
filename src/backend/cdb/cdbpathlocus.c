/*-------------------------------------------------------------------------
 *
 * cdbpathlocus.c
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	    src/backend/cdb/cdbpathlocus.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/gp_distribution_policy.h"	/* GpPolicy */
#include "cdb/cdbhash.h"
#include "cdb/cdbpullup.h"		/* cdbpullup_findEclassInTargetList() */
#include "nodes/makefuncs.h"	/* makeVar() */
#include "nodes/nodeFuncs.h"	/* exprType() and exprTypmod() */
#include "nodes/pathnodes.h"	/* RelOptInfo */
#include "nodes/plannodes.h"	/* Plan */
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h" /* contain_volatile_functions() */
#include "optimizer/pathnode.h" /* Path */
#include "optimizer/paths.h"	/* cdb_make_distkey_for_expr() */
#include "optimizer/tlist.h"	/* tlist_member() */
#include "parser/parsetree.h"	/* rt_fetch() */
#include "utils/lsyscache.h"

#include "cdb/cdbpath.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbpathlocus.h"	/* me */
#include "utils/dispatchcat.h"

static List *cdb_build_distribution_keys(PlannerInfo *root,
										 Index rti,
										 GpPolicy *policy);

/*
 * cdbpathlocus_equal
 *
 *    - Returns false if locus a or locus b is "strewn", meaning that it
 *      cannot be determined whether it is equal to another partitioned
 *      distribution.
 *
 *    - Returns true if a and b have the same 'locustype' and 'numsegments',
 *      and distribution keys that contain the same equivalence classes.
 *
 *    - Returns false otherwise.
 */
bool
cdbpathlocus_equal(CdbPathLocus a, CdbPathLocus b)
{
	ListCell   *acell;
	ListCell   *bcell;
	ListCell   *a_ec_cell;
	ListCell   *b_ec_cell;

	/* Unless a and b have the same numsegments the result is always false */
	if (CdbPathLocus_NumSegments(a) !=
		CdbPathLocus_NumSegments(b))
		return false;

	if (CdbPathLocus_IsStrewn(a) ||
		CdbPathLocus_IsStrewn(b))
		return false;

	if (CdbPathLocus_IsEqual(a, b))
		return true;

	if (a.distkey == NIL ||
		b.distkey == NIL ||
		list_length(a.distkey) != list_length(b.distkey))
		return false;

	if ((CdbPathLocus_IsHashed(a) || CdbPathLocus_IsHashedOJ(a)) &&
		(CdbPathLocus_IsHashed(b) || CdbPathLocus_IsHashedOJ(b)))
	{
		forboth(acell, a.distkey, bcell, b.distkey)
		{
			DistributionKey *adistkey = (DistributionKey *) lfirst(acell);
			DistributionKey *bdistkey = (DistributionKey *) lfirst(bcell);

			if (adistkey->dk_opfamily != bdistkey->dk_opfamily)
				return false;

			foreach(b_ec_cell, bdistkey->dk_eclasses)
			{
				EquivalenceClass *b_ec = (EquivalenceClass *) lfirst(b_ec_cell);

				if (!list_member_ptr(adistkey->dk_eclasses, b_ec))
					return false;
			}
			foreach(a_ec_cell, adistkey->dk_eclasses)
			{
				EquivalenceClass *a_ec = (EquivalenceClass *) lfirst(a_ec_cell);

				if (!list_member_ptr(bdistkey->dk_eclasses, a_ec))
					return false;
			}
		}
		return true;
	}

	Assert(false);
	return false;
}								/* cdbpathlocus_equal */

/*
 * cdb_build_distribution_keys
 *	  Build DistributionKeys that match the policy of the given relation.
 */
List *
cdb_build_distribution_keys(PlannerInfo *root, Index rti, GpPolicy *policy)
{
	RangeTblEntry *rte = planner_rt_fetch(rti, root);
	List	   *retval = NIL;
	int			i;

	for (i = 0; i < policy->nattrs; ++i)
	{
		DistributionKey *cdistkey;

		/* Find or create a Var node that references the specified column. */
		Var		   *expr;
		Oid			typeoid;
		int32		type_mod;
		Oid			varcollid;
		Oid			eqopoid;
		Oid			opfamily = get_opclass_family(policy->opclasses[i]);
		Oid			opcintype = get_opclass_input_type(policy->opclasses[i]);
		List	   *mergeopfamilies;
		EquivalenceClass *eclass;

		get_atttypetypmodcoll(rte->relid, policy->attrs[i], &typeoid, &type_mod, &varcollid);
		expr = makeVar(rti, policy->attrs[i], typeoid, type_mod, varcollid, 0);

		/*
		 * Look up the equality operator corresponding to the distribution
		 * opclass.
		 */
		eqopoid = get_opfamily_member(opfamily, opcintype, opcintype, 1);

		/*
		 * Get Oid of the sort operator that would be used for a sort-merge
		 * equijoin on a pair of exprs of the same type.
		 */
		if (eqopoid == InvalidOid || !op_mergejoinable(eqopoid, typeoid))
		{
			/*
			 * It's in principle possible that there is no b-tree operator family
			 * that's compatible with the hash opclass's equality operator. However,
			 * we cannot construct an EquivalenceClass without the b-tree operator
			 * family, and therefore cannot build a DistributionKey to represent it.
			 * Bail out. (That makes the distribution key rather useless.)
			 */
			return NIL;
		}

		mergeopfamilies = get_mergejoin_opfamilies(eqopoid);

		eclass = get_eclass_for_sort_expr(root, (Expr *) expr,
										  NULL, /* nullable_relids */ /* GPDB_94_MERGE_FIXME: is NULL ok here? */
										  mergeopfamilies,
										  opcintype,
										  exprCollation((Node *) expr),
										  0,
										  bms_make_singleton(rti),
										  true);

		/* Create a distribution key for it. */
		cdistkey = makeNode(DistributionKey);
		cdistkey->dk_opfamily = opfamily;
		cdistkey->dk_eclasses = list_make1(eclass);

		retval = lappend(retval, cdistkey);
	}

	return retval;
}

/*
 * cdbpathlocus_for_insert
 *	  Build DistributionKeys that match the policy of the given relation.
 *
 * This is used for INSERT or split UPDATE, where 'pathtarget' the target list of
 * subpath that's producing the rows to be inserted/updated.
 *
 * As a side-effect, this assigns sortgrouprefs to any volatile expressions
 * that are used in the distribution keys.
 *
 * If the target table is distributed, but the distribution keys cannot be
 * represented as Equivalence Classes (because a datatype is missing merge
 * opfamilies), returns a NULL locus.
 *
 * Currently, this function is only invoked  by `create_motion_path_for_insert`
 * and `create_split_update_path`, and under the condition that policy type
 * is always partitioned type.
 */
CdbPathLocus
cdbpathlocus_for_insert(PlannerInfo *root, GpPolicy *policy,
						PathTarget *pathtarget)
{
	CdbPathLocus targetLocus;

	Assert(policy->ptype == POLICYTYPE_PARTITIONED);

	/* rows are distributed by hashing on specified columns */
	List	   *distkeys = NIL;
	Index		maxRef = 0;
	bool		failed = false;

	for (int i = 0; i < list_length(pathtarget->exprs); i++)
		maxRef = Max(maxRef, pathtarget->sortgrouprefs[i]);

	for (int i = 0; i < policy->nattrs; ++i)
	{
		AttrNumber	attno = policy->attrs[i];
		DistributionKey *cdistkey;
		Expr	   *expr;
		Oid			typeoid;
		Oid			eqopoid;
		Oid			opfamily = get_opclass_family(policy->opclasses[i]);
		Oid			opcintype = get_opclass_input_type(policy->opclasses[i]);
		List	   *mergeopfamilies;
		EquivalenceClass *eclass;

		expr = list_nth(pathtarget->exprs, attno - 1);
		typeoid = exprType((Node *) expr);

		/*
		 * Look up the equality operator corresponding to the distribution
		 * opclass.
		 */
		eqopoid = get_opfamily_member(opfamily, opcintype, opcintype, 1);

		if (pathtarget->sortgrouprefs[attno - 1] == 0 &&
			contain_volatile_functions((Node *) expr))
		{
			/*
			 * sortgrouprefs should never be zero if the expression is volatile!
			 */
			pathtarget->sortgrouprefs[attno - 1] = ++maxRef;
		}

		/*
		 * Get Oid of the sort operator that would be used for a sort-merge
		 * equijoin on a pair of exprs of the same type.
		 */
		if (failed || eqopoid == InvalidOid || !op_mergejoinable(eqopoid, typeoid))
		{
			/*
			 * It's in principle possible that there is no b-tree operator family
			 * that's compatible with the hash opclass's equality operator. However,
			 * we cannot construct an EquivalenceClass without the b-tree operator
			 * family, and therefore cannot build a DistributionKey to represent it.
			 * Bail out. (That makes the distribution key rather useless.)
			 */
			failed = true;
			continue;
		}

		mergeopfamilies = get_mergejoin_opfamilies(eqopoid);

		eclass = get_eclass_for_sort_expr(root, (Expr *) expr,
										  NULL, /* nullable_relids */ /* GPDB_94_MERGE_FIXME: is NULL ok here? */
										  mergeopfamilies,
										  opcintype,
										  exprCollation((Node *) expr),
										  pathtarget->sortgrouprefs[attno - 1],
										  NULL,
										  true);

		/* Create a distribution key for it. */
		cdistkey = makeNode(DistributionKey);
		cdistkey->dk_opfamily = opfamily;
		cdistkey->dk_eclasses = list_make1(eclass);

		distkeys = lappend(distkeys, cdistkey);
	}

	if (failed)
	{
		CdbPathLocus_MakeNull(&targetLocus);
	}
	else if (distkeys)
		CdbPathLocus_MakeHashed(&targetLocus, distkeys, policy->numsegments);
	else
	{
			/* DISTRIBUTED RANDOMLY */
		CdbPathLocus_MakeStrewn(&targetLocus, policy->numsegments);
	}

	return targetLocus;
}

/*
 * cdbpathlocus_from_policy
 *
 * Returns a locus describing the distribution of a policy
 */
CdbPathLocus
cdbpathlocus_from_policy(struct PlannerInfo *root, Index rti, GpPolicy *policy)
{
	CdbPathLocus result;

	if (Gp_role != GP_ROLE_DISPATCH || isInTrigger)
	{
		CdbPathLocus_MakeEntry(&result);
		return result;
	}

	if (GpPolicyIsPartitioned(policy))
	{
		/* Are the rows distributed by hashing on specified columns? */
		if (policy->nattrs > 0)
		{
			List	   *distkeys = cdb_build_distribution_keys(root,
															   rti,
															   policy);

			if (distkeys)
				CdbPathLocus_MakeHashed(&result, distkeys, policy->numsegments);
			else
			{
				/*
				 * It's possible that we fail to build a DistributionKey
				 * representation for the distribution policy.
				 */
				CdbPathLocus_MakeStrewn(&result, policy->numsegments);
			}
		}

		/* Rows are distributed on an unknown criterion (uniformly, we hope!) */
		else
			CdbPathLocus_MakeStrewn(&result, policy->numsegments);
	}
	else if (GpPolicyIsReplicated(policy))
	{
		CdbPathLocus_MakeSegmentGeneral(&result, policy->numsegments);
	}
	/* Normal catalog access */
	else
		CdbPathLocus_MakeEntry(&result);

	return result;
}								/* cdbpathlocus_from_baserel */

/*
 * cdbpathlocus_from_baserel
 *
 * Returns a locus describing the distribution of a base relation.
 */
CdbPathLocus
cdbpathlocus_from_baserel(struct PlannerInfo *root,
						  struct RelOptInfo *rel)
{
	return cdbpathlocus_from_policy(root, rel->relid, rel->cdbpolicy);
}								/* cdbpathlocus_from_baserel */


/*
 * cdbpathlocus_from_exprs
 *
 * Returns a locus specifying hashed distribution on a list of exprs.
 */
CdbPathLocus
cdbpathlocus_from_exprs(struct PlannerInfo *root,
						RelOptInfo *rel,
						List *hash_on_exprs,
						List *hash_opfamilies,
						List *hash_sortrefs,
						int numsegments)
{
	CdbPathLocus locus;
	List	   *distkeys = NIL;
	ListCell   *le, *lof, *lsr;

	forthree(le, hash_on_exprs, lof, hash_opfamilies, lsr, hash_sortrefs)
	{
		Node	   *expr = (Node *) lfirst(le);
		Oid			opfamily = lfirst_oid(lof);
		int			sortref = lfirst_int(lsr);
		DistributionKey *distkey;

		distkey = cdb_make_distkey_for_expr(root, rel, expr, opfamily, sortref);
		distkeys = lappend(distkeys, distkey);
	}

	CdbPathLocus_MakeHashed(&locus, distkeys, numsegments);
	return locus;
}								/* cdbpathlocus_from_exprs */

/*
 * cdbpathlocus_from_subquery
 *
 * Returns a locus describing the distribution of a subquery.
 * The subquery plan should have been generated already.
 *
 * 'subqplan' is the subquery plan.
 * 'subqrelid' is the subquery RTE index in the current query level, for
 *      building Var nodes that reference the subquery's result columns.
 */
CdbPathLocus
cdbpathlocus_from_subquery(struct PlannerInfo *root,
						   RelOptInfo *rel,
						   Path *subpath)
{
	CdbPathLocus locus;

	if (CdbPathLocus_IsHashed(subpath->locus) ||
		CdbPathLocus_IsHashedOJ(subpath->locus))
	{
		bool		failed = false;
		List	   *distkeys = NIL;
		int			numsegments = subpath->locus.numsegments;
		ListCell   *dk_cell;
		List	   *usable_subtlist = NIL;
		List	   *new_vars = NIL;
		ListCell   *lc;
		ListCell   *lc2 = NULL;
		RelOptInfo *parentrel = NULL;

		/*
		 * If the subquery we're pulling up is a child of an append rel,
		 * the pathkey should refer to the parent rel's Vars, not the child.
		 * Normally, the planner puts the parent and child expressions in an
		 * equivalence class for any potentially useful expressions, but that's
		 * done earlier in the planning already.
		 */
		if (rel->reloptkind == RELOPT_OTHER_MEMBER_REL)
		{
			if (root->append_rel_array == NULL || root->append_rel_array[rel->relid] == NULL) 
				elog(ERROR, "child rel %u not found in append_rel_array", rel->relid);

			/* Direct looking for AppendRelInfos by relid in the append_rel_array */
			AppendRelInfo *appendrel = root->append_rel_array[rel->relid];
			Index parent_relid = appendrel->parent_relid;
			parentrel = root->simple_rel_array[parent_relid];
			Assert(list_length(parentrel->reltarget->exprs) == list_length(rel->reltarget->exprs));
		}

		if (parentrel)
			lc2 = list_head(parentrel->reltarget->exprs);
		foreach (lc, rel->reltarget->exprs)
		{
			Expr	   *expr = (Expr *) lfirst(lc);
			Var		   *var;
			Expr	   *subexpr;
			Expr	   *parentexpr = NULL;

			if (parentrel)
			{
				parentexpr = lfirst(lc2);
				lc2 = lnext(lc2);
			}

			if (!IsA(expr, Var))
				continue;
			var = (Var *) expr;

			/* ignore whole-row vars */
			if (var->varattno == 0)
				continue;

			subexpr = list_nth(subpath->pathtarget->exprs, var->varattno - 1);
			usable_subtlist = lappend(usable_subtlist,
									  makeTargetEntry(subexpr,
													  list_length(usable_subtlist) + 1,
													  NULL,
													  false));
			new_vars = lappend(new_vars, parentrel ? parentexpr : expr);
		}

		foreach (dk_cell, subpath->locus.distkey)
		{
			DistributionKey *sub_dk = (DistributionKey *) lfirst(dk_cell);
			ListCell *ec_cell;
			DistributionKey *outer_dk = NULL;

			foreach (ec_cell, sub_dk->dk_eclasses)
			{
				EquivalenceClass *sub_ec = (EquivalenceClass *) lfirst(ec_cell);
				EquivalenceClass *outer_ec;

				outer_ec = cdb_pull_up_eclass(root,
											  sub_ec,
											  rel->relids,
											  usable_subtlist,
											  new_vars,
											  -1 /* not used */);
				if (outer_ec)
				{
					outer_dk = makeNode(DistributionKey);
					outer_dk->dk_eclasses = list_make1(outer_ec);
					outer_dk->dk_opfamily = sub_dk->dk_opfamily;
					break;
				}
			}

			if (outer_dk == NULL)
			{
				failed = true;
				break;
			}
			distkeys = lappend(distkeys, outer_dk);
		}

		if (failed)
			CdbPathLocus_MakeStrewn(&locus, numsegments);
		else if (CdbPathLocus_IsHashed(subpath->locus))
			CdbPathLocus_MakeHashed(&locus, distkeys, numsegments);
		else
		{
			Assert(CdbPathLocus_IsHashedOJ(subpath->locus));
			CdbPathLocus_MakeHashedOJ(&locus, distkeys, numsegments);
		}
	}
	else
		locus = subpath->locus;
	return locus;
}								/* cdbpathlocus_from_subquery */


/*
 * cdbpathlocus_get_distkey_exprs
 *
 * Returns a List with one Expr for each distkey column.  Each item either is
 * in the given targetlist, or has no Var nodes that are not in the targetlist;
 * and uses only rels in the given set of relids.  Returns NIL if the
 * distkey cannot be expressed in terms of the given relids and targetlist.
 */
void
cdbpathlocus_get_distkey_exprs(CdbPathLocus locus,
							   Bitmapset *relids,
							   List *targetlist,
							   List **exprs_p,
							   List **opfamilies_p)
{
	List	   *result_exprs = NIL;
	List	   *result_opfamilies = NIL;
	ListCell   *distkeycell;

	Assert(cdbpathlocus_is_valid(locus));

	*exprs_p = NIL;
	*opfamilies_p = NIL;

	if (CdbPathLocus_IsHashed(locus) || CdbPathLocus_IsHashedOJ(locus))
	{
		foreach(distkeycell, locus.distkey)
		{
			DistributionKey *distkey = (DistributionKey *) lfirst(distkeycell);
			ListCell   *ec_cell;
			Expr	   *item = NULL;

			foreach(ec_cell, distkey->dk_eclasses)
			{
				EquivalenceClass *dk_eclass = (EquivalenceClass *) lfirst(ec_cell);

				item = cdbpullup_findEclassInTargetList(dk_eclass, targetlist,
														distkey->dk_opfamily);

				if (item)
					break;
			}
			if (!item)
				return;

			result_exprs = lappend(result_exprs, item);
			result_opfamilies = lappend_oid(result_opfamilies, distkey->dk_opfamily);
		}
		*exprs_p = result_exprs;
		*opfamilies_p = result_opfamilies;
	}
}								/* cdbpathlocus_get_distkey_exprs */


/*
 * cdbpathlocus_pull_above_projection
 *
 * Given a targetlist, and a locus evaluable before the projection is
 * applied, returns an equivalent locus evaluable after the projection.
 * Returns a strewn locus if the necessary inputs are not projected.
 *
 * 'relids' is the set of relids that may occur in the targetlist exprs.
 * 'targetlist' specifies the projection.  It is a List of TargetEntry
 *      or merely a List of Expr.
 * 'newvarlist' is an optional List of Expr, in 1-1 correspondence with
 *      'targetlist'.  If specified, instead of creating a Var node to
 *      reference a targetlist item, we plug in a copy of the corresponding
 *      newvarlist item.
 * 'newrelid' is the RTE index of the projected result, for finding or
 *      building Var nodes that reference the projected columns.
 *      Ignored if 'newvarlist' is specified.
 */
CdbPathLocus
cdbpathlocus_pull_above_projection(struct PlannerInfo *root,
								   CdbPathLocus locus,
								   Bitmapset *relids,
								   List *targetlist,
								   List *newvarlist,
								   Index newrelid)
{
	CdbPathLocus newlocus;

	Assert(cdbpathlocus_is_valid(locus));

	if (CdbPathLocus_IsHashed(locus) || CdbPathLocus_IsHashedOJ(locus))
	{
		ListCell   *distkeycell;
		List	   *newdistkeys = NIL;
		int			numsegments;

		/*
		 * Keep the numsegments unchanged.
		 */
		numsegments = CdbPathLocus_NumSegments(locus);

		/* For each column of the distribution key... */
		foreach(distkeycell, locus.distkey)
		{
			DistributionKey *olddistkey = (DistributionKey *) lfirst(distkeycell);
			DistributionKey *newdistkey = NULL;

			/* Get DistributionKey for key expr rewritten in terms of projection cols. */
			ListCell   *ec_cell;
			EquivalenceClass *new_ec = NULL;

			foreach(ec_cell, olddistkey->dk_eclasses)
			{
				EquivalenceClass *old_ec = (EquivalenceClass *) lfirst(ec_cell);

				new_ec = cdb_pull_up_eclass(root,
											old_ec,
											relids,
											targetlist,
											newvarlist,
											newrelid);
				if (new_ec)
					break;
			}

			/*
			 * NB: Targetlist might include columns from both sides of outer
			 * join "=" comparison, in which case cdb_pull_up_distribution_keys might
			 * succeed on keys from more than one distkeylist. The
			 * pulled-up locus could then be a HashedOJ locus, perhaps saving
			 * a Motion when an outer join is followed by UNION ALL followed
			 * by a join or aggregate.  For now, don't bother.
			 */

			/*
			 * Fail if can't evaluate distribution key in the context of this
			 * targetlist.
			 */
			if (!new_ec)
			{
				CdbPathLocus_MakeStrewn(&newlocus, numsegments);
				return newlocus;
			}

			newdistkey = makeNode(DistributionKey);
			newdistkey->dk_eclasses = list_make1(new_ec);
			newdistkey->dk_opfamily = olddistkey->dk_opfamily;

			/* Assemble new distkeys. */
			newdistkeys = lappend(newdistkeys, newdistkey);
		}

		/* Build new locus. */
		if (CdbPathLocus_IsHashed(locus))
			CdbPathLocus_MakeHashed(&newlocus, newdistkeys, numsegments);
		else
			CdbPathLocus_MakeHashedOJ(&newlocus, newdistkeys, numsegments);
		return newlocus;
	}
	else
		return locus;
}								/* cdbpathlocus_pull_above_projection */


/*
 * cdbpathlocus_join
 *
 * Determine locus to describe the result of a join.  Any necessary Motion has
 * already been applied to the sources.
 */
CdbPathLocus
cdbpathlocus_join(JoinType jointype, CdbPathLocus a, CdbPathLocus b)
{
	ListCell   *acell;
	ListCell   *bcell;
	CdbPathLocus resultlocus = {0};
	int			numsegments;

	Assert(cdbpathlocus_is_valid(a));
	Assert(cdbpathlocus_is_valid(b));

	/* Do both input rels have same locus? */
	if (cdbpathlocus_equal(a, b))
		return a;

	/*
	 * SingleQE may have different segment counts.
	 */
	if (CdbPathLocus_IsSingleQE(a) &&
		CdbPathLocus_IsSingleQE(b))
	{
		CdbPathLocus_MakeSingleQE(&resultlocus,
								  CdbPathLocus_CommonSegments(a, b));
		return resultlocus;
	}

	if (CdbPathLocus_IsGeneral(a))
		return b;

	if (CdbPathLocus_IsGeneral(b))
		return a;

	/*
	 * If one rel is replicated, result stays with the other rel,
	 * but need to ensure the result is on the common segments.
	 */
	if (CdbPathLocus_IsReplicated(a))
	{
		b.numsegments = CdbPathLocus_CommonSegments(a, b);
		return b;
	}
	if (CdbPathLocus_IsReplicated(b))
	{
		a.numsegments = CdbPathLocus_CommonSegments(a, b);
		return a;
	}

	/*
	 * If one rel is segmentgeneral, result stays with the other rel,
	 * but need to ensure the result is on the common segments.
	 *
	 * NB: the code check SegmentGeneral and replicated is quite similar,
	 * but we have to put check-segmentgeneral below. Consider one
	 * is segmentgeneral and the other is replicated, only by this order
	 * we can be sure that this function never return a locus of
	 * Replicated.
	 * update a replicated table join with a partitioned locus table will
	 * reach here.
	 */

	if (CdbPathLocus_IsSegmentGeneral(a))
	{
		b.numsegments = CdbPathLocus_CommonSegments(a, b);
		return b;
	}
	if (CdbPathLocus_IsSegmentGeneral(b))
	{
		a.numsegments = CdbPathLocus_CommonSegments(a, b);
		return a;
	}

	/*
	 * Both sides must be Hashed (or HashedOJ), then. And the distribution
	 * keys should be compatible; otherwise the caller should not be building
	 * a join directly between these two rels (a Motion would be needed).
	 */
	if (!(CdbPathLocus_IsHashed(a) || CdbPathLocus_IsHashedOJ(a)))
		elog(ERROR, "could not be construct join with non-hashed path");
	if (!(CdbPathLocus_IsHashed(b) || CdbPathLocus_IsHashedOJ(b)))
		elog(ERROR, "could not be construct join with non-hashed path");
	if (a.distkey == NIL ||
		list_length(a.distkey) != list_length(b.distkey))
		elog(ERROR, "could not construct hashed join locus with incompatible distribution keys");
	if (CdbPathLocus_NumSegments(a) != CdbPathLocus_NumSegments(b))
		elog(ERROR, "could not construct hashed join locus with different number of segments");
	numsegments = CdbPathLocus_NumSegments(a);

	/*
	 * For a LEFT/RIGHT OUTER JOIN, we can use key of the outer, non-nullable
	 * side as is. There should not be any more joins with the nullable side
	 * above this join rel, so the inner side's keys are not interesting above
	 * this.
	 */
	if (jointype == JOIN_LEFT ||
		jointype == JOIN_LASJ_NOTIN ||
		jointype == JOIN_ANTI)
	{
		resultlocus = a;
	}
	else if (jointype == JOIN_RIGHT)
	{
		resultlocus = b;
	}
	else
	{
		/*
		 * Not a LEFT/RIGHT JOIN. We don't usually get here with INNER JOINs
		 * either, because if you have an INNER JOIN on a equality predicate,
		 * they should form an EquivalenceClass, so that the distribution keys
		 * on both sides of the join refer to the same EquivalenceClass, and
		 * we exit already at the top of this function, at the
		 * "if(cdbpathlocus_equal(a, b)" test. The usual case that we get here
		 * is a FULL JOIN.
		 *
		 * I'm not sure what non-FULL corner cases there are that lead here.
		 * But it's safe to create a HashedOJ locus for them, anyway, because
		 * the promise of a HashedOJ is weaker than Hashed.
		 */

		/* Zip the two distkey lists together to make a HashedOJ locus. */
		List	   *newdistkeys = NIL;

		forboth(acell, a.distkey, bcell, b.distkey)
		{
			DistributionKey *adistkey = (DistributionKey *) lfirst(acell);
			DistributionKey *bdistkey = (DistributionKey *) lfirst(bcell);
			DistributionKey *newdistkey;

			Assert(adistkey->dk_opfamily == bdistkey->dk_opfamily);

			newdistkey = makeNode(DistributionKey);
			newdistkey->dk_eclasses = list_union_ptr(adistkey->dk_eclasses,
													 bdistkey->dk_eclasses);
			newdistkey->dk_opfamily = adistkey->dk_opfamily;

			newdistkeys = lappend(newdistkeys, newdistkey);
		}
		CdbPathLocus_MakeHashedOJ(&resultlocus, newdistkeys, numsegments);
	}
	Assert(cdbpathlocus_is_valid(resultlocus));
	return resultlocus;
}								/* cdbpathlocus_join */

/*
 * cdbpathlocus_is_hashed_on_exprs
 *
 * This function tests whether grouping on a given set of exprs can be done
 * in place without motion.
 *
 * For a hashed locus, returns false if the distkey has a column whose
 * equivalence class contains no expr belonging to the given list.
 *
 * If 'ignore_constants' is true, any constants in the locus are ignored.
 */
bool
cdbpathlocus_is_hashed_on_exprs(CdbPathLocus locus, List *exprlist,
								   bool ignore_constants)
{
	ListCell   *distkeycell;

	Assert(cdbpathlocus_is_valid(locus));

	if (CdbPathLocus_IsHashed(locus) || CdbPathLocus_IsHashedOJ(locus))
	{
		foreach(distkeycell, locus.distkey)
		{
			DistributionKey *distkey = (DistributionKey *) lfirst(distkeycell);
			ListCell   *distkeyeccell;
			bool		found = false;

			foreach(distkeyeccell, distkey->dk_eclasses)
			{
				/* Does some expr in distkey match some item in exprlist? */
				EquivalenceClass *dk_eclass = (EquivalenceClass *) lfirst(distkeyeccell);
				ListCell   *i;

				if (ignore_constants && CdbEquivClassIsConstant(dk_eclass))
				{
					found = true;
					break;
				}

				foreach(i, dk_eclass->ec_members)
				{
					EquivalenceMember *em = (EquivalenceMember *) lfirst(i);

					if (list_member(exprlist, em->em_expr))
					{
						found = true;
						break;
					}
				}
				if (found)
					break;
			}
			if (!found)
				return false;
		}
		/* Every column of the distkey contains an expr in exprlist. */
		return true;
	}
	else
		return !CdbPathLocus_IsStrewn(locus);
}								/* cdbpathlocus_is_hashed_on_exprs */

/*
 * cdbpathlocus_is_hashed_on_eclasses
 *
 * This function tests whether grouping on a given set of exprs can be done
 * in place without motion.
 *
 * For a hashed locus, returns false if the distribution key has any column whose
 * equivalence class is not in 'eclasses' list.
 *
 * If 'ignore_constants' is true, any constants in the locus are ignored.
 */
bool
cdbpathlocus_is_hashed_on_eclasses(CdbPathLocus locus, List *eclasses,
								   bool ignore_constants)
{
	ListCell   *distkeycell;

	Assert(cdbpathlocus_is_valid(locus));

	if (CdbPathLocus_IsHashed(locus) || CdbPathLocus_IsHashedOJ(locus))
	{
		foreach(distkeycell, locus.distkey)
		{
			DistributionKey *distkey = (DistributionKey *) lfirst(distkeycell);
			ListCell   *distkeyeccell;
			bool		found = false;

			foreach(distkeyeccell, distkey->dk_eclasses)
			{
				/* Does distkey have an eclass that's not in 'eclasses'? */
				EquivalenceClass *dk_ec = (EquivalenceClass *) lfirst(distkeyeccell);
				ListCell   *eccell;

				while (dk_ec->ec_merged != NULL)
					dk_ec = dk_ec->ec_merged;

				if (ignore_constants && CdbEquivClassIsConstant(dk_ec))
				{
					found = true;
					break;
				}

				foreach(eccell, eclasses)
				{
					EquivalenceClass *ec = (EquivalenceClass *) lfirst(eccell);

					while (ec->ec_merged != NULL)
						ec = ec->ec_merged;

					if (ec == dk_ec)
					{
						found = true;
						break;
					}
				}
				if (found)
					break;
			}
			if (!found)
				return false;
		}
		/* Every column of the distribution key contains an expr in exprlist. */
		return true;
	}
	else
		return !CdbPathLocus_IsStrewn(locus);
}								/* cdbpathlocus_is_hashed_on_exprs */

/*
 * cdbpathlocus_is_hashed_on_tlist
 *
 * This function tests whether grouping on a given set of exprs can be done
 * in place without motion.
 *
 * For a hashed locus, returns false if the distkey has a column whose
 * equivalence class contains no expr belonging to the given list.
 *
 * If 'ignore_constants' is true, any constants in the locus are ignored.
 */
bool
cdbpathlocus_is_hashed_on_tlist(CdbPathLocus locus, List *tlist,
								bool ignore_constants)
{
	ListCell   *distkeycell;

	Assert(cdbpathlocus_is_valid(locus));

	if (CdbPathLocus_IsHashed(locus) || CdbPathLocus_IsHashedOJ(locus))
	{
		foreach(distkeycell, locus.distkey)
		{
			DistributionKey *distkey = (DistributionKey *) lfirst(distkeycell);
			ListCell   *distkeyeccell;

			foreach(distkeyeccell, distkey->dk_eclasses)
			{
				/* Does some expr in distkey match some item in exprlist? */
				EquivalenceClass *dk_eclass = (EquivalenceClass *) lfirst(distkeyeccell);
				ListCell   *i;
				bool		found = false;

				if (ignore_constants && CdbEquivClassIsConstant(dk_eclass))
					continue;

				if (dk_eclass->ec_sortref != 0)
				{
					foreach(i, tlist)
					{
						TargetEntry *tle = (TargetEntry *) lfirst(i);

						if (tle->ressortgroupref == dk_eclass->ec_sortref)
						{
							found = true;
							break;
						}
					}
				}
				else
				{
					foreach(i, dk_eclass->ec_members)
					{
						EquivalenceMember *em = (EquivalenceMember *) lfirst(i);
						ListCell *ltl;

						foreach(ltl, tlist)
						{
							TargetEntry *tle = (TargetEntry *) lfirst(ltl);

							if (equal(tle->expr, em->em_expr))
							{
								found = true;
								break;
							}
						}
						if (found)
							break;
					}
				}
				if (!found)
					return false;
			}
		}
		/* Every column of the distkey contains an expr in exprlist. */
		return true;
	}
	else
		return !CdbPathLocus_IsStrewn(locus);
}

/*
 * cdbpathlocus_is_hashed_on_relids
 *
 * Used when a subquery predicate (such as "x IN (SELECT ...)") has been
 * flattened into a join with the tables of the current query.  A row of
 * the cross-product of the current query's tables might join with more
 * than one row from the subquery and thus be duplicated.  Removing such
 * duplicates after the join is called deduping.  If a row's duplicates
 * might occur in more than one partition, a Motion operator will be
 * needed to bring them together.  This function tests whether the join
 * result (whose locus is given) can be deduped in place without motion.
 *
 * For a hashed locus, returns false if the distkey has a column whose
 * equivalence class contains no Var node belonging to the given set of
 * relids.  Caller should specify the relids of the non-subquery tables.
 */
bool
cdbpathlocus_is_hashed_on_relids(CdbPathLocus locus, Bitmapset *relids)
{
	Assert(cdbpathlocus_is_valid(locus));

	if (CdbPathLocus_IsHashed(locus) || CdbPathLocus_IsHashedOJ(locus))
	{
		ListCell   *lc1;

		foreach(lc1, locus.distkey)
		{
			DistributionKey *distkey = (DistributionKey *) lfirst(lc1);
			bool		found = false;
			ListCell   *lc2;

			foreach(lc2, distkey->dk_eclasses)
			{
				/* Does distribution key contain a Var whose varno is in relids? */
				EquivalenceClass *dk_eclass = (EquivalenceClass *) lfirst(lc2);
				ListCell   *lc3;

				foreach(lc3, dk_eclass->ec_members)
				{
					EquivalenceMember *em = (EquivalenceMember *) lfirst(lc3);

					if (IsA(em->em_expr, Var) && bms_is_subset(em->em_relids, relids))
					{
						found = true;
						break;
					}
				}
				if (found)
					break;
			}
			if (!found)
				return false;
		}

		/*
		 * Every column of the distkey contains a Var whose varno is in
		 * relids.
		 */
		return true;
	}
	else
		return !CdbPathLocus_IsStrewn(locus);
}								/* cdbpathlocus_is_hashed_on_relids */


/*
 * cdbpathlocus_is_valid
 *
 * Returns true if locus appears structurally valid.
 */
bool
cdbpathlocus_is_valid(CdbPathLocus locus)
{
	ListCell   *distkeycell;

	if (!CdbLocusType_IsValid(locus.locustype))
		goto bad;

	if (!CdbPathLocus_IsHashed(locus) && !CdbPathLocus_IsHashedOJ(locus) && locus.distkey != NIL)
		goto bad;

	if (CdbPathLocus_IsHashed(locus) || CdbPathLocus_IsHashedOJ(locus))
	{
		if (locus.distkey == NIL)
			goto bad;
		if (!IsA(locus.distkey, List))
			goto bad;
		foreach(distkeycell, locus.distkey)
		{
			DistributionKey *item = (DistributionKey *) lfirst(distkeycell);

			if (!item || !IsA(item, DistributionKey))
				goto bad;

			if (!OidIsValid(item->dk_opfamily))
				goto bad;
		}
	}

	/* Check that 'numsegments' is valid */
	if (!CdbPathLocus_IsGeneral(locus) &&
		!CdbPathLocus_IsEntry(locus) &&
		!CdbPathLocus_IsOuterQuery(locus) &&
		locus.numsegments == -1)
		goto bad;

	return true;

bad:
	return false;
}								/* cdbpathlocus_is_valid */
