/*-------------------------------------------------------------------------
 *
 * pg_basebackup.c - receive a base backup using streaming replication protocol
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/pg_basebackup.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "access/xlog_internal.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/logging.h"
#include "common/string.h"
#include "fe_utils/recovery_gen.h"
#include "fe_utils/string_utils.h"
#include "getopt_long.h"
#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "pgtar.h"
#include "pgtime.h"
#include "receivelog.h"
#include "replication/basebackup.h"
#include "streamutil.h"
#include "catalog/catalog.h"


#define ERRCODE_DATA_CORRUPTED	"XX001"

typedef struct TablespaceListCell
{
	struct TablespaceListCell *next;
	char		old_dir[MAXPGPATH];
	char		new_dir[MAXPGPATH];
} TablespaceListCell;

typedef struct TablespaceList
{
	TablespaceListCell *head;
	TablespaceListCell *tail;
} TablespaceList;

/*
 * pg_xlog has been renamed to pg_wal in version 10.  This version number
 * should be compared with PQserverVersion().
 */
#define MINIMUM_VERSION_FOR_PG_WAL	100000

/*
 * Temporary replication slots are supported from version 10.
 */
#define MINIMUM_VERSION_FOR_TEMP_SLOTS 100000

/*
 * Different ways to include WAL
 */
typedef enum
{
	NO_WAL,
	FETCH_WAL,
	STREAM_WAL
} IncludeWal;

/* Global options */
static char *basedir = NULL;
static TablespaceList tablespace_dirs = {NULL, NULL};
static char *xlog_dir = NULL;
static char format = 'p';		/* p(lain)/t(ar) */
static char *label = "pg_basebackup base backup";
static bool noclean = false;
static bool checksum_failure = false;
static bool showprogress = false;
static int	verbose = 0;
static int	compresslevel = 0;
static IncludeWal includewal = STREAM_WAL;
static bool fastcheckpoint = false;
static bool writerecoveryconf = false;
static bool writeconffilesonly = false;
static bool do_sync = true;
static int	standby_message_timeout = 10 * 1000;	/* 10 sec = default */
static pg_time_t last_progress_report = 0;
static int32 maxrate = 0;		/* no limit by default */
static char *replication_slot = NULL;
static bool temp_replication_slot = true;
static bool create_slot = false;
static bool no_slot = false;
static bool verify_checksums = true;

static bool success = false;
static bool made_new_pgdata = false;
static bool found_existing_pgdata = false;
static bool made_new_xlogdir = false;
static bool found_existing_xlogdir = false;
static bool made_tablespace_dirs = false;
static bool found_tablespace_dirs = false;

static bool forceoverwrite = false;
#define MAX_EXCLUDE 255
static int	num_exclude = 0;
static char *excludes[MAX_EXCLUDE];
static int	num_exclude_from = 0;
static char *excludefroms[MAX_EXCLUDE];
static int target_gp_dbid = 0;

/* GUC */
static char *log_directory = NULL;

/* Progress counters */
static uint64 totalsize;
static uint64 totaldone;
static int	tablespacecount;

/* Pipe to communicate with background wal receiver process */
#ifndef WIN32
static int	bgpipe[2] = {-1, -1};
#endif

/* Handle to child process */
static pid_t bgchild = -1;
static bool in_log_streamer = false;

/* End position for xlog streaming, empty string if unknown yet */
static XLogRecPtr xlogendptr;

#ifndef WIN32
static int	has_xlogendptr = 0;
#else
static volatile LONG has_xlogendptr = 0;
#endif

/* Contents of configuration file to be generated */
static PQExpBuffer recoveryconfcontents = NULL;

/* Function headers */
static void usage(void);
static void verify_dir_is_empty_or_create(char *dirname, bool *created, bool *found);
static void progress_report(int tablespacenum, const char *filename, bool force,
							bool finished);

static void ReceiveTarFile(PGconn *conn, PGresult *res, int rownum);
static void ReceiveAndUnpackTarFile(PGconn *conn, PGresult *res, int rownum);
static void GetLogDirectory(PGconn *conn);
static void BaseBackup(void);

static bool reached_end_position(XLogRecPtr segendpos, uint32 timeline,
								 bool segment_finished);

static const char *get_tablespace_mapping(const char *dir);
static void tablespace_list_append(const char *arg);
static void WriteInternalConfFile(void);

static char *run_simple_query(PGconn *conn, const char *sql);

static void
cleanup_directories_atexit(void)
{
	if (success || in_log_streamer)
		return;

	if (!noclean && !checksum_failure)
	{
		if (made_new_pgdata)
		{
			pg_log_info("removing data directory \"%s\"", basedir);
			if (!rmtree(basedir, true))
				pg_log_error("failed to remove data directory");
		}
		else if (found_existing_pgdata)
		{
			pg_log_info("removing contents of data directory \"%s\"", basedir);
			if (!rmtree(basedir, false))
				pg_log_error("failed to remove contents of data directory");
		}

		if (made_new_xlogdir)
		{
			pg_log_info("removing WAL directory \"%s\"", xlog_dir);
			if (!rmtree(xlog_dir, true))
				pg_log_error("failed to remove WAL directory");
		}
		else if (found_existing_xlogdir)
		{
			pg_log_info("removing contents of WAL directory \"%s\"", xlog_dir);
			if (!rmtree(xlog_dir, false))
				pg_log_error("failed to remove contents of WAL directory");
		}
	}
	else
	{
		if ((made_new_pgdata || found_existing_pgdata) && !checksum_failure)
			pg_log_info("data directory \"%s\" not removed at user's request", basedir);

		if (made_new_xlogdir || found_existing_xlogdir)
			pg_log_info("WAL directory \"%s\" not removed at user's request", xlog_dir);
	}

	if ((made_tablespace_dirs || found_tablespace_dirs) && !checksum_failure)
		pg_log_info("changes to tablespace directories will not be undone");
}

static void
disconnect_atexit(void)
{
	if (conn != NULL)
		PQfinish(conn);
}

#ifndef WIN32
/*
 * On windows, our background thread dies along with the process. But on
 * Unix, if we have started a subprocess, we want to kill it off so it
 * doesn't remain running trying to stream data.
 */
static void
kill_bgchild_atexit(void)
{
	if (bgchild > 0)
		kill(bgchild, SIGTERM);
}
#endif

/*
 * Split argument into old_dir and new_dir and append to tablespace mapping
 * list.
 */
static void
tablespace_list_append(const char *arg)
{
	TablespaceListCell *cell = (TablespaceListCell *) pg_malloc0(sizeof(TablespaceListCell));
	char	   *dst;
	char	   *dst_ptr;
	const char *arg_ptr;

	dst_ptr = dst = cell->old_dir;
	for (arg_ptr = arg; *arg_ptr; arg_ptr++)
	{
		if (dst_ptr - dst >= MAXPGPATH)
		{
			pg_log_error("directory name too long");
			exit(1);
		}

		if (*arg_ptr == '\\' && *(arg_ptr + 1) == '=')
			;					/* skip backslash escaping = */
		else if (*arg_ptr == '=' && (arg_ptr == arg || *(arg_ptr - 1) != '\\'))
		{
			if (*cell->new_dir)
			{
				pg_log_error("multiple \"=\" signs in tablespace mapping");
				exit(1);
			}
			else
				dst = dst_ptr = cell->new_dir;
		}
		else
			*dst_ptr++ = *arg_ptr;
	}

	if (!*cell->old_dir || !*cell->new_dir)
	{
		pg_log_error("invalid tablespace mapping format \"%s\", must be \"OLDDIR=NEWDIR\"", arg);
		exit(1);
	}

	/*
	 * All tablespaces are created with absolute directories, so specifying a
	 * non-absolute path here would just never match, possibly confusing users.
	 * Since we don't know whether the remote side is Windows or not, and it
	 * might be different than the local side, permit any path that could be
	 * absolute under either set of rules.
	 *
	 * (There is little practical risk of confusion here, because someone
	 * running entirely on Linux isn't likely to have a relative path that
	 * begins with a backslash or something that looks like a drive
	 * specification. If they do, and they also incorrectly believe that
	 * a relative path is acceptable here, we'll silently fail to warn them
	 * of their mistake, and the -T option will just not get applied, same
	 * as if they'd specified -T for a nonexistent tablespace.)
	 */
	if (!is_nonwindows_absolute_path(cell->old_dir) &&
		!is_windows_absolute_path(cell->old_dir))
	{
		pg_log_error("old directory is not an absolute path in tablespace mapping: %s",
					 cell->old_dir);
		exit(1);
	}

	if (!is_absolute_path(cell->new_dir))
	{
		pg_log_error("new directory is not an absolute path in tablespace mapping: %s",
					 cell->new_dir);
		exit(1);
	}

	/*
	 * Comparisons done with these values should involve similarly
	 * canonicalized path values.  This is particularly sensitive on Windows
	 * where path values may not necessarily use Unix slashes.
	 */
	canonicalize_path(cell->old_dir);
	canonicalize_path(cell->new_dir);

	if (tablespace_dirs.tail)
		tablespace_dirs.tail->next = cell;
	else
		tablespace_dirs.head = cell;
	tablespace_dirs.tail = cell;
}


#ifdef HAVE_LIBZ
static const char *
get_gz_error(gzFile gzf)
{
	int			errnum;
	const char *errmsg;

	errmsg = gzerror(gzf, &errnum);
	if (errnum == Z_ERRNO)
		return strerror(errno);
	else
		return errmsg;
}
#endif

