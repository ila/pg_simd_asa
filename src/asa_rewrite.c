/*
 * asa_rewrite.c — automatic SIMD-ASA query rewrite.
 *
 * PostgreSQL counterpart of the DuckDB extension's compiler (FK join injection,
 * aggregate transformation). With simd_asa.rewrite = on, an aggregate query over
 * relations owned by the sampling entity is rewritten during parse analysis:
 *
 *   1. Identify the entity-dependent relations in the FROM clause: the entity itself
 *      (asa_entity) and every relation with a unique asa_link path to it.
 *   2. Derive each relation's entity key. A link is followed through a join the query
 *      already contains (rel.link_column = ref.ref_column); otherwise the linked
 *      relation is joined in. A link that points at the entity needs no join: the
 *      link column holds the entity key.
 *   3. mask = AND over the distinct keys of priv_hash(asa_hash(key)): a joined tuple
 *      belongs to world j iff every entity it derives from does.
 *   4. Replace sum/count/avg/min/max(x) by as_sum/as_count/as_avg/as_min/as_max(mask, x),
 *      which return one result per world (real[64]); + - * / over aggregates are evaluated
 *      per world (asa_lane_*).
 *   5. A WHERE comparison with a scalar sub-select that aggregates over entity-dependent
 *      relations (TPC-H Q17) is a predicate over worlds: the sub-select is rewritten the
 *      same way, and the comparison becomes the mask of the worlds where it holds, AND-ed
 *      into the outer mask instead of filtering rows.
 *
 * Rewriting at parse analysis (post_parse_analyze_hook) rather than in the planner keeps
 * the changed result types consistent for prepared statements and the extended protocol.
 * Query shapes whose world semantics the rewrite does not implement (aggregates in HAVING,
 * other expressions over aggregates, other subqueries over dependent relations, dependent
 * relations on the nullable side of outer joins, ...) are rejected rather than silently
 * left unrewritten.
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/rel.h"

#include "asa_common.h"

#if PG_VERSION_NUM >= 160000

#define ASA_MAX_LINK_DEPTH 32
#define ASA_MAX_INJECTED_JOINS 64

static bool asa_rewrite_enabled = false;
static char *asa_entity_setting = NULL;
static bool asa_in_rewrite = false;
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;

#define ASA_UNSUPPORTED(...) \
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("SIMD-ASA rewrite: " __VA_ARGS__), \
					errhint("Rewrite the query by hand with the as_* aggregates, or SET simd_asa.rewrite = off.")))

/* ---------------------------------------------------------------------------
 * Metadata
 * ------------------------------------------------------------------------- */

typedef struct AsaLink
{
	Oid			relid;
	AttrNumber	attnum;
	Oid			ref_relid;
	AttrNumber	ref_attnum;
} AsaLink;

typedef struct AsaMeta
{
	Oid			schema;			/* extension schema: function lookups */
	Oid			entity_relid;	/* selected sampling entity */
	AttrNumber	entity_attnum;
	List	   *links;			/* AsaLink *: all declared links */
} AsaMeta;

static AttrNumber
asa_attnum(Oid relid, const char *column)
{
	AttrNumber	attnum = get_attnum(relid, column);

	if (attnum == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
						errmsg("SIMD-ASA: column \"%s\" of relation \"%s\" no longer exists", column,
							   get_rel_name(relid))));
	return attnum;
}

/*
 * Reads asa_entity / asa_link and selects the sampling entity: simd_asa.entity if set,
 * else the only declared entity. Returns NULL when there is nothing to rewrite.
 */
