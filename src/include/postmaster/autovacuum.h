/*-------------------------------------------------------------------------
 *
 * autovacuum.h
 *	  header file for integrated autovacuum daemon
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/autovacuum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTOVACUUM_H
#define AUTOVACUUM_H

#include "storage/block.h"

/*
 * Other processes can request specific work from autovacuum, identified by
 * AutoVacuumWorkItem elements.
 */
typedef enum
{
	AVW_BRINSummarizeRange,
	AVW_UpdateRootPartitionStats
} AutoVacuumWorkItemType;


/* GUC variables */
extern bool autovacuum_start_daemon;
extern int	autovacuum_max_workers;
extern int	autovacuum_work_mem;
extern int	autovacuum_naptime;
extern int	autovacuum_vac_thresh;
extern double autovacuum_vac_scale;
extern int	autovacuum_anl_thresh;
extern double autovacuum_anl_scale;
extern int	autovacuum_freeze_max_age;
extern int	autovacuum_multixact_freeze_max_age;
extern double autovacuum_vac_cost_delay;
extern int	autovacuum_vac_cost_limit;

/*Allowed values for gp_autovacuum_scope*/
typedef enum AutovacuumScope
{
	AV_SCOPE_CATALOG,
	AV_SCOPE_CATALOG_AO_AUX,
} AutovacuumScope;

/* autovacuum launcher PID, only valid when worker is shutting down */
extern int	AutovacuumLauncherPid;

extern int  Log_autovacuum_min_duration;

/* Status inquiry functions */
extern bool AutoVacuumingActive(void);
extern bool IsAutoVacuumLauncherProcess(void);
extern bool IsAutoVacuumWorkerProcess(void);

#define IsAnyAutoVacuumProcess() \
	(IsAutoVacuumLauncherProcess() || IsAutoVacuumWorkerProcess())

/* Functions to start autovacuum process, called from postmaster */
extern void autovac_init(void);
extern int	StartAutoVacLauncher(void);
extern int	StartAutoVacWorker(void);

/* called from postmaster when a worker could not be forked */
extern void AutoVacWorkerFailed(void);

/* autovacuum cost-delay balancer */
extern void AutoVacuumUpdateDelay(void);

#ifdef EXEC_BACKEND
extern void AutoVacLauncherMain(int argc, char *argv[]) pg_attribute_noreturn();
extern void AutoVacWorkerMain(int argc, char *argv[]) pg_attribute_noreturn();
extern void AutovacuumWorkerIAm(void);
extern void AutovacuumLauncherIAm(void);
#endif

extern bool AutoVacuumRequestWork(AutoVacuumWorkItemType type,
								  Oid relationId, BlockNumber blkno);

/* shared memory stuff */
extern Size AutoVacuumShmemSize(void);
extern void AutoVacuumShmemInit(void);

#endif							/* AUTOVACUUM_H */
