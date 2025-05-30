/*-------------------------------------------------------------------------
 *
 * dest.c
 *	  support for communication destinations
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/tcop/dest.c
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		BeginCommand - initialize the destination at start of command
 *		CreateDestReceiver - create tuple receiver object for destination
 *		EndCommand - clean up the destination at end of command
 *		NullCommand - tell dest that an empty query string was recognized
 *		ReadyForQuery - tell dest that we are ready for a new query
 *
 *	 NOTES
 *		These routines do the appropriate work before and after
 *		tuples are returned by a query to keep the backend and the
 *		"destination" portals synchronized.
 */

#include "postgres.h"

#include "access/printsimple.h"
#include "access/printtup.h"
#include "access/xact.h"
#include "commands/copy.h"
#include "commands/createas.h"
#include "commands/matview.h"
#include "executor/functions.h"
#include "executor/tqueue.h"
#include "executor/tstoreReceiver.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "utils/portal.h"

#include "cdb/cdbvars.h"
#include "utils/vmem_tracker.h"

/* ----------------
 *		dummy DestReceiver functions
 * ----------------
 */
static bool
donothingReceive(TupleTableSlot *slot, DestReceiver *self)
{
	return true;
}

static void
donothingStartup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
}

static void
donothingCleanup(DestReceiver *self)
{
	/* this is used for both shutdown and destroy methods */
}

/* ----------------
 *		static DestReceiver structs for dest types needing no local state
 * ----------------
 */
static const DestReceiver donothingDR = {
	donothingReceive, donothingStartup, donothingCleanup, donothingCleanup,
	DestNone
};

static const DestReceiver debugtupDR = {
	debugtup, debugStartup, donothingCleanup, donothingCleanup,
	DestDebug
};

static const DestReceiver printsimpleDR = {
	printsimple, printsimple_startup, donothingCleanup, donothingCleanup,
	DestRemoteSimple
};

static const DestReceiver spi_printtupDR = {
	spi_printtup, spi_dest_startup, donothingCleanup, donothingCleanup,
	DestSPI
};

/*
 * Globally available receiver for DestNone.
 *
 * It's ok to cast the constness away as any modification of the none receiver
 * would be a bug (which gets easier to catch this way).
 */
DestReceiver *None_Receiver = (DestReceiver *) &donothingDR;

/* ----------------
 *		BeginCommand - initialize the destination at start of command
 * ----------------
 */
void
BeginCommand(const char *commandTag, CommandDest dest)
{
	/* Nothing to do at present */
}

/* ----------------
 *		CreateDestReceiver - return appropriate receiver function set for dest
 * ----------------
 */
DestReceiver *
CreateDestReceiver(CommandDest dest)
{
	/*
	 * It's ok to cast the constness away as any modification of the none
	 * receiver would be a bug (which gets easier to catch this way).
	 */

	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
			return printtup_create_DR(dest);

		case DestRemoteSimple:
			return unconstify(DestReceiver *, &printsimpleDR);

		case DestNone:
			return unconstify(DestReceiver *, &donothingDR);

		case DestDebug:
			return unconstify(DestReceiver *, &debugtupDR);

		case DestSPI:
			return unconstify(DestReceiver *, &spi_printtupDR);

		case DestTuplestore:
			return CreateTuplestoreDestReceiver();

		case DestIntoRel:
			return CreateIntoRelDestReceiver(NULL);

		case DestCopyOut:
			return CreateCopyDestReceiver();

		case DestSQLFunction:
			return CreateSQLFunctionDestReceiver();

		case DestTransientRel:
			return CreateTransientRelDestReceiver(InvalidOid, InvalidOid, false, 't', false);

		case DestTupleQueue:
			return CreateTupleQueueDestReceiver(NULL);
	}

	/* should never get here */
	pg_unreachable();
}

/* ----------------
 *		EndCommand - clean up the destination at end of command
 * ----------------
 */
void
EndCommand(const char *commandTag, CommandDest dest)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:

			/*
			 * We assume the commandTag is plain ASCII and therefore requires
			 * no encoding conversion.
			 */
			pq_putmessage('C', commandTag, strlen(commandTag) + 1);
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
			break;
	}
}

/* ----------------
 *		NullCommand - tell dest that an empty query string was recognized
 *
 *		In FE/BE protocol version 1.0, this hack is necessary to support
 *		libpq's crufty way of determining whether a multiple-command
 *		query string is done.  In protocol 2.0 it's probably not really
 *		necessary to distinguish empty queries anymore, but we still do it
 *		for backwards compatibility with 1.0.  In protocol 3.0 it has some
 *		use again, since it ensures that there will be a recognizable end
 *		to the response to an Execute message.
 * ----------------
 */
void
NullCommand(CommandDest dest)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:

			/*
			 * tell the fe that we saw an empty query string.  In protocols
			 * before 3.0 this has a useless empty-string message body.
			 */
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
				pq_putemptymessage('I');
			else
				pq_putmessage('I', "", 1);
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
			break;
	}
}

/* ----------------
 *		ReadyForQuery - tell dest that we are ready for a new query
 *
 *		The ReadyForQuery message is sent so that the FE can tell when
 *		we are done processing a query string.
 *		In versions 3.0 and up, it also carries a transaction state indicator.
 *
 *		Note that by flushing the stdio buffer here, we can avoid doing it
 *		most other places and thus reduce the number of separate packets sent.
 * ----------------
 */
void
ReadyForQuery(CommandDest dest)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
			{
				StringInfoData buf;

				if (Gp_role == GP_ROLE_EXECUTE)
				{
					pq_beginmessage(&buf, 'k'); /* mop_high_watermark */
					pq_sendint64(&buf, VmemTracker_GetMaxReservedVmemBytes());
					pq_endmessage(&buf);
				}

				pq_beginmessage(&buf, 'Z');
				pq_sendbyte(&buf, TransactionBlockStatusCode());
				pq_endmessage(&buf);
			}
			else
				pq_putemptymessage('Z');
			/* Flush output at end of cycle in any case. */
			pq_flush();
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
			break;
	}
}

/*
 * Send a gpdb libpq message.
 *
 * This sends a message identical to that used when sending values of
 * GUC_REPORT gucs to the client (see ReportGUCOption()). The motion
 * listener port is sent as if there was a GUC called "qe_listener_port".
 */
void
sendQEDetails(void)
{
	StringInfoData msgbuf;
	char		port_str[11];

	snprintf(port_str, sizeof(port_str), "%u", Gp_listener_port);

	pq_beginmessage(&msgbuf, 'S');
	pq_sendstring(&msgbuf, "qe_listener_port");
	pq_sendstring(&msgbuf, port_str);
	pq_endmessage(&msgbuf);
}