static AsaMeta *
asa_load_meta(void)
{
	Oid			extoid = get_extension_oid("simd_asa", true);
	MemoryContext caller = CurrentMemoryContext;
	AsaMeta    *meta;
	const char *schema;
	List	   *entity_relids = NIL;
	List	   *entity_columns = NIL;
	Oid			wanted = InvalidOid;

	if (!OidIsValid(extoid) || creating_extension)
		return NULL;
	meta = (AsaMeta *) palloc0(sizeof(AsaMeta));
	meta->schema = get_extension_schema(extoid);
	schema = quote_identifier(get_namespace_name(meta->schema));

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SIMD-ASA: SPI_connect failed");
	if (SPI_execute(psprintf("SELECT relid::oid, key_column::text FROM %s.asa_entity", schema), true, 0) !=
		SPI_OK_SELECT)
		elog(ERROR, "SIMD-ASA: cannot read asa_entity");
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		bool		isnull;
		MemoryContext old = MemoryContextSwitchTo(caller);

		entity_relids = lappend_oid(entity_relids,
									DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1,
																   &isnull)));
		entity_columns = lappend(entity_columns, pstrdup(SPI_getvalue(SPI_tuptable->vals[i],
																	 SPI_tuptable->tupdesc, 2)));
		MemoryContextSwitchTo(old);
	}
	if (SPI_execute(psprintf("SELECT relid::oid, link_column::text, ref_relid::oid, ref_column::text "
							 "FROM %s.asa_link", schema), true, 0) != SPI_OK_SELECT)
		elog(ERROR, "SIMD-ASA: cannot read asa_link");
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		HeapTuple	tup = SPI_tuptable->vals[i];
		TupleDesc	desc = SPI_tuptable->tupdesc;
		bool		isnull;
		MemoryContext old = MemoryContextSwitchTo(caller);
		AsaLink    *link = (AsaLink *) palloc(sizeof(AsaLink));

		link->relid = DatumGetObjectId(SPI_getbinval(tup, desc, 1, &isnull));
		link->attnum = asa_attnum(link->relid, SPI_getvalue(tup, desc, 2));
		link->ref_relid = DatumGetObjectId(SPI_getbinval(tup, desc, 3, &isnull));
		link->ref_attnum = asa_attnum(link->ref_relid, SPI_getvalue(tup, desc, 4));
		meta->links = lappend(meta->links, link);
		MemoryContextSwitchTo(old);
	}
	SPI_finish();

	if (asa_entity_setting && asa_entity_setting[0] != '\0')
	{
		RangeVar   *rv = makeRangeVarFromNameList(stringToQualifiedNameList(asa_entity_setting, NULL));

		wanted = RangeVarGetRelid(rv, NoLock, false);
		if (!list_member_oid(entity_relids, wanted))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("SIMD-ASA: simd_asa.entity \"%s\" is not a declared sampling entity",
								   asa_entity_setting),
							errhint("Declare it with asa_add_entity().")));
	}
	else if (list_length(entity_relids) == 0)
		return NULL;
	else if (list_length(entity_relids) > 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("SIMD-ASA: several sampling entities are declared"),
						errhint("Choose one with SET simd_asa.entity = '<table>'.")));
	else
		wanted = linitial_oid(entity_relids);

	for (int i = 0; i < list_length(entity_relids); i++)
		if (list_nth_oid(entity_relids, i) == wanted)
		{
			meta->entity_relid = wanted;
			meta->entity_attnum = asa_attnum(wanted, list_nth(entity_columns, i));
		}
	return meta;
}

/* The unique link of relid on a path to the entity, NULL if relid does not reach it. */
static AsaLink *
asa_link_towards_entity(AsaMeta *meta, Oid relid, int depth)
{
	AsaLink    *found = NULL;
	ListCell   *lc;

	if (depth > ASA_MAX_LINK_DEPTH)
		return NULL;			/* cyclic links: no path */
	foreach(lc, meta->links)
	{
		AsaLink    *link = (AsaLink *) lfirst(lc);

		if (link->relid != relid)
			continue;
		if (link->ref_relid != meta->entity_relid &&
			asa_link_towards_entity(meta, link->ref_relid, depth + 1) == NULL)
			continue;
		if (found)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("SIMD-ASA: relation \"%s\" has more than one link path to sampling entity \"%s\"",
								   get_rel_name(relid), get_rel_name(meta->entity_relid)),
							errdetail("Every entity-dependent relation must map to the entity through a unique "
									  "ownership path.")));
		found = link;
	}
	return found;
}

static bool
asa_is_dependent(AsaMeta *meta, Oid relid)
{
	return relid == meta->entity_relid || asa_link_towards_entity(meta, relid, 0) != NULL;
}

/* ---------------------------------------------------------------------------
 * Finding entity-dependent relations below the top query level
 * ------------------------------------------------------------------------- */

typedef struct AsaNestedCtx
{
	AsaMeta    *meta;
	int			depth;			/* 0: the query being rewritten */
	int			agg_depth;		/* number of enclosing sub-queries with aggregates */
	bool		nested;			/* a sub-query or view reads a dependent relation */
	bool		nested_agg;		/* ... inside an aggregating sub-query */
} AsaNestedCtx;

static bool asa_nested_walker(Node *node, AsaNestedCtx *ctx);

static void
asa_nested_query(Query *query, AsaNestedCtx *ctx)
{
	ctx->depth++;
	if (query->hasAggs)
		ctx->agg_depth++;
	query_tree_walker(query, asa_nested_walker, ctx, QTW_EXAMINE_RTES_BEFORE);
	if (query->hasAggs)
		ctx->agg_depth--;
	ctx->depth--;
}

static bool
asa_nested_walker(Node *node, AsaNestedCtx *ctx)
{
	if (node == NULL)
		return false;
	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		if (rte->rtekind != RTE_RELATION)
			return false;		/* subquery RTEs are descended into by range_table_walker */
		if (rte->relkind == RELKIND_VIEW)
		{
			Relation	view = table_open(rte->relid, AccessShareLock);
			Query	   *view_query = copyObject(get_view_query(view));

			table_close(view, NoLock);
			asa_nested_query(view_query, ctx);
		}
		else if (ctx->depth > 0 && asa_is_dependent(ctx->meta, rte->relid))
		{
			ctx->nested = true;
			if (ctx->agg_depth > 0)
				ctx->nested_agg = true;
		}
		return false;
	}
	if (IsA(node, Query))
	{
		asa_nested_query((Query *) node, ctx);
		return false;
	}
	return expression_tree_walker(node, asa_nested_walker, ctx);
}

