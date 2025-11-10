/*
 * test_guc_contexts.c
 *
 * Test module for GUC_EXTRA_IS_CONTEXT with both simple and complex structures.
 * Demonstrates the wrapper approach for context-based GUC extra data.
 */

#include "postgres.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

/*
 * 1: Simple context-based GUC with string lists
 */

typedef struct SimpleGucExtra
{
	List	   *string_list;
	int			item_count;
	char	   *description;
} SimpleGucExtra;

/* Global to track current simple GUC state - updated by assign_hook */
static SimpleGucExtra *simple_extra = NULL;

static char *simple_guc_string = NULL;

static bool check_simple_guc(char **newval, void **extra, GucSource source);
static void assign_simple_guc(const char *newval, void *extra);

typedef struct PlanShapeNode
{
	char	   *node_type;
	char	   *relation_name;
	double		cost_limit;
	List	   *children;
	List	   *hints;
} PlanShapeNode;

typedef struct PlanShapeExtra
{
	List	   *shapes;
	HTAB	   *shape_lookup;
	int			num_shapes;
	char	   *description;
} PlanShapeExtra;

typedef struct ShapeLookupEntry
{
	char		name[64];
	PlanShapeNode *node;
} ShapeLookupEntry;

/* Global to track current plan shape GUC state - updated by assign_hook */
static PlanShapeExtra *plan_shape_extra = NULL;

static char *plan_shape_string = NULL;

static bool check_plan_shape_guc(char **newval, void **extra, GucSource source);
static void assign_plan_shape_guc(const char *newval, void *extra);
static PlanShapeNode *parse_plan_shape_node(char **str_ptr, MemoryContext ctx);
static void dump_plan_shape_node(PlanShapeNode *node, int depth, StringInfo buf);

PG_FUNCTION_INFO_V1(show_simple_guc_items);
PG_FUNCTION_INFO_V1(count_simple_guc_items);
PG_FUNCTION_INFO_V1(show_plan_shapes);
PG_FUNCTION_INFO_V1(count_plan_shapes);
PG_FUNCTION_INFO_V1(lookup_plan_shape);

void
_PG_init(void)
{
	DefineCustomStringVariable("test_guc_contexts.simple",
							   "Simple context-based GUC with string lists",
							   "Comma-separated list of strings",
							   &simple_guc_string,
							   NULL,
							   PGC_USERSET,
							   GUC_EXTRA_IS_CONTEXT,
							   check_simple_guc,
							   assign_simple_guc,
							   NULL);

	DefineCustomStringVariable("test_guc_contexts.complex",
							   "Complex context-based GUC with plan shapes",
							   "Format: name1:NodeType(relation):cost,name2:...",
							   &plan_shape_string,
							   NULL,
							   PGC_USERSET,
							   GUC_EXTRA_IS_CONTEXT,
							   check_plan_shape_guc,
							   assign_plan_shape_guc,
							   NULL);
}

/*
 * ============================================================================
 * PART 1 Implementation: Simple GUC
 * ============================================================================
 */

static bool
check_simple_guc(char **newval, void **extra, GucSource source)
{
	MemoryContext extra_cxt;
	MemoryContext oldcontext;
	SimpleGucExtra *simple;
	GucContextExtra *wrapper;
	char	   *str_copy;
	char	   *token;
	int			num_items;

	if (*newval == NULL || **newval == '\0')
	{
		*extra = NULL;
		return true;
	}

	/* Count items */
	str_copy = pstrdup(*newval);
	num_items = 1;
	for (char *p = str_copy; *p; p++)
	{
		if (*p == ',')
			num_items++;
	}
	pfree(str_copy);

	extra_cxt = AllocSetContextCreate(TopMemoryContext,
									  "simple_guc_context",
									  ALLOCSET_SMALL_SIZES);

	oldcontext = MemoryContextSwitchTo(extra_cxt);

	simple = (SimpleGucExtra *) palloc(sizeof(SimpleGucExtra));
	simple->string_list = NIL;
	simple->item_count = num_items;
	simple->description = psprintf("Contains %d items", num_items);

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
			simple->string_list = lappend(simple->string_list,
										  pstrdup(token));

		token = strtok(NULL, ",");
	}
	pfree(str_copy);

	MemoryContextSwitchTo(oldcontext);

	/* Create wrapper using guc_malloc (not palloc!) */
	wrapper = (GucContextExtra *) guc_malloc(LOG, sizeof(GucContextExtra));
	wrapper->context = extra_cxt;
	wrapper->data = simple;

	*extra = wrapper;

	elog(DEBUG1, "simple GUC: parsed %d items, context=%p, data=%p",
		 num_items, extra_cxt, simple);

	return true;
}

