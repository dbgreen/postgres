/*
 * test_guc.c
 */

#include "postgres.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

typedef struct SimpleCounterExtra
{
	int			count;
	char		description[64];
}			SimpleCounterExtra;

static SimpleCounterExtra * counter_extra = NULL;
static char *counter_string = NULL;

static bool check_counter_guc(char **newval, void **extra, GucSource source);
static void assign_counter_guc(const char *newval, void *extra);

typedef struct ServerPool
{
	char	   *pool_name;
	List	   *servers;
	int			max_connections;
	int			timeout_seconds;
	char	   *description;
}			ServerPool;

static ServerPool * pool_extra = NULL;
static char *pool_string = NULL;

static bool check_pool_guc(char **newval, void **extra, GucSource source);
static void assign_pool_guc(const char *newval, void *extra);

PG_FUNCTION_INFO_V1(get_counter_value);
PG_FUNCTION_INFO_V1(get_counter_description);
PG_FUNCTION_INFO_V1(show_server_pool);
PG_FUNCTION_INFO_V1(count_servers);
PG_FUNCTION_INFO_V1(get_pool_setting);

void
_PG_init(void)
{
	DefineCustomStringVariable("test_guc.counter",
							   "Simple GUC without context (backward compatibility)",
							   "Integer counter value",
							   &counter_string,
							   NULL,
							   PGC_USERSET,
							   0,	/* No GUC_EXTRA_IS_CONTEXT flag */
							   check_counter_guc,
							   assign_counter_guc,
							   NULL);

	DefineCustomStringVariable("test_guc.pool",
							   "Server pool configuration with context",
							   "Format: name:server1,server2;max_connections=N;timeout=N",
							   &pool_string,
							   NULL,
							   PGC_USERSET,
							   GUC_EXTRA_IS_CONTEXT,	/* GUC machinery manages
														 * context */
							   check_pool_guc,
							   assign_pool_guc,
							   NULL);
}

static bool
check_counter_guc(char **newval, void **extra, GucSource source)
{
	SimpleCounterExtra *data;
	int			count;

	if (*newval == NULL || **newval == '\0')
	{
		*extra = NULL;
		return true;
	}

	count = atoi(*newval);

	data = (SimpleCounterExtra *) guc_malloc(LOG, sizeof(SimpleCounterExtra));
	if (data == NULL)
		return false;

	data->count = count;
	snprintf(data->description, sizeof(data->description), "Count is %d", count);

	*extra = data;

	elog(DEBUG1, "counter GUC: parsed count=%d, data=%p", count, data);

	return true;
}

static void
assign_counter_guc(const char *newval, void *extra)
{
	if (extra == NULL)
	{
		counter_extra = NULL;
		elog(DEBUG1, "Counter GUC: cleared");
		return;
	}

	counter_extra = (SimpleCounterExtra *) extra;

	elog(DEBUG1, "counter GUC: active with count=%d, data=%p",
		 counter_extra->count, counter_extra);
}

Datum
get_counter_value(PG_FUNCTION_ARGS)
{
	if (counter_extra == NULL)
		PG_RETURN_INT32(0);

	PG_RETURN_INT32(counter_extra->count);
}

Datum
get_counter_description(PG_FUNCTION_ARGS)
{
	if (counter_extra == NULL)
		PG_RETURN_TEXT_P(cstring_to_text("No counter configured"));

	PG_RETURN_TEXT_P(cstring_to_text(counter_extra->description));
}

/*
 * Parse server pool configuration
 * Format: "pool_name:server1,server2,server3;max_connections=10;timeout=30"
 */