/* ---------------------------------------------------------------------------
 * Join tree analysis
 * ------------------------------------------------------------------------- */

typedef struct AsaEq
{
	Var		   *a;
	Var		   *b;
} AsaEq;

typedef struct AsaCtx
{
	ParseState *pstate;
	Query	   *query;
	AsaMeta    *meta;
	Bitmapset  *nullable;		/* rtindexes on the nullable side of an outer join */
	List	   *eqs;			/* AsaEq *: top-level equality conjuncts between two Vars */
	Expr	  **keys;			/* entity key per rtindex (memo) */
	int			max_rtindex;
	int			injected;
} AsaCtx;

static void
asa_mark_nullable(Node *jtnode, bool nullable, Bitmapset **out)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		if (nullable)
			*out = bms_add_member(*out, ((RangeTblRef *) jtnode)->rtindex);
	}
	else if (IsA(jtnode, FromExpr))
	{
		ListCell   *lc;

		foreach(lc, ((FromExpr *) jtnode)->fromlist)
			asa_mark_nullable(lfirst(lc), nullable, out);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		asa_mark_nullable(j->larg, nullable || j->jointype == JOIN_RIGHT || j->jointype == JOIN_FULL, out);
		asa_mark_nullable(j->rarg, nullable || j->jointype == JOIN_LEFT || j->jointype == JOIN_FULL, out);
	}
}

static Var *
asa_strip_var(Node *node)
{
	while (node && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	if (node && IsA(node, Var) && ((Var *) node)->varlevelsup == 0)
		return (Var *) node;
	return NULL;
}

static void
asa_add_eqs(Node *quals, List **eqs)
{
	ListCell   *lc;

	foreach(lc, make_ands_implicit((Expr *) quals))
	{
		OpExpr	   *op = (OpExpr *) lfirst(lc);
		Var		   *a,
				   *b;
		char	   *opname;

		if (!IsA(op, OpExpr) || list_length(op->args) != 2)
			continue;
		opname = get_opname(op->opno);
		a = asa_strip_var(linitial(op->args));
		b = asa_strip_var(lsecond(op->args));
		if (opname && strcmp(opname, "=") == 0 && a && b)
		{
			AsaEq	   *eq = (AsaEq *) palloc(sizeof(AsaEq));

			eq->a = a;
			eq->b = b;
			*eqs = lappend(*eqs, eq);
		}
	}
}

/* Equality conjuncts that hold for every result row: WHERE and non-nullable inner joins. */
static void
asa_collect_eqs(Node *jtnode, bool nullable, List **eqs)
{
	if (jtnode == NULL || IsA(jtnode, RangeTblRef))
		return;
	if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *lc;

		if (!nullable)
			asa_add_eqs(f->quals, eqs);
		foreach(lc, f->fromlist)
			asa_collect_eqs(lfirst(lc), nullable, eqs);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		if (!nullable && j->jointype == JOIN_INNER)
			asa_add_eqs(j->quals, eqs);
		asa_collect_eqs(j->larg, nullable || j->jointype == JOIN_RIGHT || j->jointype == JOIN_FULL, eqs);
		asa_collect_eqs(j->rarg, nullable || j->jointype == JOIN_LEFT || j->jointype == JOIN_FULL, eqs);
	}
}

static Var *
asa_make_var(AsaCtx *ctx, int rtindex, AttrNumber attnum)
{
	RangeTblEntry *rte = rt_fetch(rtindex, ctx->query->rtable);
	Oid			type;
	int32		typmod;
	Oid			collid;
	Var		   *var;

	get_atttypetypmodcoll(rte->relid, attnum, &type, &typmod, &collid);
	var = makeVar(rtindex, attnum, type, typmod, collid, 0);
	markVarForSelectPriv(ctx->pstate, var);
	return var;
}

static bool
asa_var_is(Var *var, int rtindex, AttrNumber attnum)
{
	return var->varno == rtindex && var->varattno == attnum;
}

/* An rtindex of ref_relid joined to (rtindex, attnum) on ref_attnum, or 0. */
static int
asa_find_joined(AsaCtx *ctx, int rtindex, AttrNumber attnum, Oid ref_relid, AttrNumber ref_attnum)
{
	ListCell   *lc;

	foreach(lc, ctx->eqs)
	{
		AsaEq	   *eq = (AsaEq *) lfirst(lc);
		Var		   *other = asa_var_is(eq->a, rtindex, attnum) ? eq->b :
			asa_var_is(eq->b, rtindex, attnum) ? eq->a : NULL;
		RangeTblEntry *rte;

		if (!other || other->varattno != ref_attnum || other->varno > list_length(ctx->query->rtable))
			continue;
		rte = rt_fetch(other->varno, ctx->query->rtable);
		if (rte->rtekind == RTE_RELATION && rte->relid == ref_relid && !bms_is_member(other->varno, ctx->nullable))
			return other->varno;
	}
	return 0;
}

