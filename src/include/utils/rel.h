/*-------------------------------------------------------------------------
 *
 * rel.h
 *	  POSTGRES relation descriptor (a/k/a relcache entry) definitions.
 *
 *
 * Portions Copyright (c) 2005-2009, Greenplum inc.
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/rel.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REL_H
#define REL_H

#include "access/tupdesc.h"
#include "access/xlog.h"
#include "catalog/pg_appendonly.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_publication.h"
#include "fmgr.h"
#include "nodes/bitmapset.h"
#include "rewrite/prs2lock.h"
#include "storage/block.h"
#include "storage/relfilenode.h"
#include "storage/smgr.h"
#include "utils/relcache.h"
#include "utils/reltrigger.h"

#include "catalog/pg_am.h"


/*
 * LockRelId and LockInfo really belong to lmgr.h, but it's more convenient
 * to declare them here so we can have a LockInfoData field in a Relation.
 */

typedef struct LockRelId
{
	Oid			relId;			/* a relation identifier */
	Oid			dbId;			/* a database identifier */
} LockRelId;

typedef struct LockInfoData
{
	LockRelId	lockRelId;
} LockInfoData;

typedef LockInfoData *LockInfo;

/*
 * Here are the contents of a relation cache entry.
 */

typedef struct RelationData
{
	RelFileNode rd_node;		/* relation physical identifier */
	SMgrRelation rd_smgr;		/* cached file handle, or NULL */
	int			rd_refcnt;		/* reference count */
	BackendId	rd_backend;		/* owning backend id, if temporary relation */
	bool		rd_islocaltemp; /* rel is a temp rel of this session */
	bool		rd_isnailed;	/* rel is nailed in cache */
	bool		rd_isvalid;		/* relcache entry is valid */
	bool		rd_indexvalid;	/* is rd_indexlist valid? (also rd_pkindex and
								 * rd_replidindex) */
	bool		rd_statvalid;	/* is rd_statlist valid? */

	/*----------
	 * rd_createSubid is the ID of the highest subtransaction the rel has
	 * survived into; or zero if the rel was not created in the current top
	 * transaction.  This can be now be relied on, whereas previously it could
	 * be "forgotten" in earlier releases. Likewise, rd_newRelfilenodeSubid is
	 * the ID of the highest subtransaction the relfilenode change has
	 * survived into, or zero if not changed in the current transaction (or we
	 * have forgotten changing it). rd_newRelfilenodeSubid can be forgotten
	 * when a relation has multiple new relfilenodes within a single
	 * transaction, with one of them occurring in a subsequently aborted
	 * subtransaction, e.g.
	 *		BEGIN;
	 *		TRUNCATE t;
	 *		SAVEPOINT save;
	 *		TRUNCATE t;
	 *		ROLLBACK TO save;
	 *		-- rd_newRelfilenodeSubid is now forgotten
	 */
	SubTransactionId rd_createSubid;	/* rel was created in current xact */
	SubTransactionId rd_newRelfilenodeSubid;	/* new relfilenode assigned in
												 * current xact */

	Form_pg_class rd_rel;		/* RELATION tuple */
	TupleDesc	rd_att;			/* tuple descriptor */
	Oid			rd_id;			/* relation's object id */
	LockInfoData rd_lockInfo;	/* lock mgr's info for locking relation */
	RuleLock   *rd_rules;		/* rewrite rules */
	MemoryContext rd_rulescxt;	/* private memory cxt for rd_rules, if any */
	TriggerDesc *trigdesc;		/* Trigger info, or NULL if rel has none */
    struct GpPolicy *rd_cdbpolicy; /* Partitioning info if distributed rel */

	/* use "struct" here to avoid needing to include rowsecurity.h: */
	struct RowSecurityDesc *rd_rsdesc;	/* row security policies, or NULL */

	/* data managed by RelationGetFKeyList: */
	List	   *rd_fkeylist;	/* list of ForeignKeyCacheInfo (see below) */
	bool		rd_fkeyvalid;	/* true if list has been computed */

	/* data managed by RelationRetrievePartitionKey: */
	PartitionKey rd_partkey;	/* partition key, or NULL */
	MemoryContext rd_partkeycxt;	/* private context for rd_partkey, if any */

	/* data managed by RelationRetrievePartitionDesc: */
	PartitionDesc rd_partdesc;	/* partition descriptor, or NULL */
	MemoryContext rd_pdcxt;		/* private context for rd_partdesc, if any */

	/* data managed by RelationGetPartitionQual: */
	List	   *rd_partcheck;	/* partition CHECK quals */
	bool		rd_partcheckvalid;	/* true if list has been computed */
	MemoryContext rd_partcheckcxt;	/* private cxt for rd_partcheck, if any */

	/* data managed by RelationGetIndexList: */
	List	   *rd_indexlist;	/* list of OIDs of indexes on relation */
	Oid			rd_pkindex;		/* OID of primary key, if any */
	Oid			rd_replidindex; /* OID of replica identity index, if any */

	/* data managed by RelationGetStatExtList: */
	List	   *rd_statlist;	/* list of OIDs of extended stats */

	/* data managed by RelationGetIndexAttrBitmap: */
	Bitmapset  *rd_indexattr;	/* identifies columns used in indexes */
	Bitmapset  *rd_keyattr;		/* cols that can be ref'd by foreign keys */
	Bitmapset  *rd_pkattr;		/* cols included in primary key */
	Bitmapset  *rd_idattr;		/* included in replica identity index */

	PublicationActions *rd_pubactions;	/* publication actions */

	/*
	 * rd_options is set whenever rd_rel is loaded into the relcache entry.
	 * Note that you can NOT look into rd_rel for this data.  NULL means "use
	 * defaults".
	 */
	bytea	   *rd_options;		/* parsed pg_class.reloptions */

	/*
	 * Oid of the handler for this relation. For an index this is a function
	 * returning IndexAmRoutine, for table like relations a function returning
	 * TableAmRoutine.  This is stored separately from rd_indam, rd_tableam as
	 * its lookup requires syscache access, but during relcache bootstrap we
	 * need to be able to initialize rd_tableam without syscache lookups.
	 */
	Oid			rd_amhandler;	/* OID of index AM's handler function */

	/*
	 * Table access method.
	 */
	const struct TableAmRoutine *rd_tableam;

	/* These are non-NULL only for an index relation: */
	Form_pg_index rd_index;		/* pg_index tuple describing this index */
	/* use "struct" here to avoid needing to include htup.h: */
	struct HeapTupleData *rd_indextuple;	/* all of pg_index tuple */

	/*
	 * index access support info (used only for an index relation)
	 *
	 * Note: only default support procs for each opclass are cached, namely
	 * those with lefttype and righttype equal to the opclass's opcintype. The
	 * arrays are indexed by support function number, which is a sufficient
	 * identifier given that restriction.
	 */
	MemoryContext rd_indexcxt;	/* private memory cxt for this stuff */
	/* use "struct" here to avoid needing to include amapi.h: */
	struct IndexAmRoutine *rd_indam;	/* index AM's API struct */
	Oid		   *rd_opfamily;	/* OIDs of op families for each index col */
	Oid		   *rd_opcintype;	/* OIDs of opclass declared input data types */
	RegProcedure *rd_support;	/* OIDs of support procedures */
	FmgrInfo   *rd_supportinfo; /* lookup info for support procedures */
	int16	   *rd_indoption;	/* per-column AM-specific flags */
	List	   *rd_indexprs;	/* index expression trees, if any */
	List	   *rd_indpred;		/* index predicate tree, if any */
	Oid		   *rd_exclops;		/* OIDs of exclusion operators, if any */
	Oid		   *rd_exclprocs;	/* OIDs of exclusion ops' procs, if any */
	uint16	   *rd_exclstrats;	/* exclusion ops' strategy numbers, if any */
	Oid		   *rd_indcollation;	/* OIDs of index collations */

	/*
	 * rd_amcache is available for index and table AMs to cache private data
	 * about the relation.  This must be just a cache since it may get reset
	 * at any time (in particular, it will get reset by a relcache inval
	 * message for the relation).  If used, it must point to a single memory
	 * chunk palloc'd in CacheMemoryContext, or in rd_indexcxt for an index
	 * relation.  A relcache reset will include freeing that chunk and setting
	 * rd_amcache = NULL.
	 */
	void	   *rd_amcache;		/* available for use by index/table AM */

	/*
	 * AO table support info (used only for AO and AOCS relations)
	 */
	Form_pg_appendonly rd_appendonly;
	struct HeapTupleData *rd_aotuple;		/* all of pg_appendonly tuple */

	/*
	 * foreign-table support
	 *
	 * rd_fdwroutine must point to a single memory chunk palloc'd in
	 * CacheMemoryContext.  It will be freed and reset to NULL on a relcache
	 * reset.
	 */

	/* use "struct" here to avoid needing to include fdwapi.h: */
	struct FdwRoutine *rd_fdwroutine;	/* cached function pointers, or NULL */

	/*
	 * Hack for CLUSTER, rewriting ALTER TABLE, etc: when writing a new
	 * version of a table, we need to make any toast pointers inserted into it
	 * have the existing toast table's OID, not the OID of the transient toast
	 * table.  If rd_toastoid isn't InvalidOid, it is the OID to place in
	 * toast pointers inserted into this rel.  (Note it's set on the new
	 * version of the main heap, not the toast table itself.)  This also
	 * causes toast_save_datum() to try to preserve toast value OIDs.
	 */
	Oid			rd_toastoid;	/* Real TOAST table's OID, or InvalidOid */

	/* use "struct" here to avoid needing to include pgstat.h: */
	struct PgStat_TableStatus *pgstat_info; /* statistics collection area */

	struct TileDmlDescData *tileDmlDesc; /* for tile: insert, update, delete, fetch */
	struct TileFetchDescData *tileFetchDesc; /* for tile: insert, update, delete, fetch */

	struct CacheNode *cdbCache;
} RelationData;


