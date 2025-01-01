#include "postgres.h"

#include "access/tileam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/memoryheapam.h"
#include "catalog/pg_database.h"
#include "utils/pickcat.h"
#include "utils/dispatchcat.h"
#include "cdb/cdbcatalogfunc.h"
#include "cdb/cdbsrlz.h"
#include "commands/copy.h"
#include "commands/explain.h"
#include "commands/sequence.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq-int.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#ifndef WIN32
#include <unistd.h>
#else							/* WIN32 */
#include <io.h>
#include <win32.h>
#endif							/* WIN32 */

typedef struct CatConnectOptions
{
	bool		quiet;

	char	   *dbname;
	char	   *hostname;
	char	   *port;
	char	   *username;
} CatConnectOptions;


typedef struct CcPrintUp
{
	int				nattr;
	PGresAttDesc   *attDescs;
	MemoryContext	tmpcontext;
	bool			sendDescrip;
	StringInfoData	buf;
} CcPrintUp;

bool accessHeap = false;
bool accessTile = false;
PGconn	*csConn = NULL;
bool errorFromCatalogServer = false;

static bytea *cstring_to_bytea(char *inputText);
static CcPrintUp *cc_printup_create_DR(PGresAttDesc *attDescs, int nattr,
									   bool sendDescrip);
static void cc_printtup_startup(CcPrintUp *myState);
static bool cc_printtup(char **strs, CcPrintUp *myState);
static void cc_preintup_shutdown(CcPrintUp *myState);
static void cc_printup_destroy(CcPrintUp *myState);

static void
SetConnOptions(CatConnectOptions *options)
{
	options->dbname = NULL;
	options->hostname = NULL;
	options->port = "5432";
	options->username = NULL;

	options->quiet = false;

	options->dbname = MyDatabaseName;
}

static void
NoticeProcessor(void *arg, const char *message)
{
	char *subMsg;

	(void) arg;					/* not used */

	subMsg = palloc(strlen(message) + 1);
	if(strncmp(message, "NOTICE:  ", strlen("NOTICE:  ")) == 0)
	{
		strcpy(subMsg, message + strlen("NOTICE:  "));
		elog(NOTICE, "%s", subMsg);
	}
	else if(strncmp(message, "WARNING:  ", strlen("WARNING:  ")) == 0)
	{
		strcpy(subMsg, message + strlen("WARNING:  "));
		elog(WARNING, "%s", subMsg);
	}
	else if(strncmp(message, "INFO:  ", strlen("INFO:  ")) == 0)
	{
		strcpy(subMsg, message + strlen("INFO:  "));
		elog(INFO, "%s", subMsg);
	}
	else
		elog(NOTICE, "%s", message);
}

static void
ProccessErrorMessage(char *errMsg)
{
	char *subMsg;

	subMsg = palloc(strlen(errMsg) + 1);
	if(strncmp(errMsg, "ERROR:  ", strlen("ERROR:  ")) == 0)
	{
		strcpy(subMsg, errMsg + strlen("ERROR:  "));
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("%s", subMsg)));
	}
	else if (strncmp(errMsg, "PANIC:  ", strlen("PANIC:  ")) == 0)
	{
		strcpy(subMsg, errMsg + strlen("PANIC:  "));
		ereport(PANIC,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("%s", subMsg)));
	}
	else if (strncmp(errMsg, "FATAL:  ", strlen("FATAL:  ")) == 0)
	{
		strcpy(subMsg, errMsg + strlen("FATAL:  "));
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("%s", subMsg)));
	}
	else
	{
		errorFromCatalogServer = true;

		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("%s", errMsg)));
	}
}