/* Joins ref_relid into the query on (rtindex, attnum) = ref.ref_attnum; returns its rtindex. */
static int
asa_inject_join(AsaCtx *ctx, int rtindex, AttrNumber attnum, Oid ref_relid, AttrNumber ref_attnum)
{
	Relation	rel;
	ParseNamespaceItem *nsitem;
	RangeTblRef *rtr;
	Var		   *a;
	Var		   *b;
	Node	   *eq;
	int			ref_index;

	if (++ctx->injected > ASA_MAX_INJECTED_JOINS)
		ASA_UNSUPPORTED("cannot join \"%s\" into this query; add the join to \"%s\" explicitly",
						get_rel_name(ref_relid), get_rel_name(ref_relid));
	rel = table_open(ref_relid, AccessShareLock);
	nsitem = addRangeTableEntryForRelation(ctx->pstate, rel, AccessShareLock,
										   makeAlias(psprintf("asa_%s", RelationGetRelationName(rel)), NIL),
										   true, false);
	table_close(rel, NoLock);
	ctx->query->rtable = ctx->pstate->p_rtable;
	ctx->query->rteperminfos = ctx->pstate->p_rteperminfos;
	ref_index = nsitem->p_rtindex;

	rtr = makeNode(RangeTblRef);
	rtr->rtindex = ref_index;
	ctx->query->jointree->fromlist = lappend(ctx->query->jointree->fromlist, rtr);
	a = asa_make_var(ctx, rtindex, attnum);
	b = asa_make_var(ctx, ref_index, ref_attnum);
	eq = (Node *) make_op(ctx->pstate, list_make2(makeString("pg_catalog"), makeString("=")), (Node *) a,
						  (Node *) b, NULL, -1);
	ctx->query->jointree->quals = make_and_qual(ctx->query->jointree->quals, eq);
	asa_add_eqs(eq, &ctx->eqs);
	return ref_index;
}

/* The entity key that the rows of rtindex derive from. */
static Expr *
asa_entity_key(AsaCtx *ctx, int rtindex, int depth)
{
	RangeTblEntry *rte = rt_fetch(rtindex, ctx->query->rtable);
	AsaLink    *link;
	int			ref_index;
	Expr	   *key;

	if (rtindex <= ctx->max_rtindex && ctx->keys[rtindex])
		return ctx->keys[rtindex];
	if (depth > ASA_MAX_LINK_DEPTH)
		ASA_UNSUPPORTED("link path to the sampling entity is too long");

	if (rte->relid == ctx->meta->entity_relid)
		key = (Expr *) asa_make_var(ctx, rtindex, ctx->meta->entity_attnum);
	else
	{
		link = asa_link_towards_entity(ctx->meta, rte->relid, 0);
		ref_index = asa_find_joined(ctx, rtindex, link->attnum, link->ref_relid, link->ref_attnum);
		if (ref_index > 0)
			key = asa_entity_key(ctx, ref_index, depth + 1);
		else if (link->ref_relid == ctx->meta->entity_relid)
			key = (Expr *) asa_make_var(ctx, rtindex, link->attnum); /* link column = entity key */
		else
			key = asa_entity_key(ctx, asa_inject_join(ctx, rtindex, link->attnum, link->ref_relid,
													  link->ref_attnum), depth + 1);
	}
	if (rtindex <= ctx->max_rtindex)
		ctx->keys[rtindex] = key;
	return key;
}

/* ---------------------------------------------------------------------------
 * Mask and aggregates
 * ------------------------------------------------------------------------- */

static Oid
asa_func(AsaCtx *ctx, const char *name, int nargs, Oid *argtypes)
{
	List	   *qualified = list_make2(makeString(get_namespace_name(ctx->meta->schema)),
									   makeString(pstrdup(name)));

	return LookupFuncName(qualified, nargs, argtypes, false);
}

/* priv_hash(asa_hash(key)); smallint keys hash like integers (as DuckDB's hash()). */
static Expr *
asa_key_mask(AsaCtx *ctx, Expr *key)
{
	Oid			type = exprType((Node *) key);
	Oid			int8_arg[1] = {INT8OID};
	Expr	   *hash;

	if (type == INT2OID)
	{
		key = (Expr *) coerce_to_target_type(ctx->pstate, (Node *) key, INT2OID, INT4OID, -1,
											 COERCION_IMPLICIT, COERCE_IMPLICIT_CAST, -1);
		type = INT4OID;
	}
	if (type != INT4OID && type != INT8OID)
		ASA_UNSUPPORTED("sampling-entity keys must be smallint, integer or bigint, not %s",
						format_type_be(type));
	hash = (Expr *) makeFuncExpr(asa_func(ctx, "asa_hash", 1, &type), INT8OID, list_make1(key), InvalidOid,
								 InvalidOid, COERCE_EXPLICIT_CALL);
	return (Expr *) makeFuncExpr(asa_func(ctx, "priv_hash", 1, int8_arg), INT8OID, list_make1(hash), InvalidOid,
								 InvalidOid, COERCE_EXPLICIT_CALL);
}