/*
 * ForeignKeyCacheInfo
 *		Information the relcache can cache about foreign key constraints
 *
 * This is basically just an image of relevant columns from pg_constraint.
 * We make it a subclass of Node so that copyObject() can be used on a list
 * of these, but we also ensure it is a "flat" object without substructure,
 * so that list_free_deep() is sufficient to free such a list.
 * The per-FK-column arrays can be fixed-size because we allow at most
 * INDEX_MAX_KEYS columns in a foreign key constraint.
 *
 * Currently, we mostly cache fields of interest to the planner, but the set
 * of fields has already grown the constraint OID for other uses.
 */
typedef struct ForeignKeyCacheInfo
{
	NodeTag		type;
	Oid			conoid;			/* oid of the constraint itself */
	Oid			conrelid;		/* relation constrained by the foreign key */
	Oid			confrelid;		/* relation referenced by the foreign key */
	int			nkeys;			/* number of columns in the foreign key */
	/* these arrays each have nkeys valid entries: */
	AttrNumber	conkey[INDEX_MAX_KEYS]; /* cols in referencing table */
	AttrNumber	confkey[INDEX_MAX_KEYS];	/* cols in referenced table */
	Oid			conpfeqop[INDEX_MAX_KEYS];	/* PK = FK operator OIDs */
} ForeignKeyCacheInfo;


