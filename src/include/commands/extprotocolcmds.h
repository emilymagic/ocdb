/*-------------------------------------------------------------------------
 *
 * extprotocolcmds.h
 *	  prototypes for extprotocolcmds.c.
 *
 *-------------------------------------------------------------------------
 */

#ifndef EXTPROTOCOLCMDS_H
#define EXTPROTOCOLCMDS_H

#include "nodes/pg_list.h"

extern void RemoveExtProtocolById(Oid protOid);

#endif   /* EXTPROTOCOLCMDS_H */