static bool
asa_supported_value_type(Oid type)
{
	return type == INT2OID || type == INT4OID || type == INT8OID || type == FLOAT4OID || type == FLOAT8OID ||
		type == NUMERICOID;
}

static Aggref *
asa_rewrite_aggref(AsaCtx *ctx, Aggref *agg, Expr *mask)
{
	char	   *name = get_func_name(agg->aggfnoid);
	const char *as_name;
	Aggref	   *out;
	TargetEntry *mask_te = makeTargetEntry((Expr *) copyObject(mask), 1, NULL, false);
	Oid			argtypes[2] = {INT8OID, InvalidOid};

	if (get_func_namespace(agg->aggfnoid) != PG_CATALOG_NAMESPACE)
		ASA_UNSUPPORTED("aggregate %s has no stochastic counterpart", format_procedure(agg->aggfnoid));
	if (strcmp(name, "count") == 0)
		as_name = "as_count";
	else if (strcmp(name, "sum") == 0)
		as_name = "as_sum";
	else if (strcmp(name, "avg") == 0)
		as_name = "as_avg";
	else if (strcmp(name, "min") == 0)
		as_name = "as_min";
	else if (strcmp(name, "max") == 0)
		as_name = "as_max";
	else
		ASA_UNSUPPORTED("aggregate %s has no stochastic counterpart", format_procedure(agg->aggfnoid));
	if (agg->aggkind != AGGKIND_NORMAL || agg->agglevelsup != 0 || agg->aggdirectargs)
		ASA_UNSUPPORTED("ordered-set and outer-level aggregates are not supported");
	if (agg->aggdistinct)
		ASA_UNSUPPORTED("%s(DISTINCT ...) is not supported", name);
	if (agg->aggorder)
		ASA_UNSUPPORTED("ORDER BY inside %s(...) is not supported", name);

	out = copyObject(agg);
	if (agg->aggstar)
	{
		out->args = list_make1(mask_te);
		out->aggargtypes = list_make1_oid(INT8OID);
		out->aggstar = false;
		out->aggfnoid = asa_func(ctx, as_name, 1, argtypes);
	}
	else
	{
		TargetEntry *value_te;
		Oid			value_type;

		if (list_length(agg->args) != 1)
			ASA_UNSUPPORTED("multi-argument aggregates are not supported");
		value_te = copyObject(linitial_node(TargetEntry, agg->args));
		value_type = exprType((Node *) value_te->expr);
		if (strcmp(as_name, "as_count") == 0)
			argtypes[1] = ANYOID;
		else if (asa_supported_value_type(value_type))
			argtypes[1] = value_type;
		else
			ASA_UNSUPPORTED("%s(%s) is not supported (smallint, integer, bigint, real, double precision, numeric)",
							name, format_type_be(value_type));
		value_te->resno = 2;
		out->args = list_make2(mask_te, value_te);
		out->aggargtypes = list_make2_oid(INT8OID, value_type);
		out->aggfnoid = asa_func(ctx, as_name, 2, argtypes);
	}
	out->aggtype = FLOAT4ARRAYOID;
	out->aggcollid = InvalidOid;
	out->aggtranstype = InvalidOid;
	out->aggsplit = AGGSPLIT_SIMPLE;
	return out;
}

/* ORDER BY / DISTINCT on a rewritten column now sorts real[] (array order: world 1 first). */
static void
asa_fix_sortgroup(List *clauses, List *targetlist, Bitmapset *rewritten)
{
	ListCell   *lc;

	foreach(lc, clauses)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, targetlist);
		Oid			opfamily;
		Oid			opcintype;
		int16		strategy;
		bool		reverse = false;

		if (!bms_is_member(tle->resno, rewritten))
			continue;
		if (OidIsValid(sgc->sortop) &&
			get_ordering_op_properties(sgc->sortop, &opfamily, &opcintype, &strategy))
			reverse = strategy == BTGreaterStrategyNumber;
		get_sort_group_operators(FLOAT4ARRAYOID, OidIsValid(sgc->sortop), true, false,
								 OidIsValid(sgc->sortop) ? &sgc->sortop : NULL, &sgc->eqop, NULL, &sgc->hashable);
		if (reverse)
			sgc->sortop = get_commutator(sgc->sortop);
	}
}

/*
 * True if the query already aggregates with this extension's as_* aggregates, anywhere in
 * its expressions: written by hand, or rewritten before (EXPLAIN analyzes its query, then
 * ExplainQuery calls the hook again on the same, already rewritten query).
 */