/* establish connection with database. */
void
cc_conn(char *dbname)
{
	PGconn	   *conn;
	bool		have_password = false;
	char		password[100];
	bool		new_pass;
	CatConnectOptions my_opts;

	SetConnOptions(&my_opts);

	/*
	 * Start the connection.  Loop until we have a password if requested by
	 * backend.
	 */
	do
	{
#define PARAMS_ARRAY_SIZE	7

		const char *keywords[PARAMS_ARRAY_SIZE];
		const char *values[PARAMS_ARRAY_SIZE];

		keywords[0] = "host";
		values[0] = my_opts.hostname;
		keywords[1] = "port";
		values[1] = my_opts.port;
		keywords[2] = "user";
		values[2] = my_opts.username;
		keywords[3] = "password";
		values[3] = have_password ? password : NULL;
		keywords[4] = "dbname";
		if (dbname)
			values[4] = dbname;
		else
			values[4] = "postgres";
		keywords[5] = "fallback_application_name";
		values[5] = "postgres";
		keywords[6] = "client_encoding";
		values[6] = ((!isatty(fileno(stdin)) || !isatty(fileno(stdout))) || getenv("PGCLIENTENCODING")) ? NULL : "auto";
		keywords[7] = NULL;
		values[7] = NULL;

		putenv("PGOPTIONS=-c gp_role=utility");

		new_pass = false;
		conn = PQconnectdbParams(keywords, values, true);

		if (!conn)
		{
			elog(ERROR, "%s: could not connect to database %s\n",
					"oid2name", my_opts.dbname);
		}

		if (PQstatus(conn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(conn) &&
			!have_password)
		{
			PQfinish(conn);
			simple_prompt("Password: ", password, sizeof(password), false);
			have_password = true;
			new_pass = true;
		}
	} while (new_pass);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		ProccessErrorMessage(PQerrorMessage(conn));
		PQfinish(conn);
	}

	PQsetNoticeProcessor(conn, NoticeProcessor, NULL);
	PQsetErrorVerbosity(conn, PQERRORS_DEFAULT);
	PQsetErrorContextVisibility(conn, PQSHOW_CONTEXT_ERRORS);
	/* return the conn if good */
	csConn = conn;
}

void
cc_finish(void)
{
	if (csConn)
		PQfinish(csConn);

	csConn = NULL;
}

bytea *
cc_get_returning(PGresult *res)
{
	int			nfields;
	int			nrows;
	char	   *str;
	bytea	   *value;

	/* get the number of fields */
	nrows = PQntuples(res);
	nfields = PQnfields(res);

	Assert(nrows == 1);
	Assert(nfields == 1);

	str = PQgetvalue(res, 0, 0);
	value = cstring_to_bytea(str);

	return value;
}

void
cc_pass_returning(PGresult *res)
{
	int			nfields;
	int			nrows;
	CcPrintUp *dest;
	char **strs;

	nfields = PQnfields(res);
	nrows = PQntuples(res);

	if (nfields > 0)
	{
		dest = cc_printup_create_DR(res->attDescs, res->numAttributes, true);
		cc_printtup_startup(dest);

		strs = palloc(sizeof(char *) * nfields);
		for (int i = 0; i < nrows; ++i)
		{
			for (int j = 0; j < nfields; ++j)
			{
				char *str;

				str = PQgetvalue(res, i, j);
				strs[j] = str;

			}

			cc_printtup(strs, dest);
		}

		cc_preintup_shutdown(dest);
		cc_printup_destroy(dest);
	}


	if (PQcmdStatus(res))
		pq_putmessage('C', PQcmdStatus(res), strlen(PQcmdStatus(res)) + 1);
}

PGresult *
cc_exec_plan(CsQuery *csQuery)
{
	PGresult   *res;
	char	   *csQueryBuf;
	int			csQeuryBufSize;

	csQueryBuf = serializeNode((Node *) csQuery, &csQeuryBufSize, NULL);
	res = PQexecPlan(csConn, csQueryBuf, csQeuryBufSize);
	if (!res || PQresultStatus(res) > 2)
	{
		char *errMsg;

		PQclear(res);
		errMsg =  PQerrorMessage(csConn);

		ProccessErrorMessage(errMsg);
	}

	return res;
}

char *
cc_status(PGresult *res)
{
	return PQcmdStatus(res);
}

