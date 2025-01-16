#ifndef CDBCATALOGFUNC_H
#define CDBCATALOGFUNC_H

#include "access/tileam.h"
#include "access/sdir.h"
#include "nodes/plannodes.h"
#include "tcop/utility.h"

typedef enum CsType
{
	CS_QUERY,
	CS_STARTUP,
	CS_CONF,
	CS_XACT_START,
	CS_XACT_FINISH,
	CS_XACT_ABORT,
	CS_MODIFY_TABLE,
	CS_NEXT_VAL
} CsType;

typedef enum CsRunType
{
	CS_RUN_UTILITY,
	CS_RUN_QD,
	CS_RUN_CATALOG,
	CS_RUN_SKIP
} CsRunType;

typedef struct CsQuery
{
	NodeTag	type;

	CsType cmdType;
	char *query_string;
	Node *data;
	int	cluster_id;
} CsQuery;

typedef struct NextValNode
{
	NodeTag	type;
	
	Oid		relid;
	int64	plast;
	int64	pcached;
	int64	pincrement;
	bool	poverflow;
} NextValNode;

typedef struct pg_conn PGconn;
typedef struct pg_result PGresult;

extern bool accessHeap;
extern bool accessTile;
extern PGconn	*csConn;
extern bool errorFromCatalogServer;

typedef struct CdbCatalogAuxNode CdbCatalogAuxNode;

/*
 * Catalog client side
 */
extern CdbCatalogAuxNode *cc_get_catalog(const char *sql, MemoryContext ctx);
extern void cc_conn(char *dbname);
extern void cc_finish(void);
extern void cc_xact_command(CsType csType);
extern void cc_send_modify_tabble(BlockListNode  *blockListNode);
extern void cc_next_val(Oid relid, int64 *plast, int64 *pcached, int64  *pincrement,
						bool *poverflow);
extern bytea *cc_get_conf(char *file);
extern void cc_run_on_catalog_server(const char *sql);
extern bytea *cc_get_returning(PGresult *res);
extern void cc_pass_returning(PGresult *res);
extern PGresult *cc_exec_plan(CsQuery *csQuery);
extern char *cc_status(PGresult *res);

extern CdbCatalogAuxNode *cc_catalog_or_run(const char *sql);
extern void DestReceiveBytea(char *data, int dataSize, DestReceiver *dest);

/*
 * Catalog server side
 */
extern CdbCatalogAuxNode *cs_get_catalog_from_sql(List *queryList, const char *sql,
												  char *command);
extern PlannedStmt *get_catalog_from_query(List *queries, const char *sql);
extern void cs_get_startup_catalog(DestReceiver *dest);
extern void cs_modify_table(BlockListNode *blockListNode);
extern void cs_next_val(NextValNode *nextVal, DestReceiver *dest);
extern void cs_get_conf(const char *path, DestReceiver *dest);


#endif //CDBCATALOGFUNC_H
