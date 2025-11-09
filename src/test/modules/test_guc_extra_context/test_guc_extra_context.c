/*--------------------------------------------------------------------------
 *
 * test_guc_extra_context.c
 *		Test module for GUC_EXTRA_IS_CONTEXT functionality
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_guc_extra_context/test_guc_extra_context.c
 *
 *--------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

typedef struct TestGucExtra
{
	List	   *string_list;
	int			item_count;
	char	   *description;
} TestGucExtra;

static char *test_context_guc_string = NULL;
static MemoryContext current_context = NULL;

static bool check_test_context_guc(char **newval, void **extra, GucSource source);
static void assign_test_context_guc(const char *newval, void *extra);

PG_FUNCTION_INFO_V1(guc_test_get_context_address);
PG_FUNCTION_INFO_V1(guc_test_context_exists);

void
_PG_init(void)
{
	DefineCustomStringVariable("test_guc_extra_context.value",
							   "Test GUC with context-based extra data",
							   "Accepts comma-separated values, stores as List in MemoryContext",
							   &test_context_guc_string,
							   "",
							   PGC_USERSET,
							   GUC_EXTRA_IS_CONTEXT,
							   check_test_context_guc,
							   assign_test_context_guc,
							   NULL);
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
	{
		if (*p == ',')
			num_items++;
	}
	pfree(str_copy);

	extra_cxt = AllocSetContextCreate(TopMemoryContext,
									  "test_guc_extra_context",
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
		while (*token == ' ' || *token == '\t')
			token++;
		char *end = token + strlen(token) - 1;
		while (end > token && (*end == ' ' || *end == '\t'))
			*end-- = '\0';

		if (*token != '\0')
			test_extra->string_list = lappend(test_extra->string_list,
											  pstrdup(token));

		token = strtok(NULL, ",");
	}
	pfree(str_copy);

	MemoryContextSwitchTo(oldcontext);

	*extra = extra_cxt;

	return true;
}

static void
assign_test_context_guc(const char *newval, void *extra)
{
	current_context = (MemoryContext) extra;
	test_context_guc_string = (char *) newval;
}

Datum
guc_test_get_context_address(PG_FUNCTION_ARGS)
{
	char		buf[32];

	if (current_context != NULL)
	{
		snprintf(buf, sizeof(buf), "%p", current_context);
		PG_RETURN_TEXT_P(cstring_to_text(buf));
	}
	else
	{
		PG_RETURN_TEXT_P(cstring_to_text(""));
	}
}

Datum
guc_test_context_exists(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(current_context != NULL);
}