static void
usage(void)
{
	printf(_("%s takes a base backup of a running PostgreSQL server.\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nOptions controlling the output:\n"));
	printf(_("  -D, --pgdata=DIRECTORY receive base backup into directory\n"));
	printf(_("  -F, --format=p|t       output format (plain (default), tar (Unsupported in GPDB))\n"));
	printf(_("  -r, --max-rate=RATE    maximum transfer rate to transfer data directory\n"
			 "                         (in kB/s, or use suffix \"k\" or \"M\")\n"));
	printf(_("  -R, --write-recovery-conf\n"
			 "                         write configuration for replication\n"));
	printf(_("  -o, --write-conf-files-only\n"
			 "                         write configuration files only\n"));
	printf(_("  -T, --tablespace-mapping=OLDDIR=NEWDIR\n"
			 "                         relocate tablespace in OLDDIR to NEWDIR\n"));
	printf(_("      --waldir=WALDIR    location for the write-ahead log directory\n"));
	printf(_("  -X, --wal-method=none|fetch|stream\n"
			 "                         include required WAL files with specified method\n"));
	printf(_("  -z, --gzip             compress tar output\n"));
	printf(_("  -Z, --compress=0-9     compress tar output with given compression level\n"));
	printf(_("  --target-gp-dbid       create tablespace subdirectories with given dbid\n"));
	printf(_("\nGeneral options:\n"));
	printf(_("  -c, --checkpoint=fast|spread\n"
			 "                         set fast or spread checkpointing\n"));
	printf(_("  -C, --create-slot      create replication slot\n"));
	printf(_("  -l, --label=LABEL      set backup label\n"));
	printf(_("  -n, --no-clean         do not clean up after errors\n"));
	printf(_("  -N, --no-sync          do not wait for changes to be written safely to disk\n"));
	printf(_("  -P, --progress         show progress information\n"));
	printf(_("  -S, --slot=SLOTNAME    replication slot to use\n"));
	printf(_("  -v, --verbose          output verbose messages\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("      --no-slot          prevent creation of temporary replication slot\n"));
	printf(_("      --no-verify-checksums\n"
			 "                         do not verify checksums\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -d, --dbname=CONNSTR   connection string\n"));
	printf(_("  -h, --host=HOSTNAME    database server host or socket directory\n"));
	printf(_("  -p, --port=PORT        database server port number\n"));
	printf(_("  -s, --status-interval=INTERVAL\n"
			 "                         time between status packets sent to server (in seconds)\n"));
	printf(_("  -U, --username=NAME    connect as specified database user\n"));
	printf(_("  -w, --no-password      never prompt for password\n"));
	printf(_("  -W, --password         force password prompt (should happen automatically)\n"));
	printf(_("  -E, --exclude          exclude path names\n"));
	printf(_("      --exclude-from=FILE\n"
			 "                         get path names to exclude from FILE\n"));
	printf(_("\nReport bugs to <bugs@greenplum.org>.\n"));
}


/*
 * Called in the background process every time data is received.
 * On Unix, we check to see if there is any data on our pipe
 * (which would mean we have a stop position), and if it is, check if
 * it is time to stop.
 * On Windows, we are in a single process, so we can just check if it's
 * time to stop.
 */
static bool
reached_end_position(XLogRecPtr segendpos, uint32 timeline,
					 bool segment_finished)
{
	if (!has_xlogendptr)
	{
#ifndef WIN32
		fd_set		fds;
		struct timeval tv;
		int			r;

		/*
		 * Don't have the end pointer yet - check our pipe to see if it has
		 * been sent yet.
		 */
		FD_ZERO(&fds);
		FD_SET(bgpipe[0], &fds);

		MemSet(&tv, 0, sizeof(tv));

		r = select(bgpipe[0] + 1, &fds, NULL, NULL, &tv);
		if (r == 1)
		{
			char		xlogend[64];
			uint32		hi,
						lo;

			MemSet(xlogend, 0, sizeof(xlogend));
			r = read(bgpipe[0], xlogend, sizeof(xlogend) - 1);
			if (r < 0)
			{
				pg_log_error("could not read from ready pipe: %m");
				exit(1);
			}

			if (sscanf(xlogend, "%X/%X", &hi, &lo) != 2)
			{
				pg_log_error("could not parse write-ahead log location \"%s\"",
							 xlogend);
				exit(1);
			}
			xlogendptr = ((uint64) hi) << 32 | lo;
			has_xlogendptr = 1;

			/*
			 * Fall through to check if we've reached the point further
			 * already.
			 */
		}
		else
		{
			/*
			 * No data received on the pipe means we don't know the end
			 * position yet - so just say it's not time to stop yet.
			 */
			return false;
		}
#else

		/*
		 * On win32, has_xlogendptr is set by the main thread, so if it's not
		 * set here, we just go back and wait until it shows up.
		 */
		return false;
#endif
	}

	/*
	 * At this point we have an end pointer, so compare it to the current
	 * position to figure out if it's time to stop.
	 */
	if (segendpos >= xlogendptr)
		return true;

	/*
	 * Have end pointer, but haven't reached it yet - so tell the caller to
	 * keep streaming.
	 */
	return false;
}

typedef struct
{
	PGconn	   *bgconn;
	XLogRecPtr	startptr;
	char		xlog[MAXPGPATH];	/* directory or tarfile depending on mode */
	char	   *sysidentifier;
	int			timeline;
} logstreamer_param;

static int
LogStreamerMain(logstreamer_param *param)
{
	StreamCtl	stream;

	in_log_streamer = true;

	MemSet(&stream, 0, sizeof(stream));
	stream.startpos = param->startptr;
	stream.timeline = param->timeline;
	stream.sysidentifier = param->sysidentifier;
	stream.stream_stop = reached_end_position;
#ifndef WIN32
	stream.stop_socket = bgpipe[0];
#else
	stream.stop_socket = PGINVALID_SOCKET;
#endif
	stream.standby_message_timeout = standby_message_timeout;
	stream.synchronous = false;
	/* fsync happens at the end of pg_basebackup for all data */
	stream.do_sync = false;
	stream.mark_done = true;
	stream.partial_suffix = NULL;
	stream.replication_slot = replication_slot;

	if (format == 'p')
		stream.walmethod = CreateWalDirectoryMethod(param->xlog, 0,
													stream.do_sync);
	else
		stream.walmethod = CreateWalTarMethod(param->xlog, compresslevel,
											  stream.do_sync);

	if (!ReceiveXlogStream(param->bgconn, &stream))

		/*
		 * Any errors will already have been reported in the function process,
		 * but we need to tell the parent that we didn't shutdown in a nice
		 * way.
		 */
		return 1;

	if (!stream.walmethod->finish())
	{
		pg_log_error("could not finish writing WAL files: %m");
		return 1;
	}

	PQfinish(param->bgconn);

	if (format == 'p')
		FreeWalDirectoryMethod();
	else
		FreeWalTarMethod();
	pg_free(stream.walmethod);

	return 0;
}

/*
 * Initiate background process for receiving xlog during the backup.
 * The background stream will use its own database connection so we can
 * stream the logfile in parallel with the backups.
 */
static void
StartLogStreamer(char *startpos, uint32 timeline, char *sysidentifier)
{
	logstreamer_param *param;
	uint32		hi,
				lo;
	char		statusdir[MAXPGPATH];

	param = pg_malloc0(sizeof(logstreamer_param));
	param->timeline = timeline;
	param->sysidentifier = sysidentifier;

	/* Convert the starting position */
	if (sscanf(startpos, "%X/%X", &hi, &lo) != 2)
	{
		pg_log_error("could not parse write-ahead log location \"%s\"",
					 startpos);
		exit(1);
	}
	param->startptr = ((uint64) hi) << 32 | lo;
	/* Round off to even segment position */
	param->startptr -= XLogSegmentOffset(param->startptr, WalSegSz);

#ifndef WIN32
	/* Create our background pipe */
	if (pipe(bgpipe) < 0)
	{
		pg_log_error("could not create pipe for background process: %m");
		exit(1);
	}
#endif

	/* Get a second connection */
	param->bgconn = GetConnection();
	if (!param->bgconn)
		/* Error message already written in GetConnection() */
		exit(1);

	/* In post-10 cluster, pg_xlog has been renamed to pg_wal */
	snprintf(param->xlog, sizeof(param->xlog), "%s/%s",
			 basedir,
			 PQserverVersion(conn) < MINIMUM_VERSION_FOR_PG_WAL ?
			 "pg_xlog" : "pg_wal");

	/* Temporary replication slots are only supported in 10 and newer */
	if (PQserverVersion(conn) < MINIMUM_VERSION_FOR_TEMP_SLOTS)
		temp_replication_slot = false;

	/*
	 * Create replication slot if requested
	 */
	if (temp_replication_slot && !replication_slot)
		replication_slot = psprintf("pg_basebackup_%d", (int) PQbackendPID(param->bgconn));
	if (temp_replication_slot || create_slot)
	{
		if (!CreateReplicationSlot(param->bgconn, replication_slot, NULL,
								   temp_replication_slot, true, true, false))
			exit(1);

		if (verbose)
		{
			if (temp_replication_slot)
				pg_log_info("created temporary replication slot \"%s\"",
							replication_slot);
			else
				pg_log_info("created replication slot \"%s\"",
							replication_slot);
		}
	}

	if (format == 'p')
	{
		/*
		 * Create pg_wal/archive_status or pg_xlog/archive_status (and thus
		 * pg_wal or pg_xlog) depending on the target server so we can write
		 * to basedir/pg_wal or basedir/pg_xlog as the directory entry in the
		 * tar file may arrive later.
		 */
		snprintf(statusdir, sizeof(statusdir), "%s/%s/archive_status",
				 basedir,
				 PQserverVersion(conn) < MINIMUM_VERSION_FOR_PG_WAL ?
				 "pg_xlog" : "pg_wal");

		if (pg_mkdir_p(statusdir, pg_dir_create_mode) != 0 && errno != EEXIST)
		{
			pg_log_error("could not create directory \"%s\": %m", statusdir);
			exit(1);
		}
	}

	/*
	 * Start a child process and tell it to start streaming. On Unix, this is
	 * a fork(). On Windows, we create a thread.
	 */
#ifndef WIN32
	bgchild = fork();
	if (bgchild == 0)
	{
		/* in child process */
		exit(LogStreamerMain(param));
	}
	else if (bgchild < 0)
	{
		pg_log_error("could not create background process: %m");
		exit(1);
	}

	/*
	 * Else we are in the parent process and all is well.
	 */
	atexit(kill_bgchild_atexit);
#else							/* WIN32 */
	bgchild = _beginthreadex(NULL, 0, (void *) LogStreamerMain, param, 0, NULL);
	if (bgchild == 0)
	{
		pg_log_error("could not create background thread: %m");
		exit(1);
	}
#endif
}

/*
 * Verify that the given directory exists and is empty. If it does not
 * exist, it is created. If it exists but is not empty, an error will
 * be given and the process ended.
 */
static void
verify_dir_is_empty_or_create(char *dirname, bool *created, bool *found)
{
	switch (pg_check_dir(dirname))
	{
		case 0:

			/*
			 * Does not exist, so create
			 */
			if (pg_mkdir_p(dirname, pg_dir_create_mode) == -1)
			{
				pg_log_error("could not create directory \"%s\": %m", dirname);
				exit(1);
			}
			if (created)
				*created = true;
			return;
		case 1:

			/*
			 * Exists, empty
			 */
			if (found)
				*found = true;
			return;
		case 2:
		case 3:
		case 4:

			/*
			 * Exists, not empty
			 *
			 * In GPDB, we may force pg_basebackup to continue even if the
			 * directory already exists. This is needed to preserve important
			 * things that should not be deleted such as pg_log files if we
			 * are doing segment recovery.
			 */
			if (forceoverwrite)
				return;

			pg_log_error("directory \"%s\" exists but is not empty", dirname);
			exit(1);
			break;
		case -1:

			/*
			 * Access problem
			 */
			pg_log_error("could not access directory \"%s\": %m", dirname);
			exit(1);
	}
}


/*
 * Print a progress report based on the global variables. If verbose output
 * is enabled, also print the current file name.
 *
 * Progress report is written at maximum once per second, unless the force
 * parameter is set to true.
 *
 * If finished is set to true, this is the last progress report. The cursor
 * is moved to the next line.
 */
static void
progress_report(int tablespacenum, const char *filename,
				bool force, bool finished)
{
	int			percent;
	char		totaldone_str[32];
	char		totalsize_str[32];
	pg_time_t	now;

	if (!showprogress)
		return;

	now = time(NULL);
	if (now == last_progress_report && !force && !finished)
		return;					/* Max once per second */

	last_progress_report = now;
	percent = totalsize ? (int) ((totaldone / 1024) * 100 / totalsize) : 0;

	/*
	 * Avoid overflowing past 100% or the full size. This may make the total
	 * size number change as we approach the end of the backup (the estimate
	 * will always be wrong if WAL is included), but that's better than having
	 * the done column be bigger than the total.
	 */
	if (percent > 100)
		percent = 100;
	if (totaldone / 1024 > totalsize)
		totalsize = totaldone / 1024;

	/*
	 * Separate step to keep platform-dependent format code out of
	 * translatable strings.  And we only test for INT64_FORMAT availability
	 * in snprintf, not fprintf.
	 */
	snprintf(totaldone_str, sizeof(totaldone_str), INT64_FORMAT,
			 totaldone / 1024);
	snprintf(totalsize_str, sizeof(totalsize_str), INT64_FORMAT, totalsize);

#define VERBOSE_FILENAME_LENGTH 35
	if (verbose)
	{
		if (!filename)

			/*
			 * No filename given, so clear the status line (used for last
			 * call)
			 */
			fprintf(stderr,
					ngettext("%*s/%s kB (100%%), %d/%d tablespace %*s",
							 "%*s/%s kB (100%%), %d/%d tablespaces %*s",
							 tablespacecount),
					(int) strlen(totalsize_str),
					totaldone_str, totalsize_str,
					tablespacenum, tablespacecount,
					VERBOSE_FILENAME_LENGTH + 5, "");
		else
		{
			bool		truncate = (strlen(filename) > VERBOSE_FILENAME_LENGTH);

			fprintf(stderr,
					ngettext("%*s/%s kB (%d%%), %d/%d tablespace (%s%-*.*s)",
							 "%*s/%s kB (%d%%), %d/%d tablespaces (%s%-*.*s)",
							 tablespacecount),
					(int) strlen(totalsize_str),
					totaldone_str, totalsize_str, percent,
					tablespacenum, tablespacecount,
			/* Prefix with "..." if we do leading truncation */
					truncate ? "..." : "",
					truncate ? VERBOSE_FILENAME_LENGTH - 3 : VERBOSE_FILENAME_LENGTH,
					truncate ? VERBOSE_FILENAME_LENGTH - 3 : VERBOSE_FILENAME_LENGTH,
			/* Truncate filename at beginning if it's too long */
					truncate ? filename + strlen(filename) - VERBOSE_FILENAME_LENGTH + 3 : filename);
		}
	}
	else
		fprintf(stderr,
				ngettext("%*s/%s kB (%d%%), %d/%d tablespace",
						 "%*s/%s kB (%d%%), %d/%d tablespaces",
						 tablespacecount),
				(int) strlen(totalsize_str),
				totaldone_str, totalsize_str, percent,
				tablespacenum, tablespacecount);

	/*
	 * Stay on the same line if reporting to a terminal and we're not done
	 * yet.
	 */
	fputc((!finished && isatty(fileno(stderr))) ? '\r' : '\n', stderr);
}

static int32
parse_max_rate(char *src)
{
	double		result;
	char	   *after_num;
	char	   *suffix = NULL;

	errno = 0;
	result = strtod(src, &after_num);
	if (src == after_num)
	{
		pg_log_error("transfer rate \"%s\" is not a valid value", src);
		exit(1);
	}
	if (errno != 0)
	{
		pg_log_error("invalid transfer rate \"%s\": %m", src);
		exit(1);
	}

	if (result <= 0)
	{
		/*
		 * Reject obviously wrong values here.
		 */
		pg_log_error("transfer rate must be greater than zero");
		exit(1);
	}

	/*
	 * Evaluate suffix, after skipping over possible whitespace. Lack of
	 * suffix means kilobytes.
	 */
	while (*after_num != '\0' && isspace((unsigned char) *after_num))
		after_num++;

	if (*after_num != '\0')
	{
		suffix = after_num;
		if (*after_num == 'k')
		{
			/* kilobyte is the expected unit. */
			after_num++;
		}
		else if (*after_num == 'M')
		{
			after_num++;
			result *= 1024.0;
		}
	}

	/* The rest can only consist of white space. */
	while (*after_num != '\0' && isspace((unsigned char) *after_num))
		after_num++;

	if (*after_num != '\0')
	{
		pg_log_error("invalid --max-rate unit: \"%s\"", suffix);
		exit(1);
	}

	/* Valid integer? */
	if ((uint64) result != (uint64) ((uint32) result))
	{
		pg_log_error("transfer rate \"%s\" exceeds integer range", src);
		exit(1);
	}

	/*
	 * The range is checked on the server side too, but avoid the server
	 * connection if a nonsensical value was passed.
	 */
	if (result < MAX_RATE_LOWER || result > MAX_RATE_UPPER)
	{
		pg_log_error("transfer rate \"%s\" is out of range", src);
		exit(1);
	}

	return (int32) result;
}

/*
 * Write a piece of tar data
 */
static void
writeTarData(
#ifdef HAVE_LIBZ
			 gzFile ztarfile,
#endif
			 FILE *tarfile, char *buf, int r, char *current_file)
{
#ifdef HAVE_LIBZ
	if (ztarfile != NULL)
	{
		errno = 0;
		if (gzwrite(ztarfile, buf, r) != r)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			pg_log_error("could not write to compressed file \"%s\": %s",
						 current_file, get_gz_error(ztarfile));
			exit(1);
		}
	}
	else
#endif
	{
		errno = 0;
		if (fwrite(buf, r, 1, tarfile) != 1)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			pg_log_error("could not write to file \"%s\": %m", current_file);
			exit(1);
		}
	}
}

