/*-------------------------------------------------------------------------
 *
 * postinit.c
 *	  postgres initialization utilities
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/postinit.c
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/session.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/memoryheapam.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_tablespace.h"
#include "catalog/indexing.h"
#include "catalog/storage_tablespace.h"
#include "commands/tablespace.h"

#include "libpq/auth.h"
#include "libpq/hba.h"
#include "libpq/libpq-be.h"
#include "cdb/cdbcatalogfunc.h"
#include "cdb/cdbendpoint.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbutil.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/fts.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/backendid.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/proc.h"
#include "storage/sinvaladt.h"
#include "storage/smgr.h"
#include "storage/sync.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/backend_cancel.h"
#include "utils/dispatchcat.h"
#include "utils/faultinjector.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/pickcat.h"
#include "utils/portal.h"
#include "utils/ps_status.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timeout.h"

#include "utils/resource_manager.h"
#include "utils/session_state.h"


static HeapTuple GetDatabaseTuple(const char *dbname);
static HeapTuple GetDatabaseTupleByOid(Oid dboid);
static void PerformAuthentication(Port *port);
static void CheckMyDatabase(const char *name, bool am_superuser, bool override_allow_connections);
static void InitCommunication(void);
static void ShutdownPostgres(int code, Datum arg);
static void StatementTimeoutHandler(void);
static void LockTimeoutHandler(void);
static void IdleInTransactionSessionTimeoutHandler(void);
static void IdleGangTimeoutHandler(void);
static void GpParallelRetrieveCursorCheckTimeoutHandler(void);
static void ClientCheckTimeoutHandler(void);
static bool ThereIsAtLeastOneRole(void);
static void process_startup_options(Port *port, bool am_superuser);
static void process_settings(Oid databaseid, Oid roleid);

extern bool DoingCommandRead;
#ifdef USE_ORCA
extern void InitGPOPT();
extern void TerminateGPOPT();
#endif


/*** InitPostgres support ***/

/*
 * FindMyDatabase
 *
 * Get oids of the database and default tablespace by the database name.
 * Returns true if succeeded.
 *
 * XXX: This shouldn't be necessary, but for now it's here for fts, etc.
 */
bool
FindMyDatabase(const char *dbname, Oid *db_id, Oid *db_tablespace)
{
	HeapTuple		tuple;
	Form_pg_database dbform;

	tuple = GetDatabaseTuple(dbname);

	if (!HeapTupleIsValid(tuple))
		return false;

	/* Return oids to the caller */
	dbform = (Form_pg_database) GETSTRUCT(tuple);
	*db_id = dbform->oid;
	*db_tablespace = dbform->dattablespace;

	/* Be tidy */
	pfree(tuple);

	return true;
}

/*
 * GetDatabaseTuple -- fetch the pg_database row for a database
 *
 * This is used during backend startup when we don't yet have any access to
 * system catalogs in general.  In the worst case, we can seqscan pg_database
 * using nothing but the hard-wired descriptor that relcache.c creates for
 * pg_database.  In more typical cases, relcache.c was able to load
 * descriptors for both pg_database and its indexes from the shared relcache
 * cache file, and so we can do an indexscan.  criticalSharedRelcachesBuilt
 * tells whether we got the cached descriptors.
 */
static HeapTuple
GetDatabaseTuple(const char *dbname)
{
	HeapTuple	tuple;
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key[1];

	/*
	 * form a scan key
	 */
	ScanKeyInit(&key[0],
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(dbname));

	/*
	 * Open pg_database and fetch a tuple.  Force heap scan if we haven't yet
	 * built the critical shared relcache entries (i.e., we're starting up
	 * without a shared relcache cache file).
	 */
	relation = table_open(DatabaseRelationId, AccessShareLock);
	scan = systable_beginscan(relation, DatabaseNameIndexId,
							  criticalSharedRelcachesBuilt,
							  NULL,
							  1, key);

	tuple = systable_getnext(scan);

	/* Must copy tuple before releasing buffer */
	if (HeapTupleIsValid(tuple))
		tuple = heap_copytuple(tuple);

	/* all done */
	systable_endscan(scan);
	table_close(relation, AccessShareLock);

	return tuple;
}

/*
 * GetDatabaseTupleByOid -- as above, but search by database OID
 */
static HeapTuple
GetDatabaseTupleByOid(Oid dboid)
{
	HeapTuple	tuple;
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key[1];

	/*
	 * form a scan key
	 */
	ScanKeyInit(&key[0],
				Anum_pg_database_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dboid));

	/*
	 * Open pg_database and fetch a tuple.  Force heap scan if we haven't yet
	 * built the critical shared relcache entries (i.e., we're starting up
	 * without a shared relcache cache file).
	 */
	relation = table_open(DatabaseRelationId, AccessShareLock);
	scan = systable_beginscan(relation, DatabaseOidIndexId,
							  criticalSharedRelcachesBuilt,
							  NULL,
							  1, key);

	tuple = systable_getnext(scan);

	/* Must copy tuple before releasing buffer */
	if (HeapTupleIsValid(tuple))
		tuple = heap_copytuple(tuple);

	/* all done */
	systable_endscan(scan);
	table_close(relation, AccessShareLock);

	return tuple;
}


/*
 * PerformAuthentication -- authenticate a remote client
 *
 * returns: nothing.  Will not return at all if there's any failure.
 */