/*
 * StdRdOptions
 *		Standard contents of rd_options for heaps and generic indexes.
 *
 * RelationGetFillFactor() and RelationGetTargetPageFreeSpace() can only
 * be applied to relations that use this format or a superset for
 * private options data.
 */
 /* autovacuum-related reloptions. */
typedef struct AutoVacOpts
{
	bool		enabled;
	int			vacuum_threshold;
	int			analyze_threshold;
	int			vacuum_cost_limit;
	int			freeze_min_age;
	int			freeze_max_age;
	int			freeze_table_age;
	int			multixact_freeze_min_age;
	int			multixact_freeze_max_age;
	int			multixact_freeze_table_age;
	int			log_min_duration;
	float8		vacuum_cost_delay;
	float8		vacuum_scale_factor;
	float8		analyze_scale_factor;
} AutoVacOpts;

typedef struct StdRdOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			fillfactor;		/* page fill factor in percent (0..100) */
	/* fraction of newly inserted tuples prior to trigger index cleanup */
	float8		vacuum_cleanup_index_scale_factor;
	int			toast_tuple_target; /* target for tuple toasting */
	AutoVacOpts autovacuum;		/* autovacuum-related options */
	bool		user_catalog_table; /* use as an additional catalog relation */
	int			parallel_workers;	/* max number of parallel workers */
	bool		vacuum_index_cleanup;	/* enables index vacuuming and cleanup */
	bool		vacuum_truncate;	/* enables vacuum to truncate a relation */
	bool		analyze_hll_non_part_table; 		/* force hll statistics collection on relation */

	int			blocksize;		/* max varblock size (AO rels only) */
	int			compresslevel;  /* compression level (AO rels only) */
	char		compresstype[NAMEDATALEN]; /* compression type (AO rels only) */
	bool		checksum;		/* checksum (AO rels only) */
} StdRdOptions;