CdbCatalogAuxNode *
cc_catalog_or_run(const char *sql)
{
	CsQuery	   *csQuery;

	PGresult   *res;
	char *cmdStatus;
	bytea *value;
	CdbCatalogAuxNode *catAuxNode;

	csQuery = makeNode(CsQuery);
	csQuery->cmdType = CS_QUERY;
	csQuery->query_string = (char *) sql;

	res = cc_exec_plan(csQuery);
	cmdStatus = cc_status(res);

	if (strcmp(cmdStatus, "Catalog") == 0)
	{
		MemoryContext oldCtx;

		value = cc_get_returning(res);

		InvalidateSystemCaches();
		MemoryHeapStorageReset();

		oldCtx = MemoryContextSwitchTo(memoryHeapContext);
		catAuxNode = (CdbCatalogAuxNode *) deserializeNode(VARDATA(value), VARSIZE(value));
		MemoryContextSwitchTo(oldCtx);
	}
	else if (strcmp(cmdStatus, "Both") == 0)
	{
		InvalidateSystemCaches();
		MemoryHeapStorageReset();
		catAuxNode = makeNode(CdbCatalogAuxNode);
	}
	else
	{
		cc_pass_returning(res);

		catAuxNode = NULL;
	}

	return catAuxNode;
}

void
cc_run_on_catalog_server(const char *sql)
{
	PGresult   *res;

	res = PQexec(csConn, sql);
	/* check and deal with errors */
	if (!res || PQresultStatus(res) > 2)
	{
		char *errMsg;

		PQclear(res);
		errMsg = PQerrorMessage(csConn);
		ProccessErrorMessage(errMsg);
	}

	cc_pass_returning(res);

	PQclear(res);
}

CdbCatalogAuxNode *
cc_get_catalog(const char *sql, MemoryContext ctx)
{
	CsQuery *csQuery;
	PGresult   *res;
	bytea *value;
	CdbCatalogAuxNode *catAuxNode;
	MemoryContext oldCtx;
	PGconn *conn = csConn;
	char 		*csQueryBuf;
	int		csQueryLen;

	csQuery = makeNode(CsQuery);

	if (sql)
	{
		csQuery->cmdType = CS_QUERY;
		csQuery->query_string = palloc(strlen(sql) + 1);
		strcpy(csQuery->query_string, sql);
	}
	else
		csQuery->cmdType = CS_STARTUP;

	csQueryBuf = serializeNode((Node *) csQuery, &csQueryLen, NULL);

	/* make the call */
	res = PQexecPlan(conn, csQueryBuf, csQueryLen);

	/* check and deal with errors */
	if (!res || PQresultStatus(res) > 2)
	{
		char *errMsg;

		PQclear(res);
		errMsg =  PQerrorMessage(csConn);

		ProccessErrorMessage(errMsg);
	}

	oldCtx = MemoryContextSwitchTo(ctx);

	value = cc_get_returning(res);
	catAuxNode = (CdbCatalogAuxNode *) deserializeNode(VARDATA(value), VARSIZE(value));

	MemoryContextSwitchTo(oldCtx);

	/* cleanup */
	PQclear(res);

	return catAuxNode;
}

void
cc_send_modify_tabble(BlockListNode *blockListNode)
{
	CsQuery *csQuery;
	PGresult   *res;
	char 		*csQueryBuf;
	int		csQueryLen;

	csQuery = makeNode(CsQuery);
	csQuery->cmdType = CS_MODIFY_TABLE;
	csQuery->data = (Node*) blockListNode;

	/* make the call */
	csQueryBuf = serializeNode((Node *) csQuery, &csQueryLen, NULL);
	res = PQexecPlan(csConn, csQueryBuf, csQueryLen);
	/* check and deal with errors */
	if (PQresultStatus(res) > 2)
	{
		char *errMsg;

		PQclear(res);
		errMsg = PQerrorMessage(csConn);
		ProccessErrorMessage(errMsg);
	}
	PQclear(res);
}