static void
PerformAuthentication(Port *port)
{
	/* This should be set already, but let's make sure */
	ClientAuthInProgress = true;	/* limit visibility of log messages */

	/*
	 * In EXEC_BACKEND case, we didn't inherit the contents of pg_hba.conf
	 * etcetera from the postmaster, and have to load them ourselves.
	 *
	 * FIXME: [fork/exec] Ugh.  Is there a way around this overhead?
	 */
#ifdef EXEC_BACKEND

	/*
	 * load_hba() and load_ident() want to work within the PostmasterContext,
	 * so create that if it doesn't exist (which it won't).  We'll delete it
	 * again later, in PostgresMain.
	 */
	if (PostmasterContext == NULL)
		PostmasterContext = AllocSetContextCreate(TopMemoryContext,
												  "Postmaster",
												  ALLOCSET_DEFAULT_SIZES);

	if (!load_hba())
	{
		/*
		 * It makes no sense to continue if we fail to load the HBA file,
		 * since there is no way to connect to the database in this case.
		 */
		ereport(FATAL,
				(errmsg("could not load pg_hba.conf")));
	}

	if (!load_ident())
	{
		/*
		 * It is ok to continue if we fail to load the IDENT file, although it
		 * means that you cannot log in using any of the authentication
		 * methods that need a user name mapping. load_ident() already logged
		 * the details of error to the log.
		 */
	}
#endif

	/*
	 * Set up a timeout in case a buggy or malicious client fails to respond
	 * during authentication.  Since we're inside a transaction and might do
	 * database access, we have to use the statement_timeout infrastructure.
	 */
	enable_timeout_after(STATEMENT_TIMEOUT, AuthenticationTimeout * 1000);

	/*
	 * Now perform authentication exchange.
	 */
	ClientAuthentication(port); /* might not return, if failure */

	/*
	 * Done with authentication.  Disable the timeout, and log if needed.
	 */
	disable_timeout(STATEMENT_TIMEOUT, false);

	if (Log_connections)
	{
		StringInfoData logmsg;

		initStringInfo(&logmsg);
		if (am_walsender)
			appendStringInfo(&logmsg, _("replication connection authorized: user=%s"),
							 port->user_name);
		else
			appendStringInfo(&logmsg, _("connection authorized: user=%s"),
							 port->user_name);
		if (!am_walsender)
			appendStringInfo(&logmsg, _(" database=%s"), port->database_name);

		if (port->application_name != NULL)
			appendStringInfo(&logmsg, _(" application_name=%s"),
							 port->application_name);

#ifdef USE_SSL
		if (port->ssl_in_use)
			appendStringInfo(&logmsg, _(" SSL enabled (protocol=%s, cipher=%s, bits=%d, compression=%s)"),
							 be_tls_get_version(port),
							 be_tls_get_cipher(port),
							 be_tls_get_cipher_bits(port),
							 be_tls_get_compression(port) ? _("on") : _("off"));
#endif
#ifdef ENABLE_GSS
		if (port->gss)
		{
			const char *princ = be_gssapi_get_princ(port);

			if (princ)
				appendStringInfo(&logmsg,
								 _(" GSS (authenticated=%s, encrypted=%s, principal=%s)"),
								 be_gssapi_get_auth(port) ? _("yes") : _("no"),
								 be_gssapi_get_enc(port) ? _("yes") : _("no"),
								 princ);
			else
				appendStringInfo(&logmsg,
								 _(" GSS (authenticated=%s, encrypted=%s)"),
								 be_gssapi_get_auth(port) ? _("yes") : _("no"),
								 be_gssapi_get_enc(port) ? _("yes") : _("no"));
		}
#endif

		ereport(LOG, errmsg_internal("%s", logmsg.data));
		pfree(logmsg.data);
	}

	set_ps_display("startup", false);

	ClientAuthInProgress = false;	/* client_min_messages is active now */
}


/*
 * CheckMyDatabase -- fetch information from the pg_database entry for our DB
 */
static void
CheckMyDatabase(const char *name, bool am_superuser, bool override_allow_connections)
{
	HeapTuple	tup;
	Form_pg_database dbform;
	char	   *collate;
	char	   *ctype;

	/* Fetch our pg_database row normally, via syscache */
	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	dbform = (Form_pg_database) GETSTRUCT(tup);

	/* This recheck is strictly paranoia */
	if (strcmp(name, NameStr(dbform->datname)) != 0)
		ereport(FATAL,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" has disappeared from pg_database",
						name),
				 errdetail("Database OID %u now seems to belong to \"%s\".",
						   MyDatabaseId, NameStr(dbform->datname))));

	/*
	 * Check permissions to connect to the database.
	 *
	 * These checks are not enforced when in standalone mode, so that there is
	 * a way to recover from disabling all access to all databases, for
	 * example "UPDATE pg_database SET datallowconn = false;".
	 *
	 * We do not enforce them for autovacuum worker processes either.
	 */
	if (IsUnderPostmaster && !IsAutoVacuumWorkerProcess())
	{
		/*
		 * Check that the database is currently allowing connections.
		 */
		if (!dbform->datallowconn && !override_allow_connections)
			ereport(FATAL,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("database \"%s\" is not currently accepting connections",
							name)));

		/*
		 * Check privilege to connect to the database.  (The am_superuser test
		 * is redundant, but since we have the flag, might as well check it
		 * and save a few cycles.)
		 */
		if (!am_superuser &&
			pg_database_aclcheck(MyDatabaseId, GetUserId(),
								 ACL_CONNECT) != ACLCHECK_OK)
			ereport(FATAL,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied for database \"%s\"", name),
					 errdetail("User does not have CONNECT privilege.")));

		/*
		 * Check connection limit for this database.
		 *
		 * There is a race condition here --- we create our PGPROC before
		 * checking for other PGPROCs.  If two backends did this at about the
		 * same time, they might both think they were over the limit, while
		 * ideally one should succeed and one fail.  Getting that to work
		 * exactly seems more trouble than it is worth, however; instead we
		 * just document that the connection limit is approximate.
		 *
		 * We do not want to do this for QEs since a single QD might initialise
		 * many connections to each segment to execute a non-trivial plan and
		 * the db connection limit does not map, semantically, to that idea.
		 */
		if (Gp_role == GP_ROLE_DISPATCH && dbform->datconnlimit >= 0 &&
			!am_superuser &&
			CountDBConnections(MyDatabaseId) > dbform->datconnlimit)
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("too many connections for database \"%s\"",
							name)));
	}

	/*
	 * OK, we're golden.  Next to-do item is to save the encoding info out of
	 * the pg_database tuple.
	 */
	SetDatabaseEncoding(dbform->encoding);
	/* Record it as a GUC internal option, too */
	SetConfigOption("server_encoding", GetDatabaseEncodingName(),
					PGC_INTERNAL, PGC_S_OVERRIDE);
	/* If we have no other source of client_encoding, use server encoding */
	SetConfigOption("client_encoding", GetDatabaseEncodingName(),
					PGC_BACKEND, PGC_S_DYNAMIC_DEFAULT);

	/* assign locale variables */
	collate = NameStr(dbform->datcollate);
	ctype = NameStr(dbform->datctype);

	if (pg_perm_setlocale(LC_COLLATE, collate) == NULL)
		ereport(FATAL,
				(errmsg("database locale is incompatible with operating system"),
				 errdetail("The database was initialized with LC_COLLATE \"%s\", "
						   " which is not recognized by setlocale().", collate),
				 errhint("Recreate the database with another locale or install the missing locale.")));

	if (pg_perm_setlocale(LC_CTYPE, ctype) == NULL)
		ereport(FATAL,
				(errmsg("database locale is incompatible with operating system"),
				 errdetail("The database was initialized with LC_CTYPE \"%s\", "
						   " which is not recognized by setlocale().", ctype),
				 errhint("Recreate the database with another locale or install the missing locale.")));

	/* Make the locale settings visible as GUC variables, too */
	SetConfigOption("lc_collate", collate, PGC_INTERNAL, PGC_S_OVERRIDE);
	SetConfigOption("lc_ctype", ctype, PGC_INTERNAL, PGC_S_OVERRIDE);

	check_strxfrm_bug();

	ReleaseSysCache(tup);
}