#define HEAP_MIN_FILLFACTOR			10
#define HEAP_DEFAULT_FILLFACTOR		100

/*
 * RelationGetToastTupleTarget
 *		Returns the relation's toast_tuple_target.  Note multiple eval of argument!
 */
#define RelationGetToastTupleTarget(relation, defaulttarg) \
	((relation)->rd_options ? \
	 ((StdRdOptions *) (relation)->rd_options)->toast_tuple_target : (defaulttarg))

/*
 * RelationGetFillFactor
 *		Returns the relation's fillfactor.  Note multiple eval of argument!
 */
#define RelationGetFillFactor(relation, defaultff) \
	((relation)->rd_options ? \
	 ((StdRdOptions *) (relation)->rd_options)->fillfactor : (defaultff))

/*
 * RelationGetTargetPageUsage
 *		Returns the relation's desired space usage per page in bytes.
 */
#define RelationGetTargetPageUsage(relation, defaultff) \
	(BLCKSZ * RelationGetFillFactor(relation, defaultff) / 100)

/*
 * RelationGetTargetPageFreeSpace
 *		Returns the relation's desired freespace per page in bytes.
 */
#define RelationGetTargetPageFreeSpace(relation, defaultff) \
	(BLCKSZ * (100 - RelationGetFillFactor(relation, defaultff)) / 100)

/*
 * RelationIsUsedAsCatalogTable
 *		Returns whether the relation should be treated as a catalog table
 *		from the pov of logical decoding.  Note multiple eval of argument!
 */
#define RelationIsUsedAsCatalogTable(relation)	\
	((relation)->rd_options && \
	 ((relation)->rd_rel->relkind == RELKIND_RELATION || \
	  (relation)->rd_rel->relkind == RELKIND_MATVIEW) ? \
	 ((StdRdOptions *) (relation)->rd_options)->user_catalog_table : false)

/*
 * RelationGetParallelWorkers
 *		Returns the relation's parallel_workers reloption setting.
 *		Note multiple eval of argument!
 */
#define RelationGetParallelWorkers(relation, defaultpw) \
	((relation)->rd_options ? \
	 ((StdRdOptions *) (relation)->rd_options)->parallel_workers : (defaultpw))


/*
 * ViewOptions
 *		Contents of rd_options for views
 */
typedef struct ViewOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	bool		security_barrier;
	int			check_option_offset;
} ViewOptions;

/*
 * RelationIsSecurityView
 *		Returns whether the relation is security view, or not.  Note multiple
 *		eval of argument!
 */
#define RelationIsSecurityView(relation)	\
	((relation)->rd_options ?				\
	 ((ViewOptions *) (relation)->rd_options)->security_barrier : false)

/*
 * RelationHasCheckOption
 *		Returns true if the relation is a view defined with either the local
 *		or the cascaded check option.  Note multiple eval of argument!
 */
#define RelationHasCheckOption(relation)									\
	((relation)->rd_options &&												\
	 ((ViewOptions *) (relation)->rd_options)->check_option_offset != 0)

/*
 * RelationHasLocalCheckOption
 *		Returns true if the relation is a view defined with the local check
 *		option.  Note multiple eval of argument!
 */
