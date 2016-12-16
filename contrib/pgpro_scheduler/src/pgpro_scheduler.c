#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/dsm.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shm_toc.h"

#include "pg_config.h"
#include "fmgr.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "executor/spi.h"
#include "tcop/utility.h"
#include "lib/stringinfo.h"
#include "access/xact.h"
#include "utils/snapmgr.h"
#include "utils/datetime.h"
#include "catalog/pg_db_role_setting.h"
#include "commands/dbcommands.h"

#include "char_array.h"
#include "sched_manager_poll.h"
#include "cron_string.h"
#include "pgpro_scheduler.h"
#include "scheduler_manager.h"
#include "memutils.h"
#include "scheduler_spi_utils.h"


#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

volatile sig_atomic_t got_sighup = false;
volatile sig_atomic_t got_sigterm = false;

/* Custom GUC variables */
static char *scheduler_databases = NULL;
static char *scheduler_nodename = NULL;
static char *scheduler_transaction_state = NULL;
static int  scheduler_max_workers = 2;
static bool scheduler_service_enabled = false;
static char *scheduler_schema = NULL;
/* Custom GUC done */

extern void
worker_spi_sighup(SIGNAL_ARGS)
{
	int save_errno = errno;

	got_sighup = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

extern void
worker_spi_sigterm(SIGNAL_ARGS)
{
	int save_errno = errno;

	got_sigterm = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/** Some utils **/

void reload_db_role_config(char *dbname)
{
	Relation    relsetting;
	Snapshot    snapshot;
	Oid databaseid;

	StartTransactionCommand();
	databaseid = get_database_oid(dbname, false);

	relsetting = heap_open(DbRoleSettingRelationId, AccessShareLock);
	snapshot = RegisterSnapshot(GetCatalogSnapshot(DbRoleSettingRelationId));
	ApplySetting(snapshot, databaseid, InvalidOid, relsetting, PGC_S_DATABASE);
	UnregisterSnapshot(snapshot);
	heap_close(relsetting, AccessShareLock);
	CommitTransactionCommand();
}

TimestampTz timestamp_add_seconds(TimestampTz to, int add)
{
	if(to == 0) to = GetCurrentTimestamp();
#ifdef HAVE_INT64_TIMESTAMP
	add *= USECS_PER_SEC;
#endif
	return add + to;
}

int get_integer_from_string(char *s, int start, int len)
{
	char buff[100];

	memcpy(buff, s + start, len);
	buff[len] = 0;
	return atoi(buff);
}

char *make_date_from_timestamp(TimestampTz ts)
{
	struct pg_tm dt;
	char *str = worker_alloc(sizeof(char) * 17);
	int tz;
	fsec_t fsec;
	const char *tzn;

	timestamp2tm(ts, &tz, &dt, &fsec, &tzn, NULL ); 
	sprintf(str, "%04d-%02d-%02d %02d:%02d", dt.tm_year , dt.tm_mon,
			dt.tm_mday, dt.tm_hour, dt.tm_min);
	return str;
}

TimestampTz get_timestamp_from_string(char *str)
{
    struct pg_tm dt;
    int tz;
    TimestampTz ts;

    memset(&dt, 0, sizeof(struct tm));
    dt.tm_year  = get_integer_from_string(str,  0, 4);
    dt.tm_mon   = get_integer_from_string(str,  5, 2);
    dt.tm_mday  = get_integer_from_string(str,  8, 2);
    dt.tm_hour  = get_integer_from_string(str, 11, 2);
    dt.tm_min   = get_integer_from_string(str, 14, 2);

    tz = DetermineTimeZoneOffset(&dt, session_timezone);

    tm2timestamp(&dt, 0, &tz, &ts);

    return ts;
}

TimestampTz _round_timestamp_to_minute(TimestampTz ts)
{
#ifdef HAVE_INT64_TIMESTAMP
	return ts - ts % USECS_PER_MINUTE;
#else
	return ts - ts % SECS_PER_MINUTE;
#endif
}

bool is_scheduler_enabled(void)
{
	const char *opt;

	opt = GetConfigOption("schedule.enabled", false, true);
	if(memcmp(opt, "on", 2) == 0) return true;
	return false;
}


/** END of SOME UTILS **/



char_array_t *readBasesToCheck(void)
{
	const char *value;
	int value_len = 0;
	int nnames = 0;
	char_array_t *names;
	char_array_t *result;
	char *clean_value;
	int i;
	int cv_len = 0;
	StringInfoData sql;
	int ret;
	int start_pos = 0;
	int processed;
	char *ptr = NULL;


	pgstat_report_activity(STATE_RUNNING, "read configuration");
	result = makeCharArray();

	value = GetConfigOption("schedule.database", 1, 0);
	if(!value || strlen(value) == 0)
	{
		return result;
	}
	value_len = strlen(value);
	clean_value = worker_alloc(sizeof(char)*(value_len+1));
	nnames = 1;
	for(i=0; i < value_len; i++)
	{
		if(value[i] != ' ')
		{
			if(value[i] == ',')
			{
				nnames++;
				clean_value[cv_len++] = 0;
			}
			else
			{
				clean_value[cv_len++] = value[i];
			}
		}
	}
	clean_value[cv_len] = 0;
	if(cv_len == 0 || nnames == cv_len)
	{
		pfree(clean_value);
		return result;
	}
	names = makeCharArray();
	for(i=0; i < cv_len + 1; i++)
	{
		if(clean_value[i] == 0)
		{
			ptr = clean_value + start_pos;
			if(strlen(ptr)) pushCharArray(names, ptr);
			start_pos = i + 1;
		}
	}
	pfree(clean_value);
	if(names->n == 0)
	{
		return result;
	}

	initStringInfo(&sql);
	appendStringInfo(&sql, "select datname from pg_database where datname in (");
	for(i=0; i < names->n; i++)
	{
		appendStringInfo(&sql, "'%s'", names->data[i]);
		if(i + 1  != names->n) appendStringInfo(&sql, ",");
	} 
	appendStringInfo(&sql, ")");
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	ret = SPI_execute(sql.data, true, 0);
	if (ret != SPI_OK_SELECT)
	{
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	destroyCharArray(names);
	processed  = SPI_processed;
	if(processed == 0)
	{
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		return result;
	}
	for(i=0; i < processed; i++)
	{
		clean_value = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);
		pushCharArray(result, clean_value);
	}
	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
	sortCharArray(result);
	return result;
}

void parent_scheduler_main(Datum arg)
{
	int rc = 0, i;
	char_array_t *names = NULL;
	schd_managers_poll_t *poll;
	schd_manager_share_t *shared;
	bool refresh = false;

	init_worker_mem_ctx("Parent scheduler context");

	/*CurrentResourceOwner = ResourceOwnerCreate(NULL, "pgpro_scheduler");*/
	SetConfigOption("application_name", "pgp-s supervisor", PGC_USERSET, PGC_S_SESSION);
	pgstat_report_activity(STATE_RUNNING, "Initialize");
	pqsignal(SIGHUP, worker_spi_sighup);
	pqsignal(SIGTERM, worker_spi_sigterm);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection("postgres", NULL);
	names = readBasesToCheck();
	poll = initSchedulerManagerPool(names);
	destroyCharArray(names);

	set_supervisor_pgstatus(poll);

	while(!got_sigterm)
	{

		if(rc & WL_POSTMASTER_DEATH) proc_exit(1);
		if(got_sighup)
		{
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
			refresh = false;
			names = NULL;
			if(is_scheduler_enabled() != poll->enabled)
			{
				if(poll->enabled)
				{
					poll->enabled = false;
					stopAllManagers(poll);
					set_supervisor_pgstatus(poll);
				}
				else
				{
					refresh = true;
					poll->enabled = true;
					names = readBasesToCheck();
				}
			}
			else if(poll->enabled)
			{
				names = readBasesToCheck();
				if(isBaseListChanged(names, poll)) refresh = true;
				else destroyCharArray(names);
			}

			if(refresh)
			{
				refreshManagers(names, poll);
				set_supervisor_pgstatus(poll);
				destroyCharArray(names);
			}
		}
		else 
		{
			for(i=0; i < poll->n; i++)
			{
				shared = dsm_segment_address(poll->workers[i]->shared);

				if(shared->setbychild)
				{
				/* elog(LOG, "got status change from: %s", poll->workers[i]->dbname); */
					shared->setbychild = false;
					if(shared->status == SchdManagerConnected)
					{
						poll->workers[i]->connected = true;
					}
					else if(shared->status == SchdManagerQuit)
					{
						removeManagerFromPoll(poll, poll->workers[i]->dbname, 1, true);
						set_supervisor_pgstatus(poll);
					}
					else if(shared->status == SchdManagerDie)
					{
						removeManagerFromPoll(poll, poll->workers[i]->dbname, 1, false);
						set_supervisor_pgstatus(poll);
					}
					else
					{
						elog(WARNING, "manager: %s set strange status: %d", poll->workers[i]->dbname, shared->status);
					}
				}
			}
		}
		rc = WaitLatch(MyLatch,
			WL_LATCH_SET | WL_POSTMASTER_DEATH, 0);
		CHECK_FOR_INTERRUPTS();
		ResetLatch(MyLatch);
	}
	stopAllManagers(poll);
	delete_worker_mem_ctx();

	proc_exit(0);
}

void
pg_scheduler_startup(void)
{
	BackgroundWorker worker;

	elog(LOG, "Start PostgresPro scheduler");

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main = parent_scheduler_main;
	worker.bgw_notify_pid = 0;
	worker.bgw_main_arg = 0;
	strcpy(worker.bgw_name, "pgpro scheduler");

	/* elog(LOG, "Register WORKER"); */


	RegisterBackgroundWorker(&worker); 
}

void _PG_init(void)
{
	if(!process_shared_preload_libraries_in_progress)
	{
		elog(ERROR, "pgpro_scheduler module must be initialized by Postmaster. "
					"Put the following line to configuration file: "
					"shared_preload_libraries='pgpro_scheduler'");
	}
	DefineCustomStringVariable(
		"schedule.schema",
		"The name of scheduler schema",
		NULL,
		&scheduler_schema,
		"schedule",
		PGC_POSTMASTER,
		0,
		NULL,
		NULL,
		NULL
	);
	DefineCustomStringVariable(
		"schedule.database",
		"On which databases scheduler could be run",
		NULL,
		&scheduler_databases,
		"",
		PGC_SIGHUP,
		0,
		NULL,
		NULL,
		NULL
	);
	DefineCustomStringVariable(
		"schedule.nodename",
		"The name of scheduler node",
		NULL,
		&scheduler_nodename,
		"master",
		PGC_SIGHUP,
		0,
		NULL,
		NULL,
		NULL
	);
	DefineCustomStringVariable(
		"schedule.transaction_state",
		"State of scheduler executor transaction",
		"If not under scheduler executor process the variable has no mean and has a value = 'undefined', possible values: progress, success, failure",
		&scheduler_transaction_state , 
		"undefined",
		PGC_INTERNAL,
		0,
		NULL,
		NULL,
		NULL
	);
	DefineCustomIntVariable(
		"schedule.max_workers",
		"How much workers can serve scheduler on one database",
		NULL,
		&scheduler_max_workers,
		2,
		1,
		100,
		PGC_SUSET,
		0,
		NULL,
		NULL,
		NULL
	);
	DefineCustomBoolVariable(
		"schedule.enabled",
		"Enable schedule service",
		NULL,
		&scheduler_service_enabled,
		false,
		PGC_SIGHUP,
		0,
		NULL,
		NULL,
		NULL
	);
	pg_scheduler_startup();
}

PG_FUNCTION_INFO_V1(temp_now);
Datum
temp_now(PG_FUNCTION_ARGS)
{
	TimestampTz ts;
	struct pg_tm info;
	struct pg_tm cp;
	int tz;
	fsec_t fsec;
	const char *tzn;
	long int toff = 0;

	if(!PG_ARGISNULL(0))
	{
		ts = PG_GETARG_TIMESTAMPTZ(0);
	}
	else
	{
		ts = GetCurrentTimestamp();
	}

	timestamp2tm(ts, &tz, &info, &fsec, &tzn, session_timezone );
	info.tm_wday = j2day(date2j(info.tm_year, info.tm_mon, info.tm_mday));

/*	elog(NOTICE, "WDAY: %d, MON: %d, MDAY: %d, HOUR: %d, MIN: %d, YEAR: %d (%ld)", 
		info.tm_wday, info.tm_mon, info.tm_mday, info.tm_hour, info.tm_min,
		info.tm_year, info.tm_gmtoff);
	elog(NOTICE, "TZP: %d, ZONE: %s", tz, tzn); */

	cp.tm_mon = info.tm_mon;
	cp.tm_mday = info.tm_mday;
	cp.tm_hour = info.tm_hour;
	cp.tm_min = info.tm_min;
	cp.tm_year = info.tm_year;
	cp.tm_sec = info.tm_sec;

	toff = DetermineTimeZoneOffset(&cp, session_timezone);
/*	elog(NOTICE, "Detect: offset = %ld", toff); */

	cp.tm_gmtoff = -toff;
	tm2timestamp(&cp, 0, &tz, &ts);


	PG_RETURN_TIMESTAMPTZ(ts);
}

PG_FUNCTION_INFO_V1(cron_string_to_json_text);
Datum
cron_string_to_json_text(PG_FUNCTION_ARGS)
{
	char *source = NULL;
	char *jsonText = NULL;
	text *text_p;
	int len;
	char *error = NULL;
	
	if(PG_ARGISNULL(0))
	{  
		PG_RETURN_NULL();
	}
	source = PG_GETARG_CSTRING(0);
	jsonText = parse_crontab_to_json_text(source);
	
	if(jsonText)
	{
		len = strlen(jsonText);
		text_p = (text *) palloc(sizeof(char)*len + VARHDRSZ);
		memcpy((void *)VARDATA(text_p), jsonText, len);
		SET_VARSIZE(text_p, sizeof(char)*len + VARHDRSZ);
		free(jsonText);
		PG_RETURN_TEXT_P(text_p);
	}
	else
	{
		error = get_cps_error();
		if(error)
		{
			elog(ERROR, "%s (%d)", error, cps_error);
		}
		else
		{
			elog(ERROR, "unknown error: %d", cps_error);
		}
	}
}