/* --------------------------------
 *		InitCommunication
 *
 *		This routine initializes stuff needed for ipc, locking, etc.
 *		it should be called something more informative.
 * --------------------------------
 */
static void
InitCommunication(void)
{
	/*
	 * initialize shared memory and semaphores appropriately.
	 */
	if (!IsUnderPostmaster)		/* postmaster already did this */
	{
		/*
		 * We're running a postgres bootstrap process or a standalone backend.
		 * Though we won't listen on PostPortNumber, use it to select a shmem
		 * key.  This increases the chance of detecting a leftover live
		 * backend of this DataDir.
		 */
		CreateSharedMemoryAndSemaphores(PostPortNumber);
	}
}


/*
 * pg_split_opts -- split a string of options and append it to an argv array
 *
 * The caller is responsible for ensuring the argv array is large enough.  The
 * maximum possible number of arguments added by this routine is
 * (strlen(optstr) + 1) / 2.
 *
 * Because some option values can contain spaces we allow escaping using
 * backslashes, with \\ representing a literal backslash.
 */
void
pg_split_opts(char **argv, int *argcp, const char *optstr)
{
	StringInfoData s;

	initStringInfo(&s);

	while (*optstr)
	{
		bool		last_was_escape = false;

		resetStringInfo(&s);

		/* skip over leading space */
		while (isspace((unsigned char) *optstr))
			optstr++;

		if (*optstr == '\0')
			break;

		/*
		 * Parse a single option, stopping at the first space, unless it's
		 * escaped.
		 */
		while (*optstr)
		{
			if (isspace((unsigned char) *optstr) && !last_was_escape)
				break;

			if (!last_was_escape && *optstr == '\\')
				last_was_escape = true;
			else
			{
				last_was_escape = false;
				appendStringInfoChar(&s, *optstr);
			}

			optstr++;
		}

		/* now store the option in the next argv[] position */
		argv[(*argcp)++] = pstrdup(s.data);
	}

	pfree(s.data);
}

/*
 * Initialize MaxBackends value from config options.
 *
 * This must be called after modules have had the chance to register background
 * workers in shared_preload_libraries, and before shared memory size is
 * determined.
 *
 * Note that in EXEC_BACKEND environment, the value is passed down from
 * postmaster to subprocesses via BackendParameters in SubPostmasterMain; only
 * postmaster itself and processes not under postmaster control should call
 * this.
 */
void
InitializeMaxBackends(void)
{
	Assert(MaxBackends == 0);

	/* the extra unit accounts for the autovacuum launcher */
	MaxBackends = MaxConnections + autovacuum_max_workers + 1 +
		max_worker_processes + max_wal_senders;

	/* internal error because the values were all checked previously */
	if (MaxBackends > MAX_BACKENDS)
		elog(ERROR, "too many backends configured");
}

/*
 * Early initialization of a backend (either standalone or under postmaster).
 * This happens even before InitPostgres.
 *
 * This is separate from InitPostgres because it is also called by auxiliary
 * processes, such as the background writer process, which may not call
 * InitPostgres at all.
 */
void
BaseInit(void)
{
	/*
	 * Attach to shared memory and semaphores, and initialize our
	 * input/output/debugging file descriptors.
	 */
	InitCommunication();
	DebugFileOpen();

	/* Do local initialization of file, storage and buffer managers */
	InitFileAccess();
	InitSync();
	smgrinit();
	InitBufferPoolAccess();

	/* 
	 * Initialize catalog tablespace storage component
	 * with knowledge of how to perform unlink.
	 * 
	 * Needed for xlog replay and normal operations.
	 */
	TablespaceStorageInit(UnlinkTablespaceDirectory);
}

/*
 * Make sure we reserve enough connections for FTS handler.
 */
static void check_superuser_connection_limit()
{
	if (!am_ftshandler &&
		!IS_QUERY_DISPATCHER() &&
		!HaveNFreeProcs(RESERVED_FTS_CONNECTIONS))
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
						errmsg("connection limit exceeded for superusers (need "
									   "at least %d connections reserved for FTS handler)",
							   RESERVED_FTS_CONNECTIONS)));
}