static bool
check_pool_guc(char **newval, void **extra, GucSource source)
{
	ServerPool *pool;
	char	   *work_str;
	char	   *pool_name;
	char	   *servers_part;
	char	   *settings_part;
	char	   *server_token;
	int			max_conn = 10;
	int			timeout = 30;
	int			server_count = 0;

	if (*newval == NULL || **newval == '\0')
	{
		*extra = NULL;
		return true;
	}

	work_str = pstrdup(*newval);

	pool_name = work_str;
	servers_part = strchr(work_str, ':');
	if (servers_part == NULL)
	{
		pfree(work_str);
		GUC_check_errdetail("Format should be 'name:server1,server2;setting=val'");
		return false;
	}
	*servers_part++ = '\0';

	settings_part = strchr(servers_part, ';');
	if (settings_part != NULL)
		*settings_part++ = '\0';

	if (settings_part != NULL)
	{
		char	   *setting = strtok(settings_part, ";");

		while (setting != NULL)
		{
			char	   *eq = strchr(setting, '=');

			if (eq != NULL)
			{
				*eq++ = '\0';
				if (strcmp(setting, "max_connections") == 0)
					max_conn = atoi(eq);
				else if (strcmp(setting, "timeout") == 0)
					timeout = atoi(eq);
			}
			setting = strtok(NULL, ";");
		}
	}

	pool = (ServerPool *) palloc(sizeof(ServerPool));
	pool->pool_name = pstrdup(pool_name);
	pool->servers = NIL;
	pool->max_connections = max_conn;
	pool->timeout_seconds = timeout;

	/* Parse server list */
	server_token = strtok(servers_part, ",");
	while (server_token != NULL)
	{
		while (*server_token == ' ' || *server_token == '\t')
			server_token++;
		char	   *end = server_token + strlen(server_token) - 1;

		while (end > server_token && (*end == ' ' || *end == '\t'))
			*end-- = '\0';

		if (*server_token != '\0')
		{
			pool->servers = lappend(pool->servers, pstrdup(server_token));
			server_count++;
		}

		server_token = strtok(NULL, ",");
	}

	pool->description = psprintf("Pool '%s': %d servers, max_conn=%d, timeout=%d",
								 pool->pool_name,
								 server_count,
								 pool->max_connections,
								 pool->timeout_seconds);

	*extra = pool;

	pfree(work_str);

	elog(DEBUG1, "pool GUC: parsed pool '%s' with %d servers, data=%p",
		 pool->pool_name, server_count, pool);

	return true;
}

static void
assign_pool_guc(const char *newval, void *extra)
{
	if (extra == NULL)
	{
		pool_extra = NULL;
		elog(DEBUG1, "Server pool GUC: cleared");
		return;
	}

	pool_extra = (ServerPool *) extra;

	elog(DEBUG1, "pool GUC: active pool '%s' with %d servers, data=%p",
		 pool_extra->pool_name,
		 list_length(pool_extra->servers),
		 pool_extra);
}

Datum
show_server_pool(PG_FUNCTION_ARGS)
{
	StringInfoData buf;
	ListCell   *lc;

	if (pool_extra == NULL)
		PG_RETURN_TEXT_P(cstring_to_text("No server pool configured"));

	initStringInfo(&buf);
	appendStringInfo(&buf, "Pool: %s\n", pool_extra->pool_name);
	appendStringInfo(&buf, "Max connections: %d\n", pool_extra->max_connections);
	appendStringInfo(&buf, "Timeout: %d seconds\n", pool_extra->timeout_seconds);
	appendStringInfo(&buf, "Servers (%d total):\n", list_length(pool_extra->servers));

	foreach(lc, pool_extra->servers)
	{
		char	   *server = (char *) lfirst(lc);

		appendStringInfo(&buf, "  - %s\n", server);
	}

	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

Datum
count_servers(PG_FUNCTION_ARGS)
{
	if (pool_extra == NULL)
		PG_RETURN_INT32(0);

	PG_RETURN_INT32(list_length(pool_extra->servers));
}

Datum
get_pool_setting(PG_FUNCTION_ARGS)
{
	text	   *setting_name = PG_GETARG_TEXT_PP(0);
	char	   *name = text_to_cstring(setting_name);
	char		result[256];

	if (pool_extra == NULL)
		PG_RETURN_TEXT_P(cstring_to_text("No pool configured"));

	if (strcmp(name, "pool_name") == 0)
		snprintf(result, sizeof(result), "%s", pool_extra->pool_name);
	else if (strcmp(name, "max_connections") == 0)
		snprintf(result, sizeof(result), "%d", pool_extra->max_connections);
	else if (strcmp(name, "timeout") == 0)
		snprintf(result, sizeof(result), "%d", pool_extra->timeout_seconds);
	else if (strcmp(name, "description") == 0)
		snprintf(result, sizeof(result), "%s", pool_extra->description);
	else
		snprintf(result, sizeof(result), "Unknown setting: %s", name);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}