#ifdef HAVE_LIBZ
#define WRITE_TAR_DATA(buf, sz) writeTarData(ztarfile, tarfile, buf, sz, filename)
#else
#define WRITE_TAR_DATA(buf, sz) writeTarData(tarfile, buf, sz, filename)
#endif

/*
 * Receive a tar format file from the connection to the server, and write
 * the data from this file directly into a tar file. If compression is
 * enabled, the data will be compressed while written to the file.
 *
 * The file will be named base.tar[.gz] if it's for the main data directory
 * or <tablespaceoid>.tar[.gz] if it's for another tablespace.
 *
 * No attempt to inspect or validate the contents of the file is done.
 */
static void
ReceiveTarFile(PGconn *conn, PGresult *res, int rownum)
{
	char		filename[MAXPGPATH];
	char	   *copybuf = NULL;
	FILE	   *tarfile = NULL;
	char		tarhdr[512];
	bool		basetablespace = PQgetisnull(res, rownum, 0);
	bool		in_tarhdr = true;
	bool		skip_file = false;
	bool		is_recovery_guc_supported = true;
	bool		is_postgresql_auto_conf = false;
	bool		found_postgresql_auto_conf = false;
	int			file_padding_len = 0;
	size_t		tarhdrsz = 0;
	pgoff_t		filesz = 0;

#ifdef HAVE_LIBZ
	gzFile		ztarfile = NULL;
#endif

	/* recovery.conf is integrated into postgresql.conf in 12 and newer */
	if (PQserverVersion(conn) < MINIMUM_VERSION_FOR_RECOVERY_GUC)
		is_recovery_guc_supported = false;

	if (basetablespace)
	{
		/*
		 * Base tablespaces
		 */
		if (strcmp(basedir, "-") == 0)
		{
#ifdef WIN32
			_setmode(fileno(stdout), _O_BINARY);
#endif

#ifdef HAVE_LIBZ
			if (compresslevel != 0)
			{
				ztarfile = gzdopen(dup(fileno(stdout)), "wb");
				if (gzsetparams(ztarfile, compresslevel,
								Z_DEFAULT_STRATEGY) != Z_OK)
				{
					pg_log_error("could not set compression level %d: %s",
								 compresslevel, get_gz_error(ztarfile));
					exit(1);
				}
			}
			else
#endif
				tarfile = stdout;
			strcpy(filename, "-");
		}
		else
		{
#ifdef HAVE_LIBZ
			if (compresslevel != 0)
			{
				snprintf(filename, sizeof(filename), "%s/base.tar.gz", basedir);
				ztarfile = gzopen(filename, "wb");
				if (gzsetparams(ztarfile, compresslevel,
								Z_DEFAULT_STRATEGY) != Z_OK)
				{
					pg_log_error("could not set compression level %d: %s",
								 compresslevel, get_gz_error(ztarfile));
					exit(1);
				}
			}
			else
#endif
			{
				snprintf(filename, sizeof(filename), "%s/base.tar", basedir);
				tarfile = fopen(filename, "wb");
			}
		}
	}
	else
	{
		/*
		 * Specific tablespace
		 */
#ifdef HAVE_LIBZ
		if (compresslevel != 0)
		{
			snprintf(filename, sizeof(filename), "%s/%s.tar.gz", basedir,
					 PQgetvalue(res, rownum, 0));
			ztarfile = gzopen(filename, "wb");
			if (gzsetparams(ztarfile, compresslevel,
							Z_DEFAULT_STRATEGY) != Z_OK)
			{
				pg_log_error("could not set compression level %d: %s",
							 compresslevel, get_gz_error(ztarfile));
				exit(1);
			}
		}
		else
#endif
		{
			snprintf(filename, sizeof(filename), "%s/%s.tar", basedir,
					 PQgetvalue(res, rownum, 0));
			tarfile = fopen(filename, "wb");
		}
	}

#ifdef HAVE_LIBZ
	if (compresslevel != 0)
	{
		if (!ztarfile)
		{
			/* Compression is in use */
			pg_log_error("could not create compressed file \"%s\": %s",
						 filename, get_gz_error(ztarfile));
			exit(1);
		}
	}
	else
#endif
	{
		/* Either no zlib support, or zlib support but compresslevel = 0 */
		if (!tarfile)
		{
			pg_log_error("could not create file \"%s\": %m", filename);
			exit(1);
		}
	}

	/*
	 * Get the COPY data stream
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COPY_OUT)
	{
		pg_log_error("could not get COPY data stream: %s",
					 PQerrorMessage(conn));
		exit(1);
	}

	while (1)
	{
		int			r;

		if (copybuf != NULL)
		{
			PQfreemem(copybuf);
			copybuf = NULL;
		}

		r = PQgetCopyData(conn, &copybuf, 0);
		if (r == -1)
		{
			/*
			 * End of chunk. If requested, and this is the base tablespace,
			 * write configuration file into the tarfile. When done, close the
			 * file (but not stdout).
			 *
			 * Also, write two completely empty blocks at the end of the tar
			 * file, as required by some tar programs.
			 */
			char		zerobuf[1024];

			MemSet(zerobuf, 0, sizeof(zerobuf));

			if (basetablespace && writerecoveryconf)
			{
				char		header[512];

				/*
				 * If postgresql.auto.conf has not been found in the streamed
				 * data, add recovery configuration to postgresql.auto.conf if
				 * recovery parameters are GUCs.  If the instance connected to
				 * is older than 12, create recovery.conf with this data
				 * otherwise.
				 */
				if (!found_postgresql_auto_conf || !is_recovery_guc_supported)
				{
					int			padding;

					tarCreateHeader(header,
									is_recovery_guc_supported ? "postgresql.auto.conf" : "recovery.conf",
									NULL,
									recoveryconfcontents->len,
									pg_file_create_mode, 04000, 02000,
									time(NULL));

					padding = ((recoveryconfcontents->len + 511) & ~511) - recoveryconfcontents->len;

					WRITE_TAR_DATA(header, sizeof(header));
					WRITE_TAR_DATA(recoveryconfcontents->data,
								   recoveryconfcontents->len);
					if (padding)
						WRITE_TAR_DATA(zerobuf, padding);
				}

				/*
				 * standby.signal is supported only if recovery parameters are
				 * GUCs.
				 */
				if (is_recovery_guc_supported)
				{
					tarCreateHeader(header, "standby.signal", NULL,
									0,	/* zero-length file */
									pg_file_create_mode, 04000, 02000,
									time(NULL));

					WRITE_TAR_DATA(header, sizeof(header));

					/*
					 * we don't need to pad out to a multiple of the tar block
					 * size here, because the file is zero length, which is a
					 * multiple of any block size.
					 */
				}
			}

			/* 2 * 512 bytes empty data at end of file */
			WRITE_TAR_DATA(zerobuf, sizeof(zerobuf));

#ifdef HAVE_LIBZ
			if (ztarfile != NULL)
			{
				errno = 0;		/* in case gzclose() doesn't set it */
				if (gzclose(ztarfile) != 0)
				{
					pg_log_error("could not close compressed file \"%s\": %m",
								 filename);
					exit(1);
				}
			}
			else
#endif
			{
				if (strcmp(basedir, "-") != 0)
				{
					if (fclose(tarfile) != 0)
					{
						pg_log_error("could not close file \"%s\": %m",
									 filename);
						exit(1);
					}
				}
			}

			break;
		}
		else if (r == -2)
		{
			pg_log_error("could not read COPY data: %s",
						 PQerrorMessage(conn));
			exit(1);
		}

		if (!writerecoveryconf || !basetablespace)
		{
			/*
			 * When not writing config file, or when not working on the base
			 * tablespace, we never have to look for an existing configuration
			 * file in the stream.
			 */
			WRITE_TAR_DATA(copybuf, r);
		}
		else
		{
			/*
			 * Look for a config file in the existing tar stream. If it's
			 * there, we must skip it so we can later overwrite it with our
			 * own version of the file.
			 *
			 * To do this, we have to process the individual files inside the
			 * TAR stream. The stream consists of a header and zero or more
			 * chunks, all 512 bytes long. The stream from the server is
			 * broken up into smaller pieces, so we have to track the size of
			 * the files to find the next header structure.
			 */
			int			rr = r;
			int			pos = 0;

			while (rr > 0)
			{
				if (in_tarhdr)
				{
					/*
					 * We're currently reading a header structure inside the
					 * TAR stream, i.e. the file metadata.
					 */
					if (tarhdrsz < 512)
					{
						/*
						 * Copy the header structure into tarhdr in case the
						 * header is not aligned to 512 bytes or it's not
						 * returned in whole by the last PQgetCopyData call.
						 */
						int			hdrleft;
						int			bytes2copy;

						hdrleft = 512 - tarhdrsz;
						bytes2copy = (rr > hdrleft ? hdrleft : rr);

						memcpy(&tarhdr[tarhdrsz], copybuf + pos, bytes2copy);

						rr -= bytes2copy;
						pos += bytes2copy;
						tarhdrsz += bytes2copy;
					}
					else
					{
						/*
						 * We have the complete header structure in tarhdr,
						 * look at the file metadata: we may want append
						 * recovery info into postgresql.auto.conf and skip
						 * standby.signal file if recovery parameters are
						 * integrated as GUCs, and recovery.conf otherwise. In
						 * both cases we must calculate tar padding.
						 */
						if (is_recovery_guc_supported)
						{
							skip_file = (strcmp(&tarhdr[0], "standby.signal") == 0);
							is_postgresql_auto_conf = (strcmp(&tarhdr[0], "postgresql.auto.conf") == 0);
						}
						else
							skip_file = (strcmp(&tarhdr[0], "recovery.conf") == 0);

						filesz = read_tar_number(&tarhdr[124], 12);
						file_padding_len = ((filesz + 511) & ~511) - filesz;

						if (is_recovery_guc_supported &&
							is_postgresql_auto_conf &&
							writerecoveryconf)
						{
							/* replace tar header */
							char		header[512];

							tarCreateHeader(header, "postgresql.auto.conf", NULL,
											filesz + recoveryconfcontents->len,
											pg_file_create_mode, 04000, 02000,
											time(NULL));

							WRITE_TAR_DATA(header, sizeof(header));
						}
						else
						{
							/* copy stream with padding */
							filesz += file_padding_len;

							if (!skip_file)
							{
								/*
								 * If we're not skipping the file, write the
								 * tar header unmodified.
								 */
								WRITE_TAR_DATA(tarhdr, 512);
							}
						}

						/* Next part is the file, not the header */
						in_tarhdr = false;
					}
				}
				else
				{
					/*
					 * We're processing a file's contents.
					 */
					if (filesz > 0)
					{
						/*
						 * We still have data to read (and possibly write).
						 */
						int			bytes2write;

						bytes2write = (filesz > rr ? rr : filesz);

						if (!skip_file)
							WRITE_TAR_DATA(copybuf + pos, bytes2write);

						rr -= bytes2write;
						pos += bytes2write;
						filesz -= bytes2write;
					}
					else if (is_recovery_guc_supported &&
							 is_postgresql_auto_conf &&
							 writerecoveryconf)
					{
						/* append recovery config to postgresql.auto.conf */
						int			padding;
						int			tailsize;

						tailsize = (512 - file_padding_len) + recoveryconfcontents->len;
						padding = ((tailsize + 511) & ~511) - tailsize;

						WRITE_TAR_DATA(recoveryconfcontents->data, recoveryconfcontents->len);

						if (padding)
						{
							char		zerobuf[512];

							MemSet(zerobuf, 0, sizeof(zerobuf));
							WRITE_TAR_DATA(zerobuf, padding);
						}

						/* skip original file padding */
						is_postgresql_auto_conf = false;
						skip_file = true;
						filesz += file_padding_len;

						found_postgresql_auto_conf = true;
					}
					else
					{
						/*
						 * No more data in the current file, the next piece of
						 * data (if any) will be a new file header structure.
						 */
						in_tarhdr = true;
						skip_file = false;
						is_postgresql_auto_conf = false;
						tarhdrsz = 0;
						filesz = 0;
					}
				}
			}
		}
		totaldone += r;
		progress_report(rownum, filename, false, false);
	}							/* while (1) */
	progress_report(rownum, filename, true, false);

	if (copybuf != NULL)
		PQfreemem(copybuf);

	/*
	 * Do not sync the resulting tar file yet, all files are synced once at
	 * the end.
	 */
}