#define RelationHasLocalCheckOption(relation)								\
	((relation)->rd_options &&												\
	 ((ViewOptions *) (relation)->rd_options)->check_option_offset != 0 ?	\
	 strcmp((char *) (relation)->rd_options +								\
			((ViewOptions *) (relation)->rd_options)->check_option_offset,	\
			"local") == 0 : false)

/*
 * RelationHasCascadedCheckOption
 *		Returns true if the relation is a view defined with the cascaded check
 *		option.  Note multiple eval of argument!
 */
#define RelationHasCascadedCheckOption(relation)							\
	((relation)->rd_options &&												\
	 ((ViewOptions *) (relation)->rd_options)->check_option_offset != 0 ?	\
	 strcmp((char *) (relation)->rd_options +								\
			((ViewOptions *) (relation)->rd_options)->check_option_offset,	\
			"cascaded") == 0 : false)


/*
 * RelationIsValid
 *		True iff relation descriptor is valid.
 */
#define RelationIsValid(relation) PointerIsValid(relation)

#define InvalidRelation ((Relation) NULL)


#define RelationIsTile(relation) \
	((relation)->rd_amhandler == TILE_TABLE_AM_HANDLER_OID)

/*
 * CAUTION: this macro is a violation of the absraction that table AM and
 * index AM interfaces provide.  Use of this macro is discouraged.  If
 * table/index AM API falls short for your use case, consider enhancing the
 * interface.
 *
 */
#define RelationIsHeap(relation) \
	((relation)->rd_rel->relam == HEAP_TABLE_AM_OID)

/*
 * CAUTION: this macro is a violation of the absraction that table AM and
 * index AM interfaces provide.  Use of this macro is discouraged.  If
 * table/index AM API falls short for your use case, consider enhancing the
 * interface.
 *
 * RelationIsAoRows
 * 		True iff relation has append only storage with row orientation
 */
#define RelationIsAoRows(relation) \
	((relation)->rd_rel->relam == AO_ROW_TABLE_AM_OID)

#define RelationStorageIsAoRows(relation) \
	((relation)->rd_rel->relam == AO_ROW_TABLE_AM_OID && \
		(relation)->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)

/*
 * CAUTION: this macro is a violation of the absraction that table AM and
 * index AM interfaces provide.  Use of this macro is discouraged.  If
 * table/index AM API falls short for your use case, consider enhancing the
 * interface.
 *
 * RelationIsAoCols
 * 		True iff relation has append only storage with column orientation
 */
#define RelationIsAoCols(relation) \
	((relation)->rd_rel->relam == AO_COLUMN_TABLE_AM_OID)

#define RelationStorageIsAoCols(relation) \
	((relation)->rd_rel->relam == AO_COLUMN_TABLE_AM_OID && \
		(relation)->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)

/*
 * CAUTION: this macro is a violation of the absraction that table AM and
 * index AM interfaces provide.  Use of this macro is discouraged.  If
 * table/index AM API falls short for your use case, consider enhancing the
 * interface.
 *
 * RelationIsAppendOptimized
 * 		True iff relation has append only storage (can be row or column orientation)
 */
#define RelationIsAppendOptimized(relation) \
	(RelationIsAoRows(relation) || RelationIsAoCols(relation))

#define RelationStorageIsAO(relation) \
	((RelationIsAoRows(relation) || RelationIsAoCols(relation)) && \
		(relation)->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)

/*
 * RelationIsBitmapIndex
 *      True iff relation is a bitmap index
 */
#define RelationIsBitmapIndex(relation) \
	((bool)((relation)->rd_rel->relam == BITMAP_AM_OID))

/*
 * RelationHasReferenceCountZero
 *		True iff relation reference count is zero.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 */
#define RelationHasReferenceCountZero(relation) \
		((bool)((relation)->rd_refcnt == 0))

/*
 * RelationGetForm
 *		Returns pg_class tuple for a relation.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 */
#define RelationGetForm(relation) ((relation)->rd_rel)

/*
 * RelationGetRelid
 *		Returns the OID of the relation
 */
#define RelationGetRelid(relation) ((relation)->rd_id)