static bool
asa_extension_aggref_walker(Node *node, Oid *schema)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref) && get_func_namespace(((Aggref *) node)->aggfnoid) == *schema)
		return true;
	if (IsA(node, Query))
		return false;			/* sub-queries are checked separately */
	return expression_tree_walker(node, asa_extension_aggref_walker, schema);
}

static bool
asa_uses_extension_aggregates(Query *query, Oid schema)
{
	return asa_extension_aggref_walker((Node *) query->targetList, &schema) ||
		asa_extension_aggref_walker(query->havingQual, &schema);
}

/* Converts expr to double precision for broadcasting to every world. */
static Node *
asa_to_float8(AsaCtx *ctx, Node *expr)
{
	Node	   *out = coerce_to_target_type(ctx->pstate, expr, exprType(expr), FLOAT8OID, -1, COERCION_EXPLICIT,
											COERCE_IMPLICIT_CAST, -1);

	if (!out)
		ASA_UNSUPPORTED("cannot combine a value of type %s with per-world results",
						format_type_be(exprType(expr)));
	return out;
}

/* Calls a lane-wise function (asa_lanes.c) on real[] / double precision arguments. */
static Expr *
asa_lane_call(AsaCtx *ctx, const char *name, List *args, Oid rettype)
{
	Oid			argtypes[2];
	ListCell   *lc;
	int			n = 0;

	foreach(lc, args)
		argtypes[n++] = exprType((Node *) lfirst(lc));
	return (Expr *) makeFuncExpr(asa_func(ctx, name, n, argtypes), rettype, args, InvalidOid, InvalidOid,
								 COERCE_EXPLICIT_CALL);
}

/*
 * Rewrites an expression over aggregates into its per-world evaluation: aggregates become
 * as_* aggregates (real[64]), + - * / become lane-wise operations, other operands are
 * broadcast, and casts are dropped (worlds are real). *lanes tells whether the result is
 * per-world.
 */
static Node *
asa_lanewise(AsaCtx *ctx, Node *node, Expr *mask, bool *lanes)
{
	*lanes = false;
	if (node == NULL || !contain_agg_clause(node))
		return node;
	*lanes = true;
	if (IsA(node, Aggref))
		return (Node *) asa_rewrite_aggref(ctx, (Aggref *) node, mask);
	if (IsA(node, RelabelType))
		return asa_lanewise(ctx, (Node *) ((RelabelType *) node)->arg, mask, lanes);
	if (IsA(node, FuncExpr) &&
		(((FuncExpr *) node)->funcformat == COERCE_IMPLICIT_CAST ||
		 ((FuncExpr *) node)->funcformat == COERCE_EXPLICIT_CAST))
		return asa_lanewise(ctx, linitial(((FuncExpr *) node)->args), mask, lanes);
	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;
		char	   *opname = get_opname(op->opno);
		int			nargs = list_length(op->args);
		const char *fn = NULL;
		List	   *args = NIL;
		ListCell   *lc;

		if (nargs == 2 && strcmp(opname, "+") == 0)
			fn = "asa_lane_add";
		else if (nargs == 2 && strcmp(opname, "-") == 0)
			fn = "asa_lane_sub";
		else if (nargs == 2 && strcmp(opname, "*") == 0)
			fn = "asa_lane_mul";
		else if (nargs == 2 && strcmp(opname, "/") == 0)
			fn = "asa_lane_div";
		else if (nargs == 1 && strcmp(opname, "-") == 0)
			fn = "asa_lane_neg";
		if (fn)
		{
			foreach(lc, op->args)
			{
				bool		arg_lanes;
				Node	   *arg = asa_lanewise(ctx, lfirst(lc), mask, &arg_lanes);

				args = lappend(args, arg_lanes ? arg : asa_to_float8(ctx, arg));
			}
			return (Node *) asa_lane_call(ctx, fn, args, FLOAT4ARRAYOID);
		}
	}
	ASA_UNSUPPORTED("this expression over aggregates cannot be evaluated per world (supported: + - * / and casts)");
	return NULL;				/* keep compiler quiet */
}

static bool asa_rewrite_level(ParseState *pstate, Query *query, AsaMeta *meta);

static bool
asa_uses_dependent(AsaMeta *meta, Query *query)
{
	AsaNestedCtx nested = {0};

	nested.meta = meta;
	asa_nested_query(query, &nested);	/* counts the relations of query itself */
	return nested.nested;
}

/*
 * A WHERE conjunct "x op (SELECT <aggregates over entity-dependent relations> ...)" is a
 * predicate over worlds: the sub-select yields one value per world, so whether a row passes
 * depends on the world (TPC-H Q17: l_quantity < (SELECT 0.2 * avg(l_quantity) ...)). The
 * sub-select is rewritten itself, and the conjunct becomes the mask of the worlds in which
 * it holds, to be AND-ed into the membership mask of the outer aggregates. Returns NULL if
 * the conjunct is an ordinary predicate.
 */