/*
 * Retrieve tablespace path, either relocated or original depending on whether
 * -T was passed or not.
 */
static const char *
get_tablespace_mapping(const char *dir)
{
	TablespaceListCell *cell;
	char		canon_dir[MAXPGPATH];

	/* Canonicalize path for comparison consistency */
	strlcpy(canon_dir, dir, sizeof(canon_dir));
	canonicalize_path(canon_dir);

	for (cell = tablespace_dirs.head; cell; cell = cell->next)
		if (strcmp(canon_dir, cell->old_dir) == 0)
			return cell->new_dir;

	return dir;
}


/*
 * Receive a tar format stream from the connection to the server, and unpack
 * the contents of it into a directory. Only files, directories and
 * symlinks are supported, no other kinds of special files.
 *
 * If the data is for the main data directory, it will be restored in the
 * specified directory. If it's for another tablespace, it will be restored
 * in the original or mapped directory.
 */
static void
ReceiveAndUnpackTarFile(PGconn *conn, PGresult *res, int rownum)
{
	char		current_path[MAXPGPATH];
	char		filename[MAXPGPATH];
	char		gp_tablespace_filename[MAXPGPATH] = {0};
	const char *mapped_tblspc_path;
	pgoff_t		current_len_left = 0;
	int			current_padding = 0;
	bool		basetablespace;
	char	   *copybuf = NULL;
	FILE	   *file = NULL;
	char		log_directory_path[MAXPGPATH];

	basetablespace = PQgetisnull(res, rownum, 0);
	if (basetablespace)
		strlcpy(current_path, basedir, sizeof(current_path));
	else
	{
		strlcpy(current_path,
				get_tablespace_mapping(PQgetvalue(res, rownum, 1)),
				sizeof(current_path));

		if (target_gp_dbid < 1)
		{
			pg_log_error("cannot restore user-defined tablespaces without the --target-gp-dbid option");
			exit(1);
		}
		
		/* 
		 * Construct the new tablespace path using the given target gp dbid
		 */
		snprintf(gp_tablespace_filename, sizeof(filename), "%s/%d/%s",
				current_path,
				target_gp_dbid,
				GP_TABLESPACE_VERSION_DIRECTORY);
	}

	/*
	 * Get the COPY data
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COPY_OUT)
	{
		pg_log_error("could not get COPY data stream: %s",
					 PQerrorMessage(conn));
		exit(1);
	}

	while (1)
	{
		int			r;

		if (copybuf != NULL)
		{
			PQfreemem(copybuf);
			copybuf = NULL;
		}

		r = PQgetCopyData(conn, &copybuf, 0);

		if (r == -1)
		{
			/*
			 * End of chunk
			 */
			if (file)
				fclose(file);

			break;
		}
		else if (r == -2)
		{
			pg_log_error("could not read COPY data: %s",
						 PQerrorMessage(conn));
			exit(1);
		}

		if (file == NULL)
		{
			int			filemode;

			/*
			 * No current file, so this must be the header for a new file
			 */
			if (r != 512)
			{
				pg_log_error("invalid tar block header size: %d", r);
				exit(1);
			}
			totaldone += 512;

			current_len_left = read_tar_number(&copybuf[124], 12);

			/* Set permissions on the file */
			filemode = read_tar_number(&copybuf[100], 8);

			/*
			 * All files are padded up to 512 bytes
			 */
			current_padding =
				((current_len_left + 511) & ~511) - current_len_left;

			/*
			 * First part of header is zero terminated filename
			 */
			if (!basetablespace)
			{
				/*
				 * Append relfile path to --target-gp-dbid tablespace path.
				 *
				 * For example, copybuf can be
				 * "<GP_TABLESPACE_VERSION_DIRECTORY>_db<dbid>/16384/16385".
				 * We create a pointer to the dbid and relfile "/16384/16385",
				 * construct the new tablespace with provided dbid, and append
				 * the dbid and relfile on top.
				 */
				char *copybuf_dbid_relfile = strstr(copybuf, "/");

				snprintf(filename, sizeof(filename), "%s%s",
						 gp_tablespace_filename,
						 copybuf_dbid_relfile);
			}
			else
			{
				snprintf(filename, sizeof(filename), "%s/%s", current_path,
						 copybuf);
			}

			if (filename[strlen(filename) - 1] == '/')
			{
				/*
				 * Ends in a slash means directory or symlink to directory
				 */
				if (copybuf[156] == '5')
				{
					/*
					 * Directory
					 */
					filename[strlen(filename) - 1] = '\0';	/* Remove trailing slash */

					/*
					 * Since the forceoverwrite flag is being used, the
					 * directories still exist. Remove them so that
					 * pg_basebackup can create them. Skip when we detect
					 * pg_log because we want to retain its contents.
					 */
					if (forceoverwrite && pg_check_dir(filename) != 0)
					{
						/*
						 * We want to retain the contents of pg_log. And for
						 * pg_xlog we assume is deleted at the start of
						 * pg_basebackup. We cannot delete pg_xlog because if
						 * streammode was used then it may have already copied
						 * new xlog files into pg_xlog directory.
						 */
						snprintf(log_directory_path, sizeof(log_directory_path), "/%s", log_directory);
						if (pg_str_endswith(filename, "/pg_log") ||
							pg_str_endswith(filename, "/log") ||
							pg_str_endswith(filename, "/pg_wal") ||
							pg_str_endswith(filename, "/pg_xlog") ||
							pg_str_endswith(filename, log_directory_path))
							continue;

						rmtree(filename, true);

					}

					bool is_gp_tablespace_directory = strncmp(gp_tablespace_filename, filename, strlen(filename)) == 0;
					if (is_gp_tablespace_directory && !forceoverwrite) {
						/*
						 * This directory has already been created during beginning of BaseBackup().
						 */
						continue;
					}

					if (mkdir(filename, pg_dir_create_mode) != 0)
					{
						/*
						 * When streaming WAL, pg_wal (or pg_xlog for pre-9.6
						 * clusters) will have been created by the wal
						 * receiver process. Also, when the WAL directory
						 * location was specified, pg_wal (or pg_xlog) has
						 * already been created as a symbolic link before
						 * starting the actual backup. So just ignore creation
						 * failures on related directories.
						 */
						if (!((pg_str_endswith(filename, "/pg_wal") ||
							   pg_str_endswith(filename, "/pg_xlog") ||
							   pg_str_endswith(filename, "/archive_status")) &&
							  errno == EEXIST))
						{
							pg_log_error("could not create directory \"%s\": %m",
										 filename);
							exit(1);
						}
					}
#ifndef WIN32
					if (chmod(filename, (mode_t) filemode))
						pg_log_error("could not set permissions on directory \"%s\": %m",
									 filename);
#endif
				}
				else if (copybuf[156] == '2')
				{
					/*
					 * Symbolic link
					 *
					 * It's most likely a link in pg_tblspc directory, to the
					 * location of a tablespace. Apply any tablespace mapping
					 * given on the command line (--tablespace-mapping). (We
					 * blindly apply the mapping without checking that the
					 * link really is inside pg_tblspc. We don't expect there
					 * to be other symlinks in a data directory, but if there
					 * are, you can call it an undocumented feature that you
					 * can map them too.)
					 */
					filename[strlen(filename) - 1] = '\0';	/* Remove trailing slash */

					mapped_tblspc_path = get_tablespace_mapping(&copybuf[157]);
					char *mapped_tblspc_path_with_dbid = psprintf("%s/%d", mapped_tblspc_path, target_gp_dbid);
					if (symlink(mapped_tblspc_path_with_dbid, filename) != 0)
					{
						pg_log_error("could not create symbolic link from \"%s\" to \"%s\": %m",
									 filename, mapped_tblspc_path);
						exit(1);
					}
					pfree(mapped_tblspc_path_with_dbid);
				}
				else
				{
					pg_log_error("unrecognized link indicator \"%c\"",
								 copybuf[156]);
					exit(1);
				}
				continue;		/* directory or link handled */
			}

			/*
			 * regular file
			 *
			 * In GPDB, we may need to remove the file first if we are forcing
			 * an overwrite instead of starting with a blank directory. Some
			 * files may have had their permissions changed to read only.
			 * Remove the file instead of literally overwriting them.
			 */
			if (forceoverwrite)
				remove(filename);

			file = fopen(filename, "wb");
			if (!file)
			{
				pg_log_error("could not create file \"%s\": %m", filename);
				exit(1);
			}

#ifndef WIN32
			if (chmod(filename, (mode_t) filemode))
				pg_log_error("could not set permissions on file \"%s\": %m",
							 filename);
#endif

			if (current_len_left == 0)
			{
				/*
				 * Done with this file, next one will be a new tar header
				 */
				fclose(file);
				file = NULL;
				continue;
			}
		}						/* new file */
		else
		{
			/*
			 * Continuing blocks in existing file
			 */
			if (current_len_left == 0 && r == current_padding)
			{
				/*
				 * Received the padding block for this file, ignore it and
				 * close the file, then move on to the next tar header.
				 */
				fclose(file);
				file = NULL;
				totaldone += r;
				continue;
			}

			errno = 0;
			if (fwrite(copybuf, r, 1, file) != 1)
			{
				/* if write didn't set errno, assume problem is no disk space */
				if (errno == 0)
					errno = ENOSPC;
				pg_log_error("could not write to file \"%s\": %m", filename);
				exit(1);
			}
			totaldone += r;
			progress_report(rownum, filename, false, false);

			current_len_left -= r;
			if (current_len_left == 0 && current_padding == 0)
			{
				/*
				 * Received the last block, and there is no padding to be
				 * expected. Close the file and move on to the next tar
				 * header.
				 */
				fclose(file);
				file = NULL;
				continue;
			}
		}						/* continuing data in existing file */
	}							/* loop over all data blocks */
	progress_report(rownum, filename, true, false);

	if (file != NULL)
	{
		pg_log_error("COPY stream ended before last file was finished");
		exit(1);
	}

	if (copybuf != NULL)
		PQfreemem(copybuf);

	if (basetablespace && writerecoveryconf)
		WriteRecoveryConfig(conn, basedir, recoveryconfcontents);

	if (basetablespace)
		WriteInternalConfFile();

	/*
	 * No data is synced here, everything is done for all tablespaces at the
	 * end.
	 */
}