void
cc_next_val(Oid relid, int64 *plast, int64 *pcached, int64  *pincrement, bool *poverflow)
{
	CsQuery	   *csQuery;
	PGresult   *res;
	NextValNode *nextVal;
	char		*csQueryBuf;
	int			 csQueryLen;
	bytea *value;
	
	
	csQuery = makeNode(CsQuery);
	csQuery->cmdType = CS_NEXT_VAL;
	nextVal = makeNode(NextValNode);
	nextVal->relid = relid;
	
	csQuery->data = (Node *) nextVal;
	/* make the call */
	csQueryBuf = serializeNode((Node *) csQuery, &csQueryLen, NULL);
	res = PQexecPlan(csConn, csQueryBuf, csQueryLen);

	/* check and deal with errors */
	if (PQresultStatus(res) > 2)
	{
		char *errMsg;

		PQclear(res);
		errMsg = PQerrorMessage(csConn);
		ProccessErrorMessage(errMsg);
	}

	value = cc_get_returning(res);
	nextVal = (NextValNode *) deserializeNode(VARDATA(value), VARSIZE(value));
	
	*plast = nextVal->plast;
	*pcached = nextVal->pcached;
	*pincrement = nextVal->pincrement;
	*poverflow = nextVal->poverflow;
	
	PQclear(res);
}

void
cc_xact_command(CsType csType)
{
	CsQuery *csQuery;
	PGresult   *res;
	char 		*csQueryBuf;
	int		csQueryLen;

	csQuery = makeNode(CsQuery);
	csQuery->cmdType = csType;

	/* make the call */
	csQueryBuf = serializeNode((Node *) csQuery, &csQueryLen, NULL);
	res = PQexecPlan(csConn, csQueryBuf, csQueryLen);
	/* check and deal with errors */
	if (PQresultStatus(res) > 2 && csType != CS_XACT_ABORT)
	{
		char *errMsg;

		PQclear(res);
		errMsg = PQerrorMessage(csConn);
		ProccessErrorMessage(errMsg);
	}

	PQclear(res);
}

bytea *
cc_get_conf(char *file)
{
	CsQuery	   *csQuery;
	PGresult   *res;
	char	   *csQueryBuf;
	int			csQueryLen;
	bytea	   *value;

	csQuery = makeNode(CsQuery);
	csQuery->cmdType = CS_CONF;
	csQuery->query_string = file;

	/* make the call */
	csQueryBuf = serializeNode((Node *) csQuery, &csQueryLen, NULL);
	res = PQexecPlan(csConn, csQueryBuf, csQueryLen);
	/* check and deal with errors */
	if (PQresultStatus(res) > 2)
	{
		char *errMsg;

		PQclear(res);
		errMsg =  PQerrorMessage(csConn);

		ProccessErrorMessage(errMsg);
	}

	value = cc_get_returning(res);

	PQclear(res);

	return value;
}

/*
 * Catalog server side
 */
static TupleDesc
GetCatalogTupleDesc(void)
{
	TupleDesc tupdesc = NULL;

	if (tupdesc == NULL)
	{
		TupleDesc	tmp;
		MemoryContext oldcontext = MemoryContextSwitchTo(catCollectCtx);

		tmp = CreateTemplateTupleDesc(1);
		TupleDescInitEntry(tmp, 1, "cataux", BYTEAOID, -1, 0);

		MemoryContextSwitchTo(oldcontext);

		tupdesc = tmp;
	}

	return tupdesc;
}

void
DestReceiveBytea(char *data, int dataSize, DestReceiver *dest)
{
	bytea			*buf;
	bool		nulls[1];
	Datum		values[1];
	TupleTableSlot *slot;
	TupleDesc		catTupleDesc;
	HeapTuple	tuple;

	buf = palloc(dataSize + VARHDRSZ);
	memcpy(VARDATA(buf), data, dataSize);
	SET_VARSIZE(buf, dataSize + VARHDRSZ);

	values[0] = PointerGetDatum(buf);
	nulls[0] = false;

	catTupleDesc = GetCatalogTupleDesc();
	tuple = heap_form_tuple(catTupleDesc, values, nulls);

	slot = MakeTupleTableSlot(catTupleDesc, &TTSOpsHeapTuple);
	slot = ExecStoreHeapTuple(tuple, slot, true);

	dest->rStartup(dest, CMD_SELECT, catTupleDesc);
	dest->receiveSlot(slot, dest);
	dest->rShutdown(dest);
	dest->rDestroy(dest);
}


void
cs_get_conf(const char *path, DestReceiver *dest)
{
	FILE *fptr;
	long fileSize;
	char *data;
	long readSize;

	fptr = fopen(path, "r");
	fseek(fptr, 0L, SEEK_END);
	fileSize = ftell(fptr);

	fseek(fptr, 0L, SEEK_SET);
	data = malloc(fileSize);
	readSize = fread(data, fileSize, 1, fptr);
	if (readSize != 1)
	{
		fclose(fptr);
		elog(ERROR, "Read rel cache init file %s error", path);
	}

	fclose(fptr);

	DestReceiveBytea(data, fileSize, dest);
}