/* --------------------------------
 * InitPostgres
 *		Initialize POSTGRES.
 *
 * The database can be specified by name, using the in_dbname parameter, or by
 * OID, using the dboid parameter.  In the latter case, the actual database
 * name can be returned to the caller in out_dbname.  If out_dbname isn't
 * NULL, it must point to a buffer of size NAMEDATALEN.
 *
 * Similarly, the username can be passed by name, using the username parameter,
 * or by OID using the useroid parameter.
 *
 * In bootstrap mode no parameters are used.  The autovacuum launcher process
 * doesn't use any parameters either, because it only goes far enough to be
 * able to read pg_database; it doesn't connect to any particular database.
 * In walsender mode only username is used.
 *
 * As of PostgreSQL 8.2, we expect InitProcess() was already called, so we
 * already have a PGPROC struct ... but it's not completely filled in yet.
 *
 * Note:
 *		Be very careful with the order of calls in the InitPostgres function.
 * --------------------------------
 */
void
InitPostgres(const char *in_dbname, Oid dboid, const char *username,
			 Oid useroid, char *out_dbname, bool override_allow_connections)
{
	bool		bootstrap = IsBootstrapProcessingMode();
	bool		am_superuser;
	char	   *fullpath;
	char		dbname[NAMEDATALEN];

	if (!IS_CATALOG_SERVER())
	{
		if (MyProcPort != NULL)
		{
			if (MyProcPort->laddr.addr.ss_family == AF_UNIX)
			{
				GpIdentity.segindex = -1;
				// GpIdentity.dbid = 1;
			}
			process_startup_options(MyProcPort, true);
		}

		if (GpIdentity.segindex < 0)
		{
			cc_conn((char *) in_dbname);
		}

		if (initCatalogBuf)
		{
			ClearMemoryHeapStorage();
			MemoryHeapDataSet2(initCatalogBuf, initCatalogBufSize, NULL, 0);
		}
		else if (GpIdentity.segindex < 0)
		{
			CdbCatalogAuxNode *catAux;
			ClearMemoryHeapStorage();
			catAux = cc_get_catalog(NULL, TopMemoryContext);
			MemoryHeapDataSet1(catAux);
			FillBaseCatalog(catAux->catalog);
		}
	}

	DataDispatcherClear();

	/*
	 * Add my PGPROC struct to the ProcArray.
	 *
	 * Once I have done this, I am visible to other backends!
	 */
	InitProcessPhase2();

	/* Initialize SessionState entry */
	SessionState_Init();
	/* Initialize memory protection */
	GPMemoryProtect_Init();

#ifdef USE_ORCA
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		/* Initialize GPOPT */
		OptimizerMemoryContext = AllocSetContextCreate(TopMemoryContext,
													"GPORCA Top-level Memory Context",
													ALLOCSET_DEFAULT_MINSIZE,
													ALLOCSET_DEFAULT_INITSIZE,
													ALLOCSET_DEFAULT_MAXSIZE);

		InitGPOPT();
	}