static void
assign_simple_guc(const char *newval, void *extra)
{
	if (extra == NULL)
	{
		simple_extra = NULL;
		elog(DEBUG1, "Simple GUC: cleared");
		return;
	}

	GucContextExtra *wrapper = (GucContextExtra *) extra;
	simple_extra = (SimpleGucExtra *) wrapper->data;

	elog(DEBUG1, "simple GUC: active with %d items, context=%p, data=%p",
		 simple_extra->item_count, wrapper->context, simple_extra);
}

Datum
show_simple_guc_items(PG_FUNCTION_ARGS)
{
	StringInfoData buf;
	ListCell   *lc;

	if (simple_extra == NULL)
		PG_RETURN_TEXT_P(cstring_to_text("No items configured"));

	initStringInfo(&buf);
	appendStringInfo(&buf, "Items (%d total):\n", simple_extra->item_count);

	foreach(lc, simple_extra->string_list)
	{
		char *item = (char *) lfirst(lc);
		appendStringInfo(&buf, "  - %s\n", item);
	}

	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

Datum
count_simple_guc_items(PG_FUNCTION_ARGS)
{
	if (simple_extra == NULL)
		PG_RETURN_INT32(0);

	PG_RETURN_INT32(simple_extra->item_count);
}

static PlanShapeNode *
parse_plan_shape_node(char **str_ptr, MemoryContext ctx)
{
	PlanShapeNode *node;
	char	   *p = *str_ptr;
	char		node_type[64];
	char		relation[64];
	int			i;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(ctx);

	node = (PlanShapeNode *) palloc(sizeof(PlanShapeNode));
	node->children = NIL;
	node->hints = NIL;
	node->relation_name = NULL;
	node->cost_limit = -1;

	i = 0;
	while (*p && *p != '(' && *p != ':' && *p != ',' && i < 63)
		node_type[i++] = *p++;
	node_type[i] = '\0';
	node->node_type = pstrdup(node_type);

	if (*p == '(')
	{
		p++;
		i = 0;

		char	   *lookahead = p;
		bool		is_nested = false;

		while (*lookahead && *lookahead != ')' && *lookahead != ':')
		{
			if (isupper((unsigned char) *lookahead))
			{
				is_nested = true;
				break;
			}
			lookahead++;
		}

		if (is_nested)
		{
			while (*p && *p != ')')
			{
				PlanShapeNode *child = parse_plan_shape_node(&p, ctx);
				node->children = lappend(node->children, child);

				if (*p == '|')
					p++;
			}
		}
		else
		{
			while (*p && *p != ')' && i < 63)
				relation[i++] = *p++;
			relation[i] = '\0';
			if (i > 0)
				node->relation_name = pstrdup(relation);
		}

		if (*p == ')')
			p++;
	}

	if (*p == ':')
	{
		p++;
		node->cost_limit = strtod(p, &p);
	}

	MemoryContextSwitchTo(oldcontext);

	*str_ptr = p;
	return node;
}

static bool
check_plan_shape_guc(char **newval, void **extra, GucSource source)
{
	MemoryContext extra_cxt;
	MemoryContext oldcontext;
	PlanShapeExtra *plan_extra;
	GucContextExtra *wrapper;
	HASHCTL		hash_ctl;
	char	   *str_copy;
	char	   *token;
	char	   *parse_ptr;
	int			num_shapes = 0;

	if (*newval == NULL || **newval == '\0')
	{
		*extra = NULL;
		return true;
	}

	extra_cxt = AllocSetContextCreate(TopMemoryContext,
									  "plan_shape_guc_context",
									  ALLOCSET_DEFAULT_SIZES);

	oldcontext = MemoryContextSwitchTo(extra_cxt);

	plan_extra = (PlanShapeExtra *) palloc(sizeof(PlanShapeExtra));
	plan_extra->shapes = NIL;
	plan_extra->num_shapes = 0;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = 64;
	hash_ctl.entrysize = sizeof(ShapeLookupEntry);
	hash_ctl.hcxt = extra_cxt;

	plan_extra->shape_lookup = hash_create("plan_shape_lookup",
										   32,
										   &hash_ctl,
										   HASH_ELEM | HASH_CONTEXT | HASH_STRINGS);

	str_copy = pstrdup(*newval);
	token = strtok(str_copy, ",");

	while (token != NULL)
	{
		char		shape_name[64];
		char	   *colon;
		ShapeLookupEntry *entry;
		bool		found;

		while (*token == ' ')
			token++;

		colon = strchr(token, ':');
		if (colon == NULL)
		{
			elog(WARNING, "invalid plan shape format: %s", token);
			token = strtok(NULL, ",");
			continue;
		}

		strncpy(shape_name, token, Min(colon - token, 63));
		shape_name[Min(colon - token, 63)] = '\0';

		parse_ptr = colon + 1;
		PlanShapeNode *shape = parse_plan_shape_node(&parse_ptr, extra_cxt);

		plan_extra->shapes = lappend(plan_extra->shapes, shape);
		num_shapes++;

		entry = (ShapeLookupEntry *) hash_search(plan_extra->shape_lookup,
												  shape_name,
												  HASH_ENTER,
												  &found);
		entry->node = shape;

		token = strtok(NULL, ",");
	}

	plan_extra->num_shapes = num_shapes;
	plan_extra->description = psprintf("Plan shapes: %d configured", num_shapes);

	pfree(str_copy);

	MemoryContextSwitchTo(oldcontext);

	wrapper = (GucContextExtra *) guc_malloc(LOG, sizeof(GucContextExtra));
	wrapper->context = extra_cxt;
	wrapper->data = plan_extra;

	*extra = wrapper;

	elog(DEBUG1, "plan shapes: parsed %d shapes, context=%p, data=%p",
		 num_shapes, extra_cxt, plan_extra);

	return true;
}

static void
assign_plan_shape_guc(const char *newval, void *extra)
{
	if (extra == NULL)
	{
		plan_shape_extra = NULL;
		elog(DEBUG1, "plan shapes: cleared");
		return;
	}

	/* Extract data from wrapper */
	GucContextExtra *wrapper = (GucContextExtra *) extra;
	plan_shape_extra = (PlanShapeExtra *) wrapper->data;

	elog(DEBUG1, "plan shapes: active with %d shapes, context=%p, data=%p",
		 plan_shape_extra->num_shapes, wrapper->context, plan_shape_extra);
}

static void
dump_plan_shape_node(PlanShapeNode *node, int depth, StringInfo buf)
{
	ListCell   *lc;

	for (int i = 0; i < depth; i++)
		appendStringInfoString(buf, "  ");

	appendStringInfo(buf, "%s", node->node_type);

	if (node->relation_name)
		appendStringInfo(buf, "(%s)", node->relation_name);

	if (node->cost_limit >= 0)
		appendStringInfo(buf, ":%.0f", node->cost_limit);

	appendStringInfoChar(buf, '\n');

	foreach(lc, node->children)
	{
		PlanShapeNode *child = (PlanShapeNode *) lfirst(lc);
		dump_plan_shape_node(child, depth + 1, buf);
	}
}

Datum
show_plan_shapes(PG_FUNCTION_ARGS)
{
	StringInfoData buf;
	ListCell   *lc;
	int			shape_num = 0;

	if (plan_shape_extra == NULL)
		PG_RETURN_TEXT_P(cstring_to_text("No plan shapes configured"));

	initStringInfo(&buf);
	appendStringInfo(&buf, "Plan Shapes (%d total):\n\n", plan_shape_extra->num_shapes);

	foreach(lc, plan_shape_extra->shapes)
	{
		PlanShapeNode *shape = (PlanShapeNode *) lfirst(lc);
		appendStringInfo(&buf, "Shape %d:\n", ++shape_num);
		dump_plan_shape_node(shape, 1, &buf);
		appendStringInfoChar(&buf, '\n');
	}

	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

Datum
count_plan_shapes(PG_FUNCTION_ARGS)
{
	if (plan_shape_extra == NULL)
		PG_RETURN_INT32(0);

	PG_RETURN_INT32(plan_shape_extra->num_shapes);
}

Datum
lookup_plan_shape(PG_FUNCTION_ARGS)
{
	text	   *name_text = PG_GETARG_TEXT_PP(0);
	char	   *name = text_to_cstring(name_text);
	ShapeLookupEntry *entry;
	StringInfoData buf;

	if (plan_shape_extra == NULL)
		PG_RETURN_TEXT_P(cstring_to_text("No plan shapes configured"));

	entry = (ShapeLookupEntry *) hash_search(plan_shape_extra->shape_lookup,
											  name,
											  HASH_FIND,
											  NULL);

	if (entry == NULL)
	{
		initStringInfo(&buf);
		appendStringInfo(&buf, "Plan shape '%s' not found", name);
		PG_RETURN_TEXT_P(cstring_to_text(buf.data));
	}

	initStringInfo(&buf);
	appendStringInfo(&buf, "Plan shape '%s':\n", name);
	dump_plan_shape_node(entry->node, 0, &buf);

	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}