void
cs_modify_table(BlockListNode *blockListNode)
{
	tile_insert_block_list_cs(blockListNode);
}

void
cs_next_val(NextValNode *nextVal, DestReceiver *dest)
{
	char *data;
	int dataSize;
	
	nextval_cs(nextVal->relid, &nextVal->plast, &nextVal->pcached, &nextVal->pincrement,
			   &nextVal->poverflow);

	data = serializeNode((Node *) nextVal, &dataSize, NULL);
	DestReceiveBytea(data, dataSize, dest);
}

void
cs_get_startup_catalog(DestReceiver *dest)
{
	CdbCatalogAuxNode *catAux;
	int		dataSize;
	char   *data;

	catAux = makeNode(CdbCatalogAuxNode);
	catAux->catalog = (CdbCatalogNode *) GetBaseCatalog();

	data = serializeNode((Node *) catAux, &dataSize, NULL);
	DestReceiveBytea(data, dataSize, dest);
}

static bool
IsTransactionExitStmt(Node *parsetree)
{
	if (parsetree && IsA(parsetree, TransactionStmt))
	{
		TransactionStmt *stmt = (TransactionStmt *) parsetree;

		if (stmt->kind == TRANS_STMT_COMMIT ||
			stmt->kind == TRANS_STMT_PREPARE ||
			stmt->kind == TRANS_STMT_ROLLBACK ||
			stmt->kind == TRANS_STMT_ROLLBACK_TO)
			return true;
	}
	return false;
}

PlannedStmt *
get_catalog_from_query(List *queries, const char *sql)
{
	List	   *stmt_list;
	ListCell   *cell;
	PlannedStmt	   *plannedStmt = NULL;
	DestReceiver *dest;

	dest = CreateDestReceiver(DestNone);

	stmt_list = pg_plan_queries(queries, CURSOR_OPT_PARALLEL_OK, NULL);

	foreach(cell, stmt_list)
	{
		PlannedStmt *stmt = lfirst_node(PlannedStmt, cell);

		CommandCounterIncrement();

		if (IsAbortedTransactionBlockState() &&
			!IsTransactionExitStmt(stmt->utilityStmt))
			ereport(ERROR,
					(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
					 errmsg("current transaction is aborted, "
							"commands ignored until end of transaction block")));

		if (stmt->utilityStmt == NULL)
		{
			QueryDesc  *qdesc;

			qdesc = CreateQueryDesc(stmt,
									sql,
									GetActiveSnapshot(), NULL,
									dest, NULL, NULL, GP_INSTRUMENT_OPTS);

			ExecutorStart(qdesc, EXEC_FLAG_EXPLAIN_ONLY);
			//ExecutorRun(qdesc, ForwardScanDirection, 0, true);
			//ExecutorFinish(qdesc);
			ExecutorEnd(qdesc);

			FreeQueryDesc(qdesc);

			plannedStmt = stmt;
		}
		else
		{
			ParseState *pstate;
			Node	   *parsetree = stmt->utilityStmt;
			pstate = make_parsestate(NULL);

			if (IsA(stmt->utilityStmt, CopyStmt))
			{
				uint64		processed;

				pstate->p_sourcetext = sql;

				CollectCopy(pstate, (CopyStmt *) stmt->utilityStmt,
							stmt->stmt_location, stmt->stmt_len, &processed);
				catCollector->hasPlOrTigger = false;
			}
			else if (IsA(stmt->utilityStmt, ExplainStmt))
			{
				ExplainStmt *explainStmt;
				ListCell	*cell;
				ListCell	*prev = NULL;
				ListCell	*next;

				explainStmt = (ExplainStmt *) stmt->utilityStmt;

				for (cell = list_head(explainStmt->options); cell; cell = next)
				{
					DefElem *opt = (DefElem *) lfirst(cell);

					next = lnext(cell);
					if (strcmp(opt->defname, "analyze") == 0)
					{
						explainStmt->options = list_delete_cell(explainStmt->options,
																cell, prev);
					}
					else
						prev = cell;

				}

				ProcessUtility(stmt,
							   sql,
							   PROCESS_UTILITY_QUERY,
							   NULL,
							   NULL,
							   dest,
							   NULL);
			}
			else if (IsA(stmt->utilityStmt, PrepareStmt) ||
					 IsA(stmt->utilityStmt, DeallocateStmt))
			{
				ProcessUtility(stmt,
							   sql,
							   PROCESS_UTILITY_QUERY,
							   NULL,
							   NULL,
							   dest,
							   NULL);
			}
			else if (IsA(stmt->utilityStmt,TransactionStmt))
			{
				CatCollectorClear();
				ProcessUtility(stmt,
							   sql,
							   PROCESS_UTILITY_QUERY,
							   NULL,
							   NULL,
							   dest,
							   NULL);
			}
			else if (IsA(stmt->utilityStmt,VariableSetStmt))
			{
				ExecSetVariableStmt((VariableSetStmt *) parsetree, true);
			}
		}
	}

	return plannedStmt;
}