#endif

	/*
	 * Initialize my entry in the shared-invalidation manager's array of
	 * per-backend data.
	 *
	 * Sets up MyBackendId, a unique backend identifier.
	 */
	MyBackendId = InvalidBackendId;

	SharedInvalBackendInit(false);

	if (MyBackendId > MaxBackends || MyBackendId <= 0)
		elog(FATAL, "bad backend ID: %d", MyBackendId);

	/* Now that we have a BackendId, we can participate in ProcSignal */
	ProcSignalInit(MyBackendId);

	/*
	 * Also set up timeout handlers needed for backend operation.  We need
	 * these in every case except bootstrap.
	 */
	if (!bootstrap)
	{
		RegisterTimeout(DEADLOCK_TIMEOUT, CheckDeadLockAlert);
		RegisterTimeout(STATEMENT_TIMEOUT, StatementTimeoutHandler);
		RegisterTimeout(LOCK_TIMEOUT, LockTimeoutHandler);
		RegisterTimeout(IDLE_IN_TRANSACTION_SESSION_TIMEOUT,
						IdleInTransactionSessionTimeoutHandler);
		RegisterTimeout(IDLE_GANG_TIMEOUT, IdleGangTimeoutHandler);
		RegisterTimeout(GP_PARALLEL_RETRIEVE_CURSOR_CHECK_TIMEOUT, GpParallelRetrieveCursorCheckTimeoutHandler);
		RegisterTimeout(CLIENT_CONNECTION_CHECK_TIMEOUT, ClientCheckTimeoutHandler);
	}

	/*
	 * bufmgr needs another initialization call too
	 */
	InitBufferPoolBackend();

	/*
	 * Initialize local process's access to XLOG.
	 */
	if (IsUnderPostmaster)
	{
		/*
		 * The postmaster already started the XLOG machinery, but we need to
		 * call InitXLOGAccess(), if the system isn't in hot-standby mode.
		 * This is handled by calling RecoveryInProgress and ignoring the
		 * result.
		 */
		(void) RecoveryInProgress();
	}
	else
	{
		/*
		 * We are either a bootstrap process or a standalone backend. Either
		 * way, start up the XLOG machinery, and register to have it closed
		 * down at exit.
		 *
		 * We don't yet have an aux-process resource owner, but StartupXLOG
		 * and ShutdownXLOG will need one.  Hence, create said resource owner
		 * (and register a callback to clean it up after ShutdownXLOG runs).
		 */
		CreateAuxProcessResourceOwner();

		StartupXLOG();
		/* Release (and warn about) any buffer pins leaked in StartupXLOG */
		ReleaseAuxProcessResources(true);
		/* Reset CurrentResourceOwner to nothing for the moment */
		CurrentResourceOwner = NULL;

		on_shmem_exit(ShutdownXLOG, 0);
	}

	/*
	 * Initialize the relation cache and the system catalog caches.  Note that
	 * no catalog access happens here; we only set up the hashtable structure.
	 * We must do this before starting a transaction because transaction abort
	 * would try to touch these hashtables.
	 */
	RelationCacheInitialize();
	InitCatalogCache();
	InitPlanCache();

	/* Initialize portal manager */
	EnablePortalManager();

	/* Initialize stats collection --- must happen before first xact */
	if (!bootstrap)
		pgstat_initialize();

	/*
	 * Load relcache entries for the shared system catalogs.  This must create
	 * at least entries for pg_database and catalogs used for authentication.
	 */
	RelationCacheInitializePhase2();

	/*
	 * Set up process-exit callback to do pre-shutdown cleanup.  This is the
	 * first before_shmem_exit callback we register; thus, this will be the
	 * last thing we do before low-level modules like the buffer manager begin
	 * to close down.  We need to have this in place before we begin our first
	 * transaction --- if we fail during the initialization transaction, as is
	 * entirely possible, we need the AbortTransaction call to clean up.
	 */
	before_shmem_exit(ShutdownPostgres, 0);

	/* The autovacuum launcher is done here */
	if (IsAutoVacuumLauncherProcess())
	{
		/* report this backend in the PgBackendStatus array */
		pgstat_bestart();

		return;
	}

	/*
	 * Start a new transaction here before first access to db, and get a
	 * snapshot.  We don't have a use for the snapshot itself, but we're
	 * interested in the secondary effect that it sets RecentGlobalXmin. (This
	 * is critical for anything that reads heap pages, because HOT may decide
	 * to prune them even if the process doesn't attempt to modify any
	 * tuples.)
	 *
	 * Skip these steps if we are responding to a FTS message on mirror.
	 * Mirror operates in standby mode and is not ready to start a
	 * transaction or create a snapshot.  Neither are they required to
	 * respond to a FTS message.
	 */
	if (!bootstrap && !am_mirror)
	{
		/* statement_timestamp must be set for timeouts to work correctly */
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();

		/*
		 * transaction_isolation will have been set to the default by the
		 * above.  If the default is "serializable", and we are in hot
		 * standby, we will fail if we don't change it to something lower.
		 * Fortunately, "read committed" is plenty good enough.
		 */
		XactIsoLevel = XACT_READ_COMMITTED;

		(void) GetTransactionSnapshot();
	}

	DataDispatcherInit();

	/*
	 * Perform client authentication if necessary, then figure out our
	 * postgres user ID, and see if we are a superuser.
	 *
	 * In standalone mode and in autovacuum worker processes, we use a fixed
	 * ID, otherwise we figure it out from the authenticated user name.
	 */
	if (bootstrap || IsAutoVacuumWorkerProcess())
	{
		InitializeSessionUserIdStandalone();
		am_superuser = true;
	}
	else if (!IsUnderPostmaster)
	{
		InitializeSessionUserIdStandalone();
		am_superuser = true;
		if (!ThereIsAtLeastOneRole())
			ereport(WARNING,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("no roles are defined in this database system"),
					 errhint("You should immediately run CREATE USER \"%s\" SUPERUSER;.",
							 username != NULL ? username : "postgres")));
	}
	else if (IsBackgroundWorker)
	{
		if (username == NULL && !OidIsValid(useroid))
		{
			InitializeSessionUserIdStandalone();
			am_superuser = true;
		}
		else
		{
			InitializeSessionUserId(username, useroid);
			am_superuser = superuser();
		}
	}
	else if (am_mirror)
	{
		Assert(am_ftshandler || am_faulthandler);
		/*
		 * A mirror must receive and act upon FTS messages.  Performing proper
		 * authentication involves reading pg_authid.  Heap access is not
		 * possible on mirror, which is in standby mode.
		 */
		FakeClientAuthentication(MyProcPort);
		InitializeSessionUserIdStandalone();
		am_superuser = true;
	}
	else
	{
		/* normal multiuser case */
		Assert(MyProcPort != NULL);
		PerformAuthentication(MyProcPort);
		InitializeSessionUserId(username, useroid);
		am_superuser = superuser();
		BackendCancelInit(MyBackendId);
	}

	/*
	 * If we're trying to shut down, only superusers can connect, and new
	 * replication connections are not allowed.
	 */
	if ((!am_superuser || am_walsender) &&
		MyProcPort != NULL &&
		MyProcPort->canAcceptConnections == CAC_SUPERUSER)
	{
		if (am_walsender)
			ereport(FATAL,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("new replication connections are not allowed during database shutdown")));
		else
			ereport(FATAL,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to connect during database shutdown")));
	}

	/*
	 * Binary upgrades only allowed super-user connections
	 */
	if (IsBinaryUpgrade && !am_superuser)
	{
		ereport(FATAL,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to connect in binary upgrade mode")));
	}

	/*
	 * The last few connection slots are reserved for superusers.  Replication
	 * connections are drawn from slots reserved with max_wal_senders and not
	 * limited by max_connections or superuser_reserved_connections.
	 *
	 * In Greenplum, there is a concept of restricted mode where
	 * superuser_reserved_connections is set equal to max_connections making
	 * it so only superusers can connect. Changes made in restricted mode need
	 * to be replicated to the standby primary. We currently only support one
	 * walsender anyways so we should allow the connection to happen. This may
	 * need to be reviewed later when we start supporting multiple mirrors.
	 */
	if ((!am_superuser /* || am_walsender */) &&
		ReservedBackends > 0 &&
		!HaveNFreeProcs(ReservedBackends))
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("remaining connection slots are reserved for non-replication superuser connections")));

	if (am_superuser)
		check_superuser_connection_limit();

	/* Check replication permissions needed for walsender processes. */
	if (am_walsender)
	{
		Assert(!bootstrap);

		/*
		 * must have authenticated as a replication role
		 *
		 * In Greenplum, this code path is overloaded for handling FTS messages
		 * on primary as well as mirror.
		 * has_rolreplication() performs a syscache lookup,
		 * which cannot happen on mirror/standby.
		 */
		if (am_walsender && !superuser() && !has_rolreplication(GetUserId()))
			ereport(FATAL,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser or replication role to start walsender")));
	}

	if ((am_ftshandler || am_faulthandler) && !am_superuser)
		ereport(FATAL,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser role to handle FTS request")));

	/*
	 * If this is a plain walsender only supporting physical replication, we
	 * don't want to connect to any particular database. Just finish the
	 * backend startup by processing any options from the startup packet, and
	 * we're done.
	 */
	if ((am_walsender && !am_db_walsender) || am_ftshandler || am_faulthandler)
	{
		/* process any options passed in the startup packet */
		if (MyProcPort != NULL)
			process_startup_options(MyProcPort, am_superuser);

		/* Apply PostAuthDelay as soon as we've read all options */
		if (PostAuthDelay > 0)
			pg_usleep(PostAuthDelay * 1000000L);

		/* initialize client encoding */
		InitializeClientEncoding();

		/* report this backend in the PgBackendStatus array */
		pgstat_bestart();

		/* close the transaction we started above */
		if (!am_mirror)
			CommitTransactionCommand();

		return;
	}

	/*
	 * Set up the global variables holding database id and default tablespace.
	 * But note we won't actually try to touch the database just yet.
	 *
	 * We take a shortcut in the bootstrap case, otherwise we have to look up
	 * the db's entry in pg_database.
	 */
	if (bootstrap)
	{
		MyDatabaseId = TemplateDbOid;
		MyDatabaseTableSpace = DEFAULTTABLESPACE_OID;
	}
	else if (in_dbname != NULL)
	{
		HeapTuple	tuple;
		Form_pg_database dbform;

		tuple = GetDatabaseTuple(in_dbname);
		if (!HeapTupleIsValid(tuple))
			ereport(FATAL,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist", in_dbname)));
		dbform = (Form_pg_database) GETSTRUCT(tuple);
		MyDatabaseId = dbform->oid;
		MyDatabaseTableSpace = dbform->dattablespace;
		/* take database name from the caller, just for paranoia */
		strlcpy(dbname, in_dbname, sizeof(dbname));
		strcpy(MyDatabaseName, dbname);
	}
	else if (OidIsValid(dboid))
	{
		/* caller specified database by OID */
		HeapTuple	tuple;
		Form_pg_database dbform;

		tuple = GetDatabaseTupleByOid(dboid);
		if (!HeapTupleIsValid(tuple))
			ereport(FATAL,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database %u does not exist", dboid)));
		dbform = (Form_pg_database) GETSTRUCT(tuple);
		MyDatabaseId = dbform->oid;
		MyDatabaseTableSpace = dbform->dattablespace;
		Assert(MyDatabaseId == dboid);
		strlcpy(dbname, NameStr(dbform->datname), sizeof(dbname));
		strcpy(MyDatabaseName, dbname);

		/* pass the database name back to the caller */
		if (out_dbname)
			strcpy(out_dbname, dbname);
	}
	else
	{
		/*
		 * If this is a background worker not bound to any particular
		 * database, we're done now.  Everything that follows only makes sense
		 * if we are bound to a specific database.  We do need to close the
		 * transaction we started before returning.
		 */
		if (!bootstrap)
		{
			pgstat_bestart();
			CommitTransactionCommand();
		}
		return;
	}

	/*
	 * Now, take a writer's lock on the database we are trying to connect to.
	 * If there is a concurrently running DROP DATABASE on that database, this
	 * will block us until it finishes (and has committed its update of
	 * pg_database).
	 *
	 * Note that the lock is not held long, only until the end of this startup
	 * transaction.  This is OK since we will advertise our use of the
	 * database in the ProcArray before dropping the lock (in fact, that's the
	 * next thing to do).  Anyone trying a DROP DATABASE after this point will
	 * see us in the array once they have the lock.  Ordering is important for
	 * this because we don't want to advertise ourselves as being in this
	 * database until we have the lock; otherwise we create what amounts to a
	 * deadlock with CountOtherDBBackends().
	 *
	 * Note: use of RowExclusiveLock here is reasonable because we envision
	 * our session as being a concurrent writer of the database.  If we had a
	 * way of declaring a session as being guaranteed-read-only, we could use
	 * AccessShareLock for such sessions and thereby not conflict against
	 * CREATE DATABASE.
	 */
	if (!bootstrap)
		LockSharedObject(DatabaseRelationId, MyDatabaseId, 0,
						 RowExclusiveLock);

	/*
	 * Now we can mark our PGPROC entry with the database ID.
	 *
	 * We assume this is an atomic store so no lock is needed; though actually
	 * things would work fine even if it weren't atomic.  Anyone searching the
	 * ProcArray for this database's ID should hold the database lock, so they
	 * would not be executing concurrently with this store.  A process looking
	 * for another database's ID could in theory see a chance match if it read
	 * a partially-updated databaseId value; but as long as all such searches
	 * wait and retry, as in CountOtherDBBackends(), they will certainly see
	 * the correct value on their next try.
	 */
	MyProc->databaseId = MyDatabaseId;

	/*
	 * We established a catalog snapshot while reading pg_authid and/or
	 * pg_database; but until we have set up MyDatabaseId, we won't react to
	 * incoming sinval messages for unshared catalogs, so we won't realize it
	 * if the snapshot has been invalidated.  Assume it's no good anymore.
	 */
	InvalidateCatalogSnapshot();

	/*
	 * Recheck pg_database to make sure the target database hasn't gone away.
	 * If there was a concurrent DROP DATABASE, this ensures we will die
	 * cleanly without creating a mess.
	 */
	if (!bootstrap)
	{
		HeapTuple	tuple;

		tuple = GetDatabaseTuple(dbname);
		if (!HeapTupleIsValid(tuple) ||
			MyDatabaseId != ((Form_pg_database) GETSTRUCT(tuple))->oid ||
			MyDatabaseTableSpace != ((Form_pg_database) GETSTRUCT(tuple))->dattablespace)
			ereport(FATAL,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist", dbname),
					 errdetail("It seems to have just been dropped or renamed.")));
	}

	/*
	 * Now we should be able to access the database directory safely. Verify
	 * it's there and looks reasonable.
	 */
	fullpath = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);

	if (!bootstrap && IS_CATALOG_SERVER())
	{
		if (access(fullpath, F_OK) == -1)
		{
			if (errno == ENOENT)
				ereport(FATAL,
						(errcode(ERRCODE_UNDEFINED_DATABASE),
						 errmsg("database \"%s\" does not exist",
								dbname),
						 errdetail("The database subdirectory \"%s\" is missing.",
								   fullpath)));
			else
				ereport(FATAL,
						(errcode_for_file_access(),
						 errmsg("could not access directory \"%s\": %m",
								fullpath)));
		}

		ValidatePgVersion(fullpath);
	}

	SetDatabasePath(fullpath);

	/*
	 * It's now possible to do real access to the system catalogs.
	 *
	 * Load relcache entries for the system catalogs.  This must create at
	 * least the minimum set of "nailed-in" cache entries.
	 */
	RelationCacheInitializePhase3();

	/* set up ACL framework (so CheckMyDatabase can check permissions) */
	initialize_acl();

	/*
	 * Re-read the pg_database row for our database, check permissions and set
	 * up database-specific GUC settings.  We can't do this until all the
	 * database-access infrastructure is up.  (Also, it wants to know if the
	 * user is a superuser, so the above stuff has to happen first.)
	 */
	if (!bootstrap)
		CheckMyDatabase(dbname, am_superuser, override_allow_connections);

	/*
	 * Reject non-utility connections if the PostMaster was started in the
	 * utility mode.
	 */
	if (IsUnderPostmaster && !IsAutoVacuumWorkerProcess() && Gp_role == GP_ROLE_UTILITY)
		should_reject_connection = true;

	/*
	 * Now process any command-line switches and any additional GUC variable
	 * settings passed in the startup packet.   We couldn't do this before
	 * because we didn't know if client is a superuser.
	 */
	if (MyProcPort != NULL)
		process_startup_options(MyProcPort, am_superuser);

	/*
	 * am_cursor_retrieve_handler is set by GUC so need to judge after calling
	 * process_startup_options().
	 */
	if (am_cursor_retrieve_handler)
	{
		Gp_role = GP_ROLE_UTILITY;
		should_reject_connection = false;

		/* Sanity check for security: This should not happen but in case ... */
		if (!retrieve_conn_authenticated)
			ereport(FATAL,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("retrieve connection was not authenticated for unknown reason")));
		InitRetrieveCtl();
	}

	/*
	 * Maintenance Mode: allow superuser to connect when
	 * gp_maintenance_conn GUC is set.  We cannot check it until
	 * process_startup_options parses the GUC.
	 */
	if (gp_maintenance_mode && Gp_role == GP_ROLE_DISPATCH &&
		!(am_superuser && gp_maintenance_conn))
		ereport(FATAL,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("maintenance mode: connected by superuser only")));

	if (should_reject_connection)
		ereport(FATAL,(errcode(ERRCODE_CANNOT_CONNECT_NOW),
					   errmsg("System was started in single node mode - only utility mode connections are allowed")));

	if (Gp_role == GP_ROLE_EXECUTE && gp_session_id < 0)
		ereport(FATAL,
				(errcode(ERRCODE_CANNOT_CONNECT_NOW),
				 errmsg("connections to primary segments are not allowed"),
				 errdetail("This database instance is running as a primary segment in a Greenplum cluster and does not permit direct connections."),
				 errhint("To force a connection anyway (dangerous!), use utility mode.")));

	/* Process pg_db_role_setting options */
	process_settings(MyDatabaseId, GetSessionUserId());

	/* Apply PostAuthDelay as soon as we've read all options */
	if (PostAuthDelay > 0)
		pg_usleep(PostAuthDelay * 1000000L);

	/*
	 * Initialize various default states that can't be set up until we've
	 * selected the active user and gotten the right GUC settings.
	 */

	/* set default namespace search path */
	InitializeSearchPath();

	/* initialize client encoding */
	InitializeClientEncoding();

	/* Initialize this backend's session state. */
	InitializeSession();

	/*
	 * report this backend in the PgBackendStatus array, meanwhile, we do not
	 * want users to see auxiliary background worker like fts in pg_stat_* views.
	 */
	if (!bootstrap && !amAuxiliaryBgWorker())
		pgstat_bestart();

	/* 
     * MPP package setup 
     *
     * Primary function is to establish connections to the qExecs.
     * This is SKIPPED when the database is in bootstrap mode or 
     * Is not UnderPostmaster.
     */
    if (!bootstrap && IsUnderPostmaster)
    {
		cdb_setup();
		on_proc_exit( cdb_cleanup, 0 );
    }

	/*
	 * Initialize resource manager.
	 */
	InitResManager();

	PickBaseCatalog();
	DataDispatcherClear();
	/* close the transaction we started above */
	if (!bootstrap)
		CommitTransactionCommand();
}