/*
 * RelationGetNumberOfAttributes
 *		Returns the total number of attributes in a relation.
 */
#define RelationGetNumberOfAttributes(relation) ((relation)->rd_rel->relnatts)

/*
 * IndexRelationGetNumberOfAttributes
 *		Returns the number of attributes in an index.
 */
#define IndexRelationGetNumberOfAttributes(relation) \
		((relation)->rd_index->indnatts)

/*
 * IndexRelationGetNumberOfKeyAttributes
 *		Returns the number of key attributes in an index.
 */
#define IndexRelationGetNumberOfKeyAttributes(relation) \
		((relation)->rd_index->indnkeyatts)

/*
 * RelationGetDescr
 *		Returns tuple descriptor for a relation.
 */
#define RelationGetDescr(relation) ((relation)->rd_att)

/*
 * RelationGetRelationName
 *		Returns the rel's name.
 *
 * Note that the name is only unique within the containing namespace.
 */
#define RelationGetRelationName(relation) \
	(NameStr((relation)->rd_rel->relname))

/*
 * RelationGetNamespace
 *		Returns the rel's namespace OID.
 */
#define RelationGetNamespace(relation) \
	((relation)->rd_rel->relnamespace)

/*
 * RelationIsMapped
 *		True if the relation uses the relfilenode map.  Note multiple eval
 *		of argument!
 */
#define RelationIsMapped(relation) \
	(RELKIND_HAS_STORAGE((relation)->rd_rel->relkind) && \
	 ((relation)->rd_rel->relfilenode == InvalidOid))

#ifndef FRONTEND
/*
 * RelationGetSmgr
 *		Returns smgr file handle for a relation, opening it if needed.
 *
 * Very little code is authorized to touch rel->rd_smgr directly.  Instead
 * use this function to fetch its value.
 *
 * Note: since a relcache flush can cause the file handle to be closed again,
 * it's unwise to hold onto the pointer returned by this function for any
 * long period.  Recommended practice is to just re-execute RelationGetSmgr
 * each time you need to access the SMgrRelation.  It's quite cheap in
 * comparison to whatever an smgr function is going to do.
 */
static inline SMgrRelation
RelationGetSmgr(Relation rel)
{
	if (unlikely(rel->rd_smgr == NULL))
		smgrsetowner(&(rel->rd_smgr), smgropen(rel->rd_node, rel->rd_backend));
	return rel->rd_smgr;
}
#endif							/* !FRONTEND */

/*
 * RelationOpenSmgr
 *		Open the relation at the smgr level, if not already done.
 *
 * XXX this is now deprecated, and should not be used in new code.
 * Instead, call RelationGetSmgr in place of fetching rd_smgr directly.
 */
#define RelationOpenSmgr(relation) \
	do { \
		if ((relation)->rd_smgr == NULL) \
			smgrsetowner(&((relation)->rd_smgr), \
						 smgropen((relation)->rd_node, \
								  (relation)->rd_backend)); \
	} while (0)

/*
 * RelationCloseSmgr
 *		Close the relation at the smgr level, if not already done.
 *
 * Note: smgrclose should unhook from owner pointer, hence the Assert.
 */
#define RelationCloseSmgr(relation) \
	do { \
		if ((relation)->rd_smgr != NULL) \
		{ \
			smgrclose((relation)->rd_smgr); \
			Assert((relation)->rd_smgr == NULL); \
		} \
	} while (0)

/*
 * RelationGetTargetBlock
 *		Fetch relation's current insertion target block.
 *
 * Returns InvalidBlockNumber if there is no current target block.  Note
 * that the target block status is discarded on any smgr-level invalidation,
 * so there's no need to re-open the smgr handle if it's not currently open.
 */
#define RelationGetTargetBlock(relation) \
	( (relation)->rd_smgr != NULL ? (relation)->rd_smgr->smgr_targblock : InvalidBlockNumber )

/*
 * RelationSetTargetBlock
 *		Set relation's current insertion target block.
 */
#define RelationSetTargetBlock(relation, targblock) \
	do { \
		RelationGetSmgr(relation)->smgr_targblock = (targblock); \
	} while (0)