CdbCatalogAuxNode *
cs_get_catalog_from_sql(List *queryList, const char *sql, char *command)
{
	CdbCatalogAuxNode *catAux = NULL;
	PlannedStmt *plannedStmt;

	PG_TRY();
	{
		Gp_role = GP_ROLE_DISPATCH;

		plannedStmt = get_catalog_from_query(queryList, sql);

		/* Be sure to advance the command counter after the last script command */
		CommandCounterIncrement();

		Gp_role = GP_ROLE_UTILITY;
	}
	PG_CATCH();
	{
		Gp_role = GP_ROLE_UTILITY;
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (catCollector)
	{
		if (!catCollector->hasPlOrTigger)
		{
			catAux = makeNode(CdbCatalogAuxNode);
			catAux->plan = plannedStmt;
			catAux->catalog = GetCatalogNode();
			catAux->aux = GetAuxNode();
			strcpy(command, "Catalog");
		}
		else
			strcpy(command, "Server");
	}
	else
		strcpy(command, "Both");

	return catAux;
}

#define VAL(CH)			((CH) - '0')
#define DIG(VAL)		((VAL) + '0')

static bytea *
cstring_to_bytea(char *inputText)
{
	char	   *tp;
	char	   *rp;
	int			bc;
	bytea	   *result;

	/* Recognize hex input */
	if (inputText[0] == '\\' && inputText[1] == 'x')
	{
		size_t		len = strlen(inputText);

		bc = (len - 2) / 2 + VARHDRSZ;	/* maximum possible length */
		result = palloc(bc);
		bc = hex_decode(inputText + 2, len - 2, VARDATA(result));
		SET_VARSIZE(result, bc + VARHDRSZ); /* actual length */

		return result;
	}

	/* Else, it's the traditional escaped style */
	for (bc = 0, tp = inputText; *tp != '\0'; bc++)
	{
		if (tp[0] != '\\')
			tp++;
		else if ((tp[0] == '\\') &&
				 (tp[1] >= '0' && tp[1] <= '3') &&
				 (tp[2] >= '0' && tp[2] <= '7') &&
				 (tp[3] >= '0' && tp[3] <= '7'))
			tp += 4;
		else if ((tp[0] == '\\') &&
				 (tp[1] == '\\'))
			tp += 2;
		else
		{
			/*
			 * one backslash, not followed by another or ### valid octal
			 */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s", "bytea")));
		}
	}

	bc += VARHDRSZ;

	result = (bytea *) palloc(bc);
	SET_VARSIZE(result, bc);

	tp = inputText;
	rp = VARDATA(result);
	while (*tp != '\0')
	{
		if (tp[0] != '\\')
			*rp++ = *tp++;
		else if ((tp[0] == '\\') &&
				 (tp[1] >= '0' && tp[1] <= '3') &&
				 (tp[2] >= '0' && tp[2] <= '7') &&
				 (tp[3] >= '0' && tp[3] <= '7'))
		{
			bc = VAL(tp[1]);
			bc <<= 3;
			bc += VAL(tp[2]);
			bc <<= 3;
			*rp++ = bc + VAL(tp[3]);

			tp += 4;
		}
		else if ((tp[0] == '\\') &&
				 (tp[1] == '\\'))
		{
			*rp++ = '\\';
			tp += 2;
		}
		else
		{
			/*
			 * We should never get here. The first pass should not allow it.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s", "bytea")));
		}
	}

	return result;
}

static CcPrintUp *
cc_printup_create_DR(PGresAttDesc *attDescs, int nattr, bool sendDescrip)
{
	CcPrintUp *ccPrintUp;

	ccPrintUp = palloc0(sizeof(CcPrintUp));

	ccPrintUp->nattr = nattr;
	ccPrintUp->attDescs = attDescs;
	ccPrintUp->sendDescrip = sendDescrip;

	return ccPrintUp;
}

static void
cc_printtup_startup(CcPrintUp *myState)
{
	StringInfo buf;

	/*
	 * Create I/O buffer to be used for all messages.  This cannot be inside
	 * tmpcontext, since we want to re-use it across rows.
	 */
	initStringInfo(&myState->buf);

	buf = &myState->buf;
	/*
	 * Create a temporary memory context that we can reset once per row to
	 * recover palloc'd memory.  This avoids any problems with leaks inside
	 * datatype output routines, and should be faster than retail pfree's
	 * anyway.
	 */
	myState->tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
												"printtup",
												ALLOCSET_DEFAULT_SIZES);

	/*
	 * If we are supposed to emit row descriptions, then send the tuple
	 * descriptor of the tuples.
	 */
	if (myState->sendDescrip)
	{
		/* tuple descriptor message type */
		pq_beginmessage_reuse(&myState->buf, 'T');
		/* # of attrs in tuples */
		pq_sendint16(&myState->buf, myState->nattr);

		enlargeStringInfo(buf, (NAMEDATALEN * MAX_CONVERSION_GROWTH /* attname */
								+ sizeof(Oid)	/* resorigtbl */
								+ sizeof(AttrNumber)	/* resorigcol */
								+ sizeof(Oid)	/* atttypid */
								+ sizeof(int16) /* attlen */
								+ sizeof(int32) /* attypmod */
								+ sizeof(int16) /* format */
							   ) * myState->nattr);

		for (int i = 0; i < myState->nattr; ++i)
		{
			pq_writestring(buf, myState->attDescs[i].name);
			pq_writeint32(buf, myState->attDescs[i].tableid);
			pq_writeint16(buf, myState->attDescs[i].columnid);
			pq_writeint32(buf, myState->attDescs[i].typid);
			pq_writeint16(buf, myState->attDescs[i].typlen);
			pq_writeint32(buf, myState->attDescs[i].atttypmod);
			pq_writeint16(buf, myState->attDescs[i].format);
		}
	}

	pq_endmessage_reuse(buf);
}

static bool
cc_printtup(char **strs, CcPrintUp *myState)
{
	StringInfo	buf = &myState->buf;
	MemoryContext oldcontext;
	int			natts = myState->nattr;


	oldcontext = MemoryContextSwitchTo(myState->tmpcontext);
	pq_beginmessage_reuse(buf, 'D');
	pq_sendint16(buf, natts);

	for (int i = 0; i < natts; ++i)
	{
		int len_net;
		int len;
		char *str;

		str = strs[i];
		if (!str)
		{
			pq_sendint32(buf, -1);
			continue;
		}

		len = strlen(str);
		len_net = htonl(len);
		appendBinaryStringInfo(buf, (char *) &len_net, 4);
		appendBinaryStringInfo(buf, str, len);
	}

	pq_endmessage_reuse(buf);

	MemoryContextSwitchTo(oldcontext);
	MemoryContextReset(myState->tmpcontext);

	return true;
}

static void
cc_preintup_shutdown(CcPrintUp *myState)
{
	if (myState->buf.data)
		pfree(myState->buf.data);
	myState->buf.data = NULL;

	if (myState->tmpcontext)
		MemoryContextDelete(myState->tmpcontext);
	myState->tmpcontext = NULL;
}

static void
cc_printup_destroy(CcPrintUp *myState)
{
	pfree(myState);
}