/*
 * Process any command-line switches and any additional GUC variable
 * settings passed in the startup packet.
 */
static void
process_startup_options(Port *port, bool am_superuser)
{
	GucContext	gucctx;
	ListCell   *gucopts;

	gucctx = am_superuser ? PGC_SU_BACKEND : PGC_BACKEND;

	/*
	 * First process any command-line switches that were included in the
	 * startup packet, if we are in a regular backend.
	 */
	if (port->cmdline_options != NULL)
	{
		/*
		 * The maximum possible number of commandline arguments that could
		 * come from port->cmdline_options is (strlen + 1) / 2; see
		 * pg_split_opts().
		 */
		char	  **av;
		int			maxac;
		int			ac;

		maxac = 2 + (strlen(port->cmdline_options) + 1) / 2;

		av = (char **) palloc(maxac * sizeof(char *));
		ac = 0;

		av[ac++] = "postgres";

		pg_split_opts(av, &ac, port->cmdline_options);

		av[ac] = NULL;

		Assert(ac < maxac);

		(void) process_postgres_switches(ac, av, gucctx, NULL);
	}

	/*
	 * Process any additional GUC variable settings passed in startup packet.
	 * These are handled exactly like command-line variables.
	 */
	gucopts = list_head(port->guc_options);
	while (gucopts)
	{
		char	   *name;
		char	   *value;

		name = lfirst(gucopts);
		gucopts = lnext(gucopts);

		value = lfirst(gucopts);
		gucopts = lnext(gucopts);

		SetConfigOption(name, value, gucctx, PGC_S_CLIENT);
	}

	/* Set GUCs that have been changed in the QD */
	/* NOTE: this code block will not change the reset_val of the GUCs */
	if (port->diff_options)
	{
		char	  **av;
		int			maxac;
		int			ac;

		maxac = 1 + (strlen(port->diff_options) + 1)/2;

		av = (char **) palloc(maxac * sizeof(char *));
		ac = 0;

		pg_split_opts(av, &ac, port->diff_options);

		av[ac] = NULL;
		Assert(ac < maxac);
		for (int i = 0; i < ac; i++)
		{
			char *name = NULL;
			char *val = NULL;
			ParseLongOption(av[i], &name, &val);
			SetConfigOption(name, val, gucctx, PGC_S_SESSION);
		}
		pfree(av);
	}
}

