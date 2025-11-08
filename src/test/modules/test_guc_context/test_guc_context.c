/*-------------------------------------------------------------------------
 *
 * test_guc_context.c
 *		Test module for GUC_EXTRA_IS_CONTEXT functionality
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "nodes/pg_list.h"

PG_MODULE_MAGIC;

typedef struct TestGucExtra
{
	List	   *string_list;
	int			item_count;
	char	   *description;
} TestGucExtra;

static char *test_context_guc = NULL;
static MemoryContext current_extra_cxt = NULL;

void		_PG_init(void);

static char *
trim_whitespace(char *str)
{
	char *end;

	while (*str == ' ' || *str == '\t')
		str++;
	if (*str == '\0')
		return str;

	end = str + strlen(str) - 1;
	while (end > str && (*end == ' ' || *end == '\t'))
		end--;
	end[1] = '\0';

	return str;
}

static bool
check_test_context_guc(char **newval, void **extra, GucSource source)
{
	MemoryContext extra_cxt;
	MemoryContext oldcontext;
	TestGucExtra *test_extra;
	char	   *str_copy;
	char	   *token;
	int			num_items;

	if (*newval == NULL || **newval == '\0')
	{
		*extra = NULL;
		return true;
	}

	str_copy = pstrdup(*newval);
	num_items = 1;
	for (char *p = str_copy; *p; p++)
		if (*p == ',')
			num_items++;
	pfree(str_copy);

	extra_cxt = AllocSetContextCreate(TopMemoryContext,
									  "test_context_guc extra",
									  ALLOCSET_SMALL_SIZES);

	oldcontext = MemoryContextSwitchTo(extra_cxt);

	test_extra = (TestGucExtra *) palloc(sizeof(TestGucExtra));
	test_extra->string_list = NIL;
	test_extra->item_count = num_items;
	test_extra->description = psprintf("Contains %d items", num_items);

	str_copy = pstrdup(*newval);
	token = strtok(str_copy, ",");
	while (token != NULL)
	{
		while (*token == ' ')
			token++;
		char	   *end = token + strlen(token) - 1;

		while (end > token && *end == ' ')
			*end-- = '\0';

		test_extra->string_list = lappend(test_extra->string_list,
										  pstrdup(token));
		token = strtok(NULL, ",");
	}
	pfree(str_copy);

	MemoryContextSwitchTo(oldcontext);

	MemoryContextSetUserData(extra_cxt, test_extra);

	*extra = (void *) extra_cxt;

	elog(LOG, "check_hook: extra_cxt=%p, *extra=%p", extra_cxt, *extra);

	return true;
}

static void
assign_test_context_guc(const char *newval, void *extra)
{
	elog(LOG, "assign_hook: ENTRY, extra=%p, current_extra_cxt=%p",
		 extra, current_extra_cxt);

	if (extra == NULL)
	{
		elog(LOG, "assign_hook: setting current_extra_cxt to NULL");
		current_extra_cxt = NULL;
		elog(LOG, "assign_hook: EXIT");
		return;
	}

	current_extra_cxt = (MemoryContext) extra;
	elog(LOG, "assign_hook: set current_extra_cxt=%p", current_extra_cxt);

	elog(LOG, "assign_hook: EXIT");
}

PG_FUNCTION_INFO_V1(test_guc_context_get_count);
Datum
test_guc_context_get_count(PG_FUNCTION_ARGS)
{
	TestGucExtra *extra;

	if (current_extra_cxt == NULL)
		PG_RETURN_NULL();

	extra = (TestGucExtra *) MemoryContextGetUserData(current_extra_cxt);
	PG_RETURN_INT32(extra->item_count);
}

PG_FUNCTION_INFO_V1(test_guc_context_get_description);
Datum
test_guc_context_get_description(PG_FUNCTION_ARGS)
{
	TestGucExtra *extra;

	if (current_extra_cxt == NULL)
		PG_RETURN_NULL();

	extra = (TestGucExtra *) MemoryContextGetUserData(current_extra_cxt);
	PG_RETURN_TEXT_P(cstring_to_text(extra->description));
}

PG_FUNCTION_INFO_V1(test_guc_context_get_list);
Datum
test_guc_context_get_list(PG_FUNCTION_ARGS)
{
	TestGucExtra *extra;
	ListCell   *lc;
	StringInfoData buf;

	if (current_extra_cxt == NULL)
		PG_RETURN_NULL();

	extra = (TestGucExtra *) MemoryContextGetUserData(current_extra_cxt);

	initStringInfo(&buf);
	foreach(lc, extra->string_list)
	{
		if (lc != list_head(extra->string_list))
			appendStringInfoChar(&buf, ',');
		appendStringInfoString(&buf, (char *) lfirst(lc));
	}

	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

void
_PG_init(void)
{
	DefineCustomStringVariable("test_guc_context.value",
							   "Test GUC with context-based extra data structure",
							   NULL,
							   &test_context_guc,
							   "",
							   PGC_USERSET,
							   GUC_EXTRA_IS_CONTEXT,
							   check_test_context_guc,
							   assign_test_context_guc,
							   NULL);
}