static Expr *
asa_world_predicate(AsaCtx *ctx, Node *conjunct)
{
	static const char *const ops[][2] = {
		{"<", "asa_lane_lt"}, {"<=", "asa_lane_le"}, {">", "asa_lane_gt"},
		{">=", "asa_lane_ge"}, {"=", "asa_lane_eq"}, {"<>", "asa_lane_ne"}};
	OpExpr	   *op = (OpExpr *) conjunct;
	const char *fn = NULL;
	char	   *opname;
	List	   *args = NIL;
	int			sublinks = 0;
	ListCell   *lc;

	if (!IsA(conjunct, OpExpr) || list_length(op->args) != 2)
		return NULL;
	opname = get_opname(op->opno);
	for (int i = 0; i < lengthof(ops); i++)
		if (strcmp(opname, ops[i][0]) == 0)
			fn = ops[i][1];
	if (!fn)
		return NULL;
	foreach(lc, op->args)
	{
		Node	   *arg = lfirst(lc);

		while (IsA(arg, RelabelType))
			arg = (Node *) ((RelabelType *) arg)->arg;
		if (IsA(arg, SubLink) && ((SubLink *) arg)->subLinkType == EXPR_SUBLINK &&
			((Query *) ((SubLink *) arg)->subselect)->hasAggs &&
			asa_uses_dependent(ctx->meta, (Query *) ((SubLink *) arg)->subselect))
		{
			SubLink    *sublink = (SubLink *) arg;
			Query	   *sub = (Query *) sublink->subselect;
			ParseState *sub_pstate = make_parsestate(NULL);

			/* the sub-select gets its own parse state: injected joins go into its range table */
			sub_pstate->p_sourcetext = ctx->pstate->p_sourcetext;
			sub_pstate->p_rtable = sub->rtable;
			sub_pstate->p_rteperminfos = sub->rteperminfos;
			if (!asa_rewrite_level(sub_pstate, sub, ctx->meta))
				return NULL;
			sublinks++;
			args = lappend(args, sublink);	/* now of type real[] */
		}
		else
			args = lappend(args, arg);
	}
	if (sublinks == 0)
		return NULL;
	foreach(lc, args)
		if (exprType((Node *) lfirst(lc)) != FLOAT4ARRAYOID)
		{
			if (contain_agg_clause(lfirst(lc)))
				return NULL;
			lfirst(lc) = asa_to_float8(ctx, lfirst(lc));
		}
	return asa_lane_call(ctx, fn, args, INT8OID);
}

static Expr *
asa_and_masks(AsaCtx *ctx, Expr *a, Expr *b)
{
	if (!a)
		return b;
	return (Expr *) make_op(ctx->pstate, list_make2(makeString("pg_catalog"), makeString("&")), (Node *) a,
							(Node *) b, NULL, -1);
}

/*
 * Rewrites one aggregating query level (the statement, or a world-predicate sub-select).
 * Returns false when the level aggregates only over sample-independent relations.
 */
static bool
asa_rewrite_level(ParseState *pstate, Query *query, AsaMeta *meta)
{
	AsaNestedCtx nested = {0};
	AsaCtx		ctx = {0};
	List	   *direct = NIL;
	List	   *keys = NIL;
	List	   *predicates = NIL;
	Expr	   *mask = NULL;
	Bitmapset  *rewritten = NULL;
	ListCell   *lc;
	int			rtindex;

	/* Vars are marked for column privileges through pstate, which must describe this query */
	if (pstate->p_rtable != query->rtable)
		ASA_UNSUPPORTED("unexpected parse state for this statement");
	ctx.pstate = pstate;
	ctx.query = query;
	ctx.meta = meta;

	/* predicates over worlds leave WHERE and become part of the mask */
	if (query->jointree->quals)
	{
		List	   *keep = NIL;

		foreach(lc, make_ands_implicit((Expr *) query->jointree->quals))
		{
			Expr	   *predicate = asa_world_predicate(&ctx, lfirst(lc));

			if (predicate)
				predicates = lappend(predicates, predicate);
			else
				keep = lappend(keep, lfirst(lc));
		}
		query->jointree->quals = (Node *) make_ands_explicit(keep);
		if (keep == NIL)
			query->jointree->quals = NULL;
	}

	nested.meta = meta;
	query_tree_walker(query, asa_nested_walker, &nested, QTW_EXAMINE_RTES_BEFORE);
	rtindex = 0;
	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		rtindex++;
		if (rte->rtekind == RTE_RELATION && rte->relkind != RELKIND_VIEW && asa_is_dependent(meta, rte->relid))
			direct = lappend_int(direct, rtindex);
	}
	if (nested.nested)
		ASA_UNSUPPORTED("entity-dependent relations in subqueries, CTEs or views are not supported "
						"(except in a comparison with a scalar sub-select in WHERE)");
	if (direct == NIL && predicates == NIL)
		return false;			/* aggregates over sample-independent relations only */
	if (query->hasWindowFuncs)
		ASA_UNSUPPORTED("window functions are not supported");
	if (query->groupingSets)
		ASA_UNSUPPORTED("GROUPING SETS, ROLLUP and CUBE are not supported");
	if (query->setOperations)
		ASA_UNSUPPORTED("set operations are not supported");
	if (contain_agg_clause(query->havingQual))
		ASA_UNSUPPORTED("aggregates in HAVING are not supported (a predicate over worlds)");

	asa_mark_nullable((Node *) query->jointree, false, &ctx.nullable);
	asa_collect_eqs((Node *) query->jointree, false, &ctx.eqs);
	ctx.max_rtindex = list_length(query->rtable) + ASA_MAX_INJECTED_JOINS;
	ctx.keys = (Expr **) palloc0((ctx.max_rtindex + 1) * sizeof(Expr *));

	foreach(lc, direct)
	{
		int			index = lfirst_int(lc);

		if (bms_is_member(index, ctx.nullable))
			ASA_UNSUPPORTED("entity-dependent relation \"%s\" is on the nullable side of an outer join",
							get_rel_name(rt_fetch(index, query->rtable)->relid));
	}
	foreach(lc, direct)
	{
		Expr	   *key = asa_entity_key(&ctx, lfirst_int(lc), 0);

		if (!list_member(keys, key))
			keys = lappend(keys, key);
	}
	foreach(lc, keys)
		mask = asa_and_masks(&ctx, mask, asa_key_mask(&ctx, (Expr *) lfirst(lc)));
	foreach(lc, predicates)
		mask = asa_and_masks(&ctx, mask, (Expr *) lfirst(lc));

	foreach(lc, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		bool		lanes;

		tle->expr = (Expr *) asa_lanewise(&ctx, (Node *) tle->expr, mask, &lanes);
		if (lanes)
			rewritten = bms_add_member(rewritten, tle->resno);
	}
	asa_fix_sortgroup(query->sortClause, query->targetList, rewritten);
	asa_fix_sortgroup(query->distinctClause, query->targetList, rewritten);
	return true;
}