static void
add_to_exclude_list(PQExpBufferData *buf, const char *exclude)
{
	char		quoted[MAXPGPATH];
	int			error;
	size_t		len;

	error = 1;
	len = PQescapeStringConn(conn, quoted, exclude, MAXPGPATH, &error);
	if (len == 0 || error != 0)
	{
		pg_log_error("could not process exclude \"%s\": %s",
					 exclude, PQerrorMessage(conn));
		exit(1);
	}
	appendPQExpBuffer(buf, " EXCLUDE '%s'", quoted);
}

static char *
build_exclude_list(void)
{
	PQExpBufferData	buf;
	int				i;

	if (num_exclude == 0 && num_exclude_from == 0)
		return "";

	initPQExpBuffer(&buf);

	for (i = 0; i < num_exclude; i++)
		add_to_exclude_list(&buf, excludes[i]);

	for (i = 0; i < num_exclude_from; i++)
	{
		const char *filename = excludefroms[i];
		FILE	   *file = fopen(filename, "r");
		char		str[MAXPGPATH];

		if (file == NULL)
		{
			pg_log_error("could not open exclude-from file \"%s\": %m",
						 filename);
			exit(1);
		}

		/*
		 * Each line contains a pathname to exclude.
		 *
		 * We must use fgets() instead of fscanf("%s") to correctly handle the
		 * spaces in the filenames.
		 */
		while (fgets(str, sizeof(str), file))
		{
			/* Remove all trailing \r and \n */
			for (int len = strlen(str);
				 len > 0 && (str[len - 1] == '\r' || str[len - 1] == '\n');
				 len--)
				str[len - 1] = '\0';

			add_to_exclude_list(&buf, str);
		}

		fclose(file);
	}

	if (PQExpBufferDataBroken(buf))
	{
		pg_log_error("out of memory");
		exit(1);
	}

	return buf.data;
}