/*
 * RelationNeedsWAL
 *		True if relation needs WAL.
 */
#define RelationNeedsWAL(relation) \
	((relation)->rd_rel->relpersistence == RELPERSISTENCE_PERMANENT)

/*
 * RelationUsesLocalBuffers
 *		True if relation's pages are stored in local buffers.
 */
#define RelationUsesLocalBuffers(relation) \
	((relation)->rd_rel->relpersistence == RELPERSISTENCE_TEMP)

/*
 * RelationUsesTempNamespace
 *		True if relation's catalog entries live in a private namespace.
 */
#define RelationUsesTempNamespace(relation) \
	((relation)->rd_rel->relpersistence == RELPERSISTENCE_TEMP)

/*
 * RELATION_IS_LOCAL
 *		If a rel is either temp or newly created in the current transaction,
 *		it can be assumed to be accessible only to the current backend.
 *		This is typically used to decide that we can skip acquiring locks.
 *
 * Beware of multiple eval of argument
 */
#define RELATION_IS_LOCAL(relation) \
	((relation)->rd_islocaltemp || \
	 (relation)->rd_createSubid != InvalidSubTransactionId)

/*
 * RELATION_IS_OTHER_TEMP
 *		Test for a temporary relation that belongs to some other session.
 *
 * Beware of multiple eval of argument
 */
#define RELATION_IS_OTHER_TEMP(relation) \
	((relation)->rd_rel->relpersistence == RELPERSISTENCE_TEMP && \
	 !(relation)->rd_islocaltemp)


/*
 * RelationIsScannable
 *		Currently can only be false for a materialized view which has not been
 *		populated by its query.  This is likely to get more complicated later,
 *		so use a macro which looks like a function.
 */
#define RelationIsScannable(relation) ((relation)->rd_rel->relispopulated)

/*
 * RelationIsPopulated
 *		Currently, we don't physically distinguish the "populated" and
 *		"scannable" properties of matviews, but that may change later.
 *		Hence, use the appropriate one of these macros in code tests.
 */
#define RelationIsPopulated(relation) ((relation)->rd_rel->relispopulated)

/*
 * RelationIsAccessibleInLogicalDecoding
 *		True if we need to log enough information to have access via
 *		decoding snapshot.
 */
#define RelationIsAccessibleInLogicalDecoding(relation) \
	(XLogLogicalInfoActive() && \
	 RelationNeedsWAL(relation) && \
	 (IsCatalogRelation(relation) || RelationIsUsedAsCatalogTable(relation)))

/*
 * RelationIsLogicallyLogged
 *		True if we need to log enough information to extract the data from the
 *		WAL stream.
 *
 * We don't log information for unlogged tables (since they don't WAL log
 * anyway) and for system tables (their content is hard to make sense of, and
 * it would complicate decoding slightly for little gain). Note that we *do*
 * log information for user defined catalog tables since they presumably are
 * interesting to the user...
 */
#define RelationIsLogicallyLogged(relation) \
	(XLogLogicalInfoActive() && \
	 RelationNeedsWAL(relation) && \
	 !IsCatalogRelation(relation))

/*
 * RelationGetPartitionKey
 *		Returns the PartitionKey of a relation
 *
 * GPDB: This should not be used anymore. It is superceded by
 * RelationRetrievePartionKey, and is kept around only to prevent ABI 
 * breakage. See comments on that function for more details.
 */
#define RelationGetPartitionKey(relation) ((relation)->rd_partkey)

/*
 * RelationGetPartitionDesc
 *		Returns partition descriptor for a relation.
 *
 * GPDB: This should not be used anymore. It is superceded by
 * RelationRetrievePartionDesc, and is kept around only to prevent ABI 
 * breakage. See comments on that function for more details.
 */
#define RelationGetPartitionDesc(relation) ((relation)->rd_partdesc)

/* routines in utils/cache/relcache.c */
extern void RelationIncrementReferenceCount(Relation rel);
extern void RelationDecrementReferenceCount(Relation rel);
extern List *RelationGetRepsetList(Relation rel);

#endif							/* REL_H */