static void
asa_rewrite_query(ParseState *pstate, Query *query)
{
	AsaMeta    *meta;
	AsaNestedCtx nested = {0};
	ListCell   *lc;
	bool		has_nested = false;

	/* a query without aggregates matters only if a sub-query (or view) might aggregate */
	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		has_nested |= rte->rtekind == RTE_SUBQUERY || (rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_VIEW);
	}
	if (!query->hasAggs && !has_nested && !query->hasSubLinks && query->cteList == NIL)
		return;
	meta = asa_load_meta();
	if (!meta || asa_uses_extension_aggregates(query, meta->schema))
		return;
	if (!query->hasAggs)
	{
		nested.meta = meta;
		query_tree_walker(query, asa_nested_walker, &nested, QTW_EXAMINE_RTES_BEFORE);
		if (nested.nested_agg)
			ASA_UNSUPPORTED("aggregation over entity-dependent relations in a subquery or view is not supported");
		return;
	}
	asa_rewrite_level(pstate, query, meta);
}

/* ---------------------------------------------------------------------------
 * Hook
 * ------------------------------------------------------------------------- */

static void
asa_post_parse_analyze(ParseState *pstate, Query *query, JumbleState *jstate)
{
	Query	   *target = query;

	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);
	if (!asa_rewrite_enabled || asa_in_rewrite)
		return;
	if (query->commandType == CMD_UTILITY)
	{
		Node	   *stmt = query->utilityStmt;

		if (IsA(stmt, ExplainStmt))
			target = (Query *) ((ExplainStmt *) stmt)->query;
		else if (IsA(stmt, CreateTableAsStmt))
			target = (Query *) ((CreateTableAsStmt *) stmt)->query;
		else if (IsA(stmt, DeclareCursorStmt))
			target = (Query *) ((DeclareCursorStmt *) stmt)->query;
		else
			return;
		if (!target || !IsA(target, Query))
			return;
	}
	if (target->commandType != CMD_SELECT)
		return;

	asa_in_rewrite = true;
	PG_TRY();
	{
		asa_rewrite_query(pstate, target);
	}
	PG_FINALLY();
	{
		asa_in_rewrite = false;
	}
	PG_END_TRY();
}

void
asa_rewrite_init(void)
{
	DefineCustomBoolVariable("simd_asa.rewrite",
							 "Rewrite aggregate queries over the sampling entity into SIMD-ASA aggregates.",
							 "Each sum/count/avg/min/max then returns one result per world (real[64]).",
							 &asa_rewrite_enabled, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("simd_asa.entity",
							   "Sampling entity of the rewritten queries (a table declared with asa_add_entity).",
							   "May be empty when exactly one sampling entity is declared.",
							   &asa_entity_setting, "", PGC_USERSET, 0, NULL, NULL, NULL);
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = asa_post_parse_analyze;
}

#else							/* PG_VERSION_NUM < 160000 */

void
asa_rewrite_init(void)
{
	/* the rewrite relies on the PostgreSQL 16 range-table permission model */
}

#endif