static void
GetLogDirectory(PGconn *conn)
{
	log_directory = run_simple_query(conn, "SHOW log_directory");
}

static void
BaseBackup(void)
{
	PGresult   *res;
	char	   *sysidentifier;
	TimeLineID	latesttli;
	TimeLineID	starttli;
	char	   *basebkp;
	char		escaped_label[MAXPGPATH];
	char	   *maxrate_clause = NULL;
	int			i;
	char		xlogstart[64];
	char		xlogend[64];
	int			minServerMajor,
				maxServerMajor;
	char 	   *exclude_list;
	int			serverVersion,
				serverMajor;

	Assert(conn != NULL);

	/*
	 * Check server version. BASE_BACKUP command was introduced in 9.1, so we
	 * can't work with servers older than 9.1.
	 */
	minServerMajor = 901;
	maxServerMajor = PG_VERSION_NUM / 100;
	serverVersion = PQserverVersion(conn);
	serverMajor = serverVersion / 100;
	if (serverMajor < minServerMajor || serverMajor > maxServerMajor)
	{
		const char *serverver = PQparameterStatus(conn, "server_version");

		pg_log_error("incompatible server version %s",
					 serverver ? serverver : "'unknown'");
		exit(1);
	}

	/*
	 * If WAL streaming was requested, also check that the server is new
	 * enough for that.
	 */
	if (includewal == STREAM_WAL && !CheckServerVersionForStreaming(conn))
	{
		/*
		 * Error message already written in CheckServerVersionForStreaming(),
		 * but add a hint about using -X none.
		 */
		pg_log_info("HINT: use -X none or -X fetch to disable log streaming");
		exit(1);
	}

	/*
	 * Build contents of configuration file if requested
	 */
	if (writerecoveryconf)
		recoveryconfcontents = GenerateRecoveryConfig(conn, replication_slot);

	/*
	 * Run IDENTIFY_SYSTEM so we can get the timeline
	 */
	if (!RunIdentifySystem(conn, &sysidentifier, &latesttli, NULL, NULL))
		exit(1);

	/*
	 * Start the actual backup
	 */
	PQescapeStringConn(conn, escaped_label, label, sizeof(escaped_label), &i);

	if (maxrate > 0)
		maxrate_clause = psprintf("MAX_RATE %u", maxrate);

	exclude_list = build_exclude_list();

	if (verbose)
		pg_log_info("initiating base backup, waiting for checkpoint to complete");

	if (showprogress && !verbose)
	{
		fprintf(stderr, "waiting for checkpoint");
		if (isatty(fileno(stderr)))
			fprintf(stderr, "\r");
		else
			fprintf(stderr, "\n");
	}

	basebkp =
		psprintf("BASE_BACKUP LABEL '%s' %s %s %s %s %s %s %s %s",
				 escaped_label,
				 showprogress ? "PROGRESS" : "",
				 includewal == FETCH_WAL ? "WAL" : "",
				 fastcheckpoint ? "FAST" : "",
				 includewal == NO_WAL ? "" : "NOWAIT",
				 maxrate_clause ? maxrate_clause : "",
				 format == 't' ? "TABLESPACE_MAP" : "",
				 verify_checksums ? "" : "NOVERIFY_CHECKSUMS",
				 exclude_list);

	if (exclude_list[0] != '\0')
		free(exclude_list);

	if (PQsendQuery(conn, basebkp) == 0)
	{
		pg_log_error("could not send replication command \"%s\": %s",
					 "BASE_BACKUP", PQerrorMessage(conn));
		exit(1);
	}

	/*
	 * Get the starting WAL location
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not initiate base backup: %s",
					 PQerrorMessage(conn));
		exit(1);
	}
	if (PQntuples(res) != 1)
	{
		pg_log_error("server returned unexpected response to BASE_BACKUP command; got %d rows and %d fields, expected %d rows and %d fields",
					 PQntuples(res), PQnfields(res), 1, 2);
		exit(1);
	}

	strlcpy(xlogstart, PQgetvalue(res, 0, 0), sizeof(xlogstart));

	if (verbose)
		pg_log_info("checkpoint completed");

	/*
	 * 9.3 and later sends the TLI of the starting point. With older servers,
	 * assume it's the same as the latest timeline reported by
	 * IDENTIFY_SYSTEM.
	 */
	if (PQnfields(res) >= 2)
		starttli = atoi(PQgetvalue(res, 0, 1));
	else
		starttli = latesttli;
	PQclear(res);
	MemSet(xlogend, 0, sizeof(xlogend));

	if (verbose && includewal != NO_WAL)
		pg_log_info("write-ahead log start point: %s on timeline %u",
					xlogstart, starttli);

	/*
	 * Get the header
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not get backup header: %s",
					 PQerrorMessage(conn));
		exit(1);
	}
	if (PQntuples(res) < 1)
	{
		pg_log_error("no data returned from server");
		exit(1);
	}

	/*
	 * Sum up the total size, for progress reporting
	 */
	totalsize = totaldone = 0;
	tablespacecount = PQntuples(res);
	for (i = 0; i < PQntuples(res); i++)
	{
		totalsize += atol(PQgetvalue(res, i, 2));

		/*
		 * Verify tablespace directories are empty. Don't bother with the
		 * first once since it can be relocated, and it will be checked before
		 * we do anything anyway.
		 */
		if (format == 'p' && !PQgetisnull(res, i, 1))
		{
			char	   *path = unconstify(char *, get_tablespace_mapping(PQgetvalue(res, i, 1)));
			char path_with_subdir[MAXPGPATH];

			snprintf(path_with_subdir, MAXPGPATH, "%s/%d/%s", path, target_gp_dbid, GP_TABLESPACE_VERSION_DIRECTORY);

			verify_dir_is_empty_or_create(path_with_subdir, &made_tablespace_dirs, &found_tablespace_dirs);
		}
	}

	/*
	 * When writing to stdout, require a single tablespace
	 */
	if (format == 't' && strcmp(basedir, "-") == 0 && PQntuples(res) > 1)
	{
		pg_log_error("can only write single tablespace to stdout, database has %d",
					 PQntuples(res));
		exit(1);
	}

	/*
	 * In the case of forceoverwrite the base directory may already exist. In
	 * this case we need to wipeout the old pg_xlog directory. This is done
	 * before StartLogStreamer and ReceiveAndUnpackTarFile so that either can
	 * create pg_xlog directory and begin populating new contents to it.
	 */
	if (forceoverwrite)
	{
		char xlog_path[MAXPGPATH];
		snprintf(xlog_path, MAXPGPATH, "%s/%s", basedir,
			PQserverVersion(conn) < MINIMUM_VERSION_FOR_PG_WAL ? "pg_xlog" : "pg_wal");

		if (pg_check_dir(xlog_path) != 0)
			rmtree(xlog_path, true);
	}

	/*
	 * If we're streaming WAL, start the streaming session before we start
	 * receiving the actual data chunks.
	 */
	if (includewal == STREAM_WAL)
	{
		if (verbose)
			pg_log_info("starting background WAL receiver");
		StartLogStreamer(xlogstart, starttli, sysidentifier);
	}

	/*
	 * Start receiving chunks
	 */
	for (i = 0; i < PQntuples(res); i++)
	{
		if (format == 't')
			ReceiveTarFile(conn, res, i);
		else
			ReceiveAndUnpackTarFile(conn, res, i);
	}							/* Loop over all tablespaces */

	if (showprogress)
		progress_report(PQntuples(res), NULL, true, true);

	PQclear(res);

	/*
	 * Get the stop position
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not get write-ahead log end position from server: %s",
					 PQerrorMessage(conn));
		exit(1);
	}
	if (PQntuples(res) != 1)
	{
		pg_log_error("no write-ahead log end position returned from server");
		exit(1);
	}
	strlcpy(xlogend, PQgetvalue(res, 0, 0), sizeof(xlogend));
	if (verbose && includewal != NO_WAL)
		pg_log_info("write-ahead log end point: %s", xlogend);
	PQclear(res);

	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);

		if (sqlstate &&
			strcmp(sqlstate, ERRCODE_DATA_CORRUPTED) == 0)
		{
			pg_log_error("checksum error occurred");
			checksum_failure = true;
		}
		else
		{
			pg_log_error("final receive failed: %s",
						 PQerrorMessage(conn));
		}
		exit(1);
	}

	if (bgchild > 0)
	{
#ifndef WIN32
		int			status;
		pid_t		r;
#else
		DWORD		status;

		/*
		 * get a pointer sized version of bgchild to avoid warnings about
		 * casting to a different size on WIN64.
		 */
		intptr_t	bgchild_handle = bgchild;
		uint32		hi,
					lo;
#endif

		if (verbose)
			pg_log_info("waiting for background process to finish streaming ...");

#ifndef WIN32
		if (write(bgpipe[1], xlogend, strlen(xlogend)) != strlen(xlogend))
		{
			pg_log_info("could not send command to background pipe: %m");
			exit(1);
		}

		/* Just wait for the background process to exit */
		r = waitpid(bgchild, &status, 0);
		if (r == (pid_t) -1)
		{
			pg_log_error("could not wait for child process: %m");
			exit(1);
		}
		if (r != bgchild)
		{
			pg_log_error("child %d died, expected %d", (int) r, (int) bgchild);
			exit(1);
		}
		if (status != 0)
		{
			pg_log_error("%s", wait_result_to_str(status));
			exit(1);
		}
		/* Exited normally, we're happy! */
#else							/* WIN32 */

		/*
		 * On Windows, since we are in the same process, we can just store the
		 * value directly in the variable, and then set the flag that says
		 * it's there.
		 */
		if (sscanf(xlogend, "%X/%X", &hi, &lo) != 2)
		{
			pg_log_error("could not parse write-ahead log location \"%s\"",
						 xlogend);
			exit(1);
		}
		xlogendptr = ((uint64) hi) << 32 | lo;
		InterlockedIncrement(&has_xlogendptr);

		/* First wait for the thread to exit */
		if (WaitForSingleObjectEx((HANDLE) bgchild_handle, INFINITE, FALSE) !=
			WAIT_OBJECT_0)
		{
			_dosmaperr(GetLastError());
			pg_log_error("could not wait for child thread: %m");
			exit(1);
		}
		if (GetExitCodeThread((HANDLE) bgchild_handle, &status) == 0)
		{
			_dosmaperr(GetLastError());
			pg_log_error("could not get child thread exit status: %m");
			exit(1);
		}
		if (status != 0)
		{
			pg_log_error("child thread exited with error %u",
						 (unsigned int) status);
			exit(1);
		}
		/* Exited normally, we're happy */
#endif
	}

	/* Free the configuration file contents */
	destroyPQExpBuffer(recoveryconfcontents);

	/*
	 * End of copy data. Final result is already checked inside the loop.
	 */
	PQclear(res);
	PQfinish(conn);
	conn = NULL;

	/*
	 * Make data persistent on disk once backup is completed. For tar format
	 * sync the parent directory and all its contents as each tar file was not
	 * synced after being completed.  In plain format, all the data of the
	 * base directory is synced, taking into account all the tablespaces.
	 * Errors are not considered fatal.
	 */
	if (do_sync)
	{
		if (verbose)
			pg_log_info("syncing data to disk ...");
		if (format == 't')
		{
			if (strcmp(basedir, "-") != 0)
				(void) fsync_dir_recurse(basedir);
		}
		else
		{
			(void) fsync_pgdata(basedir, serverVersion);
		}
	}

	if (verbose)
		pg_log_info("base backup completed");
}