/*
 * Load GUC settings from pg_db_role_setting.
 *
 * We try specific settings for the database/role combination, as well as
 * general for this database and for this user.
 */
static void
process_settings(Oid databaseid, Oid roleid)
{
	Relation	relsetting;
	Snapshot	snapshot;

	if (!IsUnderPostmaster || !IS_CATALOG_SERVER())
		return;

	relsetting = table_open(DbRoleSettingRelationId, AccessShareLock);

	/* read all the settings under the same snapshot for efficiency */
	snapshot = RegisterSnapshot(GetCatalogSnapshot(DbRoleSettingRelationId));

	/* Later settings are ignored if set earlier. */
	ApplySetting(snapshot, databaseid, roleid, relsetting, PGC_S_DATABASE_USER);
	ApplySetting(snapshot, InvalidOid, roleid, relsetting, PGC_S_USER);
	ApplySetting(snapshot, databaseid, InvalidOid, relsetting, PGC_S_DATABASE);
	ApplySetting(snapshot, InvalidOid, InvalidOid, relsetting, PGC_S_GLOBAL);

	UnregisterSnapshot(snapshot);
	table_close(relsetting, AccessShareLock);
}

/*
 * Backend-shutdown callback.  Do cleanup that we want to be sure happens
 * before all the supporting modules begin to nail their doors shut via
 * their own callbacks.
 *
 * User-level cleanup, such as temp-relation removal and UNLISTEN, happens
 * via separate callbacks that execute before this one.  We don't combine the
 * callbacks because we still want this one to happen if the user-level
 * cleanup fails.
 */