int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"pgdata", required_argument, NULL, 'D'},
		{"format", required_argument, NULL, 'F'},
		{"checkpoint", required_argument, NULL, 'c'},
		{"create-slot", no_argument, NULL, 'C'},
		{"max-rate", required_argument, NULL, 'r'},
		{"write-recovery-conf", no_argument, NULL, 'R'},
		{"write-conf-files-only", no_argument, NULL, 'o'},
		{"slot", required_argument, NULL, 'S'},
		{"tablespace-mapping", required_argument, NULL, 'T'},
		{"wal-method", required_argument, NULL, 'X'},
		{"gzip", no_argument, NULL, 'z'},
		{"compress", required_argument, NULL, 'Z'},
		{"label", required_argument, NULL, 'l'},
		{"no-clean", no_argument, NULL, 'n'},
		{"no-sync", no_argument, NULL, 'N'},
		{"dbname", required_argument, NULL, 'd'},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"status-interval", required_argument, NULL, 's'},
		{"verbose", no_argument, NULL, 'v'},
		{"progress", no_argument, NULL, 'P'},
		{"waldir", required_argument, NULL, 1},
		{"no-slot", no_argument, NULL, 2},
		{"no-verify-checksums", no_argument, NULL, 3},
		{"exclude", required_argument, NULL, 'E'},
		{"force-overwrite", no_argument, NULL, 128},
		{"target-gp-dbid", required_argument, NULL, 129},
		{"exclude-from", required_argument, NULL, 130},
		{NULL, 0, NULL, 0}
	};
	int			c;

	int			option_index;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_basebackup"));

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		else if (strcmp(argv[1], "-V") == 0
				 || strcmp(argv[1], "--version") == 0)
		{
			puts("pg_basebackup (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	num_exclude = 0;
	num_exclude_from = 0;
	atexit(cleanup_directories_atexit);

	while ((c = getopt_long(argc, argv, "CD:F:o:r:RT:xX:l:zZ:d:c:h:p:U:s:S:wWvPE:",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'C':
				create_slot = true;
				break;
			case 'D':
				basedir = pg_strdup(optarg);
				break;
			case 'F':
				if (strcmp(optarg, "p") == 0 || strcmp(optarg, "plain") == 0)
					format = 'p';
				else if (strcmp(optarg, "t") == 0 || strcmp(optarg, "tar") == 0)
					format = 't';
				else
				{
					pg_log_error("invalid output format \"%s\", must be \"plain\" or \"tar\"",
								 optarg);
					exit(1);
				}
				break;
			case 'o':
				writeconffilesonly = true;
				break;
			case 'r':
				maxrate = parse_max_rate(optarg);
				break;
			case 'R':
				writerecoveryconf = true;
				break;
			case 'S':

				/*
				 * When specifying replication slot name, use a permanent
				 * slot.
				 */
				replication_slot = pg_strdup(optarg);
				temp_replication_slot = false;
				break;
			case 2:
				no_slot = true;
				break;
			case 'T':
				tablespace_list_append(optarg);
				break;
			case 'X':
				if (strcmp(optarg, "n") == 0 ||
					strcmp(optarg, "none") == 0)
				{
					includewal = NO_WAL;
				}
				else if (strcmp(optarg, "f") == 0 ||
						 strcmp(optarg, "fetch") == 0)
				{
					includewal = FETCH_WAL;
				}
				else if (strcmp(optarg, "s") == 0 ||
						 strcmp(optarg, "stream") == 0)
				{
					includewal = STREAM_WAL;
				}
				else
				{
					pg_log_error("invalid wal-method option \"%s\", must be \"fetch\", \"stream\", or \"none\"",
								 optarg);
					exit(1);
				}
				break;
			case 1:
				xlog_dir = pg_strdup(optarg);
				break;
			case 'l':
				label = pg_strdup(optarg);
				break;
			case 'n':
				noclean = true;
				break;
			case 'N':
				do_sync = false;
				break;
			case 'z':
#ifdef HAVE_LIBZ
				compresslevel = Z_DEFAULT_COMPRESSION;
#else
				compresslevel = 1;	/* will be rejected below */
#endif
				break;
			case 'Z':
				compresslevel = atoi(optarg);
				if (compresslevel < 0 || compresslevel > 9)
				{
					pg_log_error("invalid compression level \"%s\"", optarg);
					exit(1);
				}
				break;
			case 'c':
				if (pg_strcasecmp(optarg, "fast") == 0)
					fastcheckpoint = true;
				else if (pg_strcasecmp(optarg, "spread") == 0)
					fastcheckpoint = false;
				else
				{
					pg_log_error("invalid checkpoint argument \"%s\", must be \"fast\" or \"spread\"",
								 optarg);
					exit(1);
				}
				break;
			case 'd':
				connection_string = pg_strdup(optarg);
				break;
			case 'h':
				dbhost = pg_strdup(optarg);
				break;
			case 'p':
				dbport = pg_strdup(optarg);
				break;
			case 'U':
				dbuser = pg_strdup(optarg);
				break;
			case 'w':
				dbgetpassword = -1;
				break;
			case 'W':
				dbgetpassword = 1;
				break;
			case 's':
				standby_message_timeout = atoi(optarg) * 1000;
				if (standby_message_timeout < 0)
				{
					pg_log_error("invalid status interval \"%s\"", optarg);
					exit(1);
				}
				break;
			case 'v':
				verbose++;
				break;
			case 'P':
				showprogress = true;
				break;
			case 3:
				verify_checksums = false;
				break;
			case 'E':
				if (num_exclude >= MAX_EXCLUDE)
				{
					pg_log_error("too many elements in exclude list: max is %d",
								 MAX_EXCLUDE);
					pg_log_error("HINT: use --exclude-from to load a large exclude list from a file");
					exit(1);
				}

				excludes[num_exclude++] = pg_strdup(optarg);
				break;
			case 128:
				forceoverwrite = true;
				break;
			case 129:
				target_gp_dbid = atoi(optarg);
				break;
			case 130:			/* --exclude-from=FILE */
				if (num_exclude_from >= MAX_EXCLUDE)
				{
					pg_log_error("too many elements in exclude-from list: max is %d",
								 MAX_EXCLUDE);
					exit(1);
				}

				excludefroms[num_exclude_from++] = pg_strdup(optarg);
				break;
			default:

				/*
				 * getopt_long already emitted a complaint
				 */
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
						progname);
				exit(1);
		}
	}

	/*
	 * Any non-option arguments?
	 */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/*
	 * Required arguments
	 */
	if (basedir == NULL)
	{
		pg_log_error("no target directory specified");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	if (target_gp_dbid <= 0)
	{
		pg_log_error("no target dbid specified, --target-gp-dbid is required");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/*
	 * Mutually exclusive arguments
	 */
	if (format == 'p' && compresslevel != 0)
	{
		pg_log_error("only tar mode backups can be compressed");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	if (format == 't' && includewal == STREAM_WAL && strcmp(basedir, "-") == 0)
	{
		pg_log_error("cannot stream write-ahead logs in tar mode to stdout");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	if (replication_slot && includewal != STREAM_WAL)
	{
		pg_log_error("replication slots can only be used with WAL streaming");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	if (no_slot)
	{
		if (replication_slot)
		{
			pg_log_error("--no-slot cannot be used with slot name");
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
					progname);
			exit(1);
		}
		temp_replication_slot = false;
	}

	if (create_slot)
	{
		if (!replication_slot)
		{
			pg_log_error("%s needs a slot to be specified using --slot",
						 "--create-slot");
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
					progname);
			exit(1);
		}

		if (no_slot)
		{
			pg_log_error("--create-slot and --no-slot are incompatible options");
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
					progname);
			exit(1);
		}
	}

	if (xlog_dir)
	{
		if (format != 'p')
		{
			pg_log_error("WAL directory location can only be specified in plain mode");
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
					progname);
			exit(1);
		}

		/* clean up xlog directory name, check it's absolute */
		canonicalize_path(xlog_dir);
		if (!is_absolute_path(xlog_dir))
		{
			pg_log_error("WAL directory location must be an absolute path");
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
					progname);
			exit(1);
		}
	}

	if (writeconffilesonly)
	{
		if(create_slot)
		{
			pg_log_error("--create_slot cannot be used with --write-conf-files-only");
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
					progname);
			exit(1);
		}

		if(writerecoveryconf)
		{
			pg_log_error("--write-recovery-conf cannot be used with --write-conf-files-only");
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
					progname);
			exit(1);
		}
	}