static void
ShutdownPostgres(int code, Datum arg)
{
	/* Make sure we've killed any active transaction */
	AbortOutOfAnyTransaction();

	/*
	 * If there was a segment OOM for which we haven't already reported
	 * our usage, report now.
	 */
	ReportOOMConsumption();

#ifdef USE_ORCA
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		TerminateGPOPT();

		if (OptimizerMemoryContext != NULL)
			MemoryContextDelete(OptimizerMemoryContext);
	}
#endif

	/* Disable memory protection */
	GPMemoryProtect_Shutdown();
	/* Release SessionState entry */
	SessionState_Shutdown();

	/*
	 * User locks are not released by transaction end, so be sure to release
	 * them explicitly.
	 */
	LockReleaseAll(USER_LOCKMETHOD, true);
}


/*
 * STATEMENT_TIMEOUT handler: trigger a query-cancel interrupt.
 */
static void
StatementTimeoutHandler(void)
{
	int			sig = SIGINT;

	/*
	 * During authentication the timeout is used to deal with
	 * authentication_timeout - we want to quit in response to such timeouts.
	 */
	if (ClientAuthInProgress)
		sig = SIGTERM;

#ifdef HAVE_SETSID
	/* try to signal whole process group */
	kill(-MyProcPid, sig);
#endif
	kill(MyProcPid, sig);
}

/*
 * LOCK_TIMEOUT handler: trigger a query-cancel interrupt.
 */
static void
LockTimeoutHandler(void)
{
#ifdef HAVE_SETSID
	/* try to signal whole process group */
	kill(-MyProcPid, SIGINT);
#endif
	kill(MyProcPid, SIGINT);
}

static void
IdleInTransactionSessionTimeoutHandler(void)
{
	IdleInTransactionSessionTimeoutPending = true;
	InterruptPending = true;
	SetLatch(MyLatch);
}

static void
IdleGangTimeoutHandler(void)
{
	IdleGangTimeoutPending = true;
	InterruptPending = true;
	SetLatch(MyLatch);
}

static void
ClientCheckTimeoutHandler(void)
{
	CheckClientConnectionPending = true;
	InterruptPending = true;
	SetLatch(&MyProc->procLatch);
}

static void
GpParallelRetrieveCursorCheckTimeoutHandler(void)
{
	/*
	 * issue: https://github.com/greenplum-db/gpdb/issues/15143
	 *
	 * handle errors of parallel retrieve cursor's non-root slices
	 */
	if (DoingCommandRead)
	{
		Assert(Gp_role == GP_ROLE_DISPATCH);

		/* It calls cdbdisp_checkForCancel(), which doesn't raise error */
		gp_check_parallel_retrieve_cursor_error();
		int num = GetNumOfParallelRetrieveCursors();

		/* Reset the alarm to check after a timeout */
		if (num > 0)
		{
			elog(DEBUG1, "There are still %d parallel retrieve cursors alive", num);
			enable_parallel_retrieve_cursor_check_timeout();
		}
	}
	else
	{
		elog(DEBUG1, "DoingCommandRead is false, check parallel cursor timeout delay");
		enable_parallel_retrieve_cursor_check_timeout();
	}
}

/*
 * Returns true if at least one role is defined in this database cluster.
 */
static bool
ThereIsAtLeastOneRole(void)
{
	Relation	pg_authid_rel;
	TableScanDesc scan;
	bool		result;

	pg_authid_rel = table_open(AuthIdRelationId, AccessShareLock);

	scan = table_beginscan_catalog(pg_authid_rel, 0, NULL);
	result = (heap_getnext(scan, ForwardScanDirection) != NULL);

	table_endscan(scan);
	table_close(pg_authid_rel, AccessShareLock);

	return result;
}