#ifndef HAVE_LIBZ
	if (compresslevel != 0)
	{
		pg_log_error("this build does not support compression");
		exit(1);
	}
#endif

	/* connection in replication mode to server */
	conn = GetConnection();
	if (!conn)
	{
		/* Error message already written in GetConnection() */
		exit(1);
	}
	atexit(disconnect_atexit);

	/* To only write recovery.conf and internal.auto.conf files,
	   one of the usecase is gprecoverseg differential recovery (there can be others in future)
	*/
	if (writeconffilesonly)
	{
		WriteInternalConfFile();
		WriteRecoveryConfig(conn, basedir, GenerateRecoveryConfig(conn, replication_slot));
		success = true;
		return 0;
	}

	/*
	 * Set umask so that directories/files are created with the same
	 * permissions as directories/files in the source data directory.
	 *
	 * pg_mode_mask is set to owner-only by default and then updated in
	 * GetConnection() where we get the mode from the server-side with
	 * RetrieveDataDirCreatePerm() and then call SetDataDirectoryCreatePerm().
	 */
	umask(pg_mode_mask);

	/*
	 * Verify that the target directory exists, or create it. For plaintext
	 * backups, always require the directory. For tar backups, require it
	 * unless we are writing to stdout.
	 */
	if (format == 'p' || strcmp(basedir, "-") != 0)
		verify_dir_is_empty_or_create(basedir, &made_new_pgdata, &found_existing_pgdata);

	/*
	 * GPDB: Backups in tar mode will not have the internal.auto.conf file,
	 * nor will any tablespaces have the dbid appended to their symlinks in
	 * pg_tblspc. The backups are still, in theory, valid, but the tablespace
	 * mapping and internal.auto.conf files will need to be added manually
	 * when extracting the backups.
	 */
	if (format == 't')
		pg_log_error("WARNING: tar backups are not supported on GPDB");

	/* determine remote server's xlog segment size */
	if (!RetrieveWalSegSize(conn))
		exit(1);

	/* Create pg_wal symlink, if required */
	if (xlog_dir)
	{
		char	   *linkloc;

		verify_dir_is_empty_or_create(xlog_dir, &made_new_xlogdir, &found_existing_xlogdir);

		/*
		 * Form name of the place where the symlink must go. pg_xlog has been
		 * renamed to pg_wal in post-10 clusters.
		 */
		linkloc = psprintf("%s/%s", basedir,
						   PQserverVersion(conn) < MINIMUM_VERSION_FOR_PG_WAL ?
						   "pg_xlog" : "pg_wal");

#ifdef HAVE_SYMLINK
		if (symlink(xlog_dir, linkloc) != 0)
		{
			pg_log_error("could not create symbolic link \"%s\": %m", linkloc);
			exit(1);
		}
#else
		pg_log_error("symlinks are not supported on this platform");
		exit(1);
#endif
		free(linkloc);
	}

	GetLogDirectory(conn);

	BaseBackup();

	success = true;
	return 0;
}

static void
WriteInternalConfFile(void)
{
	char		filename[MAXPGPATH];
	FILE	   *cf;
	char line_to_write[100];
	int length;

	sprintf(filename, "%s/%s", basedir, GP_INTERNAL_AUTO_CONF_FILE_NAME);

	cf = fopen(filename, "w");
	if (cf == NULL)
	{
		pg_log_error("could not create file \"%s\": %m", filename);
		exit(1);
	}

	length = snprintf(line_to_write, 100, "gp_dbid=%d\n", target_gp_dbid);

	if (fwrite(line_to_write, length, 1, cf) != 1)
	{
		pg_log_error("could not write to file \"%s\": %m", filename);
		exit(1);
	}

	fclose(cf);
}

static char *
run_simple_query(PGconn *conn, const char *sql)
{
	PGresult   *res;
	char	   *result;

	res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_log_error("error running query (%s) in source server: %s",
				 sql, PQresultErrorMessage(res));

	/* sanity check the result set */
	if (PQnfields(res) != 1 || PQntuples(res) != 1 || PQgetisnull(res, 0, 0))
		pg_log_error("unexpected result set from query");

	result = pg_strdup(PQgetvalue(res, 0, 0));

	PQclear(res);

	return result;
}
