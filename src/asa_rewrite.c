/*
 * asa_rewrite.c — automatic SIMD-ASA query rewrite.
 *
 * PostgreSQL counterpart of the DuckDB extension's PAC compiler (FK join injection,
 * aggregate transformation, categorical and top-k rewrites). With simd_asa.rewrite = on,
 * a query over relations owned by the sampling entity is rewritten during parse analysis,
 * one query level (statement, FROM-clause subquery, CTE, sub-select) at a time:
 *
 *   1. Entity-dependent relations are the entity itself (asa_entity) and every relation
 *      with a unique asa_link path to it. Each one's entity key is found through a join the
 *      level already contains, through an injected join, or directly when the link points
 *      at the entity; the level's row mask is the AND over the distinct keys of
 *      priv_hash(asa_hash(key), query_hash): a row belongs to world j iff every entity it
 *      derives from does.
 *   2. Aggregates over dependent rows are released: sum/count/avg/min/max become noised
 *      PAC aggregates (asa_noised_*) of the original result type. An arithmetic expression
 *      over several aggregates is evaluated per world (asa_lane_*) and released once
 *      (asa_noised); other expressions apply to the released values. With
 *      simd_asa.counters = on, aggregates return their 64 world counters (real[]) instead.
 *   3. A FROM-clause subquery that does not aggregate passes its row mask up as an extra
 *      column, so the aggregates above use it (TPC-H Q7, Q8, Q9, Q22). One that aggregates
 *      is released in place (a CTE: Q15). One grouped by the entity key (or a column that
 *      determines it) aggregates rows of a single entity: it stays exact, and its group key
 *      provides the mask (Q13; Q18's IN sub-select).
 *   4. A WHERE comparison with a scalar sub-select that aggregates over dependent rows is a
 *      predicate over worlds: the sub-select returns its world counters, and the comparison
 *      becomes the mask of the worlds where it holds. Below an aggregate it is AND-ed into
 *      the row mask (DuckDB's priv_select: Q17, Q22); otherwise it decides whether the row
 *      passes (priv_filter: Q20). HAVING over aggregates is decided the same way.
 *   5. ORDER BY ... LIMIT over aggregates ranks the groups by the mean of their worlds and
 *      releases only the selected ones (DuckDB's top-k pushdown: Q3, Q10, Q21).
 *
 * EXISTS / IN sub-selects without aggregation over dependent rows are ordinary filters,
 * as in DuckDB. Rewriting at parse analysis (post_parse_analyze_hook) rather than in the
 * planner keeps the result types consistent for prepared statements and the extended
 * protocol. Shapes the rewrite does not implement are rejected rather than silently left
 * unrewritten.
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
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
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/rel.h"

#include "asa_common.h"
#include "asa_noise.h"

#if PG_VERSION_NUM >= 160000

#define ASA_MAX_LINK_DEPTH 32
#define ASA_MAX_INJECTED_JOINS 64

static bool asa_rewrite_enabled = false;
static bool asa_counters_setting = false;
static bool asa_pushdown_topk = true;
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

typedef struct AsaCte
{
	CommonTableExpr *cte;
	bool		released;		/* its aggregates were released: its rows are entity-independent */
	bool		dependent;		/* its rows depend on the entity (no mask is passed up) */
} AsaCte;

/* Bind a CTE reference to its lexical declaration before rewriting any levels. */
typedef struct AsaCteRef
{
	RangeTblEntry *rte;
	CommonTableExpr *cte;
} AsaCteRef;

/* One statement being rewritten. */
typedef struct AsaStmt
{
	AsaMeta    *meta;
	uint64		query_hash;		/* constant of every mask, noised aggregate and filter */
	bool		counters;		/* simd_asa.counters: world counters instead of releases */
	const char *sourcetext;
	List	   *ctes;			/* AsaCte *: CTEs processed so far */
	List	   *cte_refs;		/* AsaCteRef *: lexical bindings, not names */
	List	   *query_stack;	/* Query *: current level first during binding */
	List	   *done;			/* Query *: sub-selects already rewritten */
} AsaStmt;

/* One query level being rewritten. */
typedef struct AsaCtx
{
	AsaStmt    *stmt;
	ParseState *pstate;			/* describes query: injected joins and column privileges */
	Query	   *query;
	AsaMeta    *meta;
	Bitmapset  *nullable;		/* rtindexes on the nullable side of an outer join */
	List	   *eqs;			/* AsaEq *: top-level equality conjuncts between two Vars */
	Expr	  **keys;			/* entity key per rtindex (memo) */
	int			max_rtindex;
	int			injected;
	List	   *lane_sublinks;	/* SubLink *: predicate sub-selects returning world counters */
	bool		categorical;	/* the mask holds predicates over worlds: release via counters */
} AsaCtx;

typedef enum AsaKind
{
	ASA_TOP,					/* the statement */
	ASA_DERIVED,				/* FROM-clause subquery or CTE */
	ASA_PREDICATE,				/* scalar sub-select of a predicate over worlds */
	ASA_SCALAR,					/* other scalar sub-select */
	ASA_FILTER					/* EXISTS / IN / ANY / ALL sub-select */
} AsaKind;

typedef struct AsaLevelResult
{
	bool		rewritten;		/* its aggregates were rewritten (released or counters) */
	AttrNumber	mask_attno;		/* output column passing the row mask up, 0 if none */
	bool		categorical;	/* that mask holds predicates over worlds */
} AsaLevelResult;

/* ---------------------------------------------------------------------------
 * Join tree analysis
 * ------------------------------------------------------------------------- */

typedef struct AsaEq
{
	Var		   *a;
	Var		   *b;
} AsaEq;


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
 * Which parts of a query read entity-dependent relations
 * ------------------------------------------------------------------------- */

/* PostgreSQL's ctelevelsup counts query levels, including CTE query bodies. */
static bool
asa_bind_ctes_walker(Node *node, AsaStmt *st)
{
	ListCell   *lc;

	if (node == NULL)
		return false;
	if (IsA(node, Query))
	{
		st->query_stack = lcons(node, st->query_stack);
		query_tree_walker((Query *) node, asa_bind_ctes_walker, st, QTW_EXAMINE_RTES_BEFORE);
		st->query_stack = list_delete_first(st->query_stack);
		return false;
	}
	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		if (rte->rtekind == RTE_CTE)
		{
			Query *owner = list_nth(st->query_stack, rte->ctelevelsup);

			foreach(lc, owner->cteList)
			{
				CommonTableExpr *cte = lfirst_node(CommonTableExpr, lc);

				if (strcmp(cte->ctename, rte->ctename) == 0)
				{
					AsaCteRef *ref = palloc(sizeof(AsaCteRef));

					ref->rte = rte;
					ref->cte = cte;
					st->cte_refs = lappend(st->cte_refs, ref);
					break;
				}
			}
		}
		return false;
	}
	return expression_tree_walker(node, asa_bind_ctes_walker, st);
}

static CommonTableExpr *
asa_cte_declaration(AsaStmt *st, RangeTblEntry *rte)
{
	ListCell   *lc;

	foreach(lc, st->cte_refs)
	{
		AsaCteRef *ref = lfirst(lc);

		if (ref->rte == rte)
			return ref->cte;
	}
	ASA_UNSUPPORTED("cannot resolve CTE \"%s\" in its lexical scope", rte->ctename);
	return NULL;
}

static AsaCte *
asa_find_cte(AsaStmt *st, RangeTblEntry *rte)
{
	CommonTableExpr *declaration = asa_cte_declaration(st, rte);
	ListCell   *lc;

	foreach(lc, st->ctes)
		if (((AsaCte *) lfirst(lc))->cte == declaration)
			return lfirst(lc);
	return NULL;
}

static Query *
asa_view_query(Oid relid)
{
	Relation	view = table_open(relid, AccessShareLock);
	Query	   *query = copyObject(get_view_query(view));

	table_close(view, NoLock);
	return query;
}

static bool asa_depends_walker(Node *node, AsaStmt *st);

/*
 * Whether query, its sub-queries, sub-selects, CTEs or views read an entity-dependent
 * relation. A processed CTE counts by its status: released aggregates are independent.
 */
static bool
asa_depends_query(Query *query, AsaStmt *st)
{
	ListCell   *lc;

	foreach(lc, query->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);
		bool		processed = false;
		ListCell   *lc2;

		foreach(lc2, st->ctes)
			processed |= ((AsaCte *) lfirst(lc2))->cte == cte;
		if (!processed && asa_depends_walker(cte->ctequery, st))
			return true;
	}
	return query_tree_walker(query, asa_depends_walker, st, QTW_EXAMINE_RTES_BEFORE | QTW_IGNORE_CTE_SUBQUERIES);
}

static bool
asa_depends_walker(Node *node, AsaStmt *st)
{
	if (node == NULL)
		return false;
	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		if (rte->rtekind == RTE_RELATION)
			return rte->relkind == RELKIND_VIEW ? asa_depends_query(asa_view_query(rte->relid), st) :
				asa_is_dependent(st->meta, rte->relid);
		if (rte->rtekind == RTE_CTE)
		{
			AsaCte	   *cte = asa_find_cte(st, rte);
			CommonTableExpr *declaration;

			if (cte)
				return cte->dependent;
			declaration = asa_cte_declaration(st, rte);
			/* The CTE body is also visited directly; do not follow recursive edges. */
			return !declaration->cterecursive && asa_depends_query((Query *) declaration->ctequery, st);
		}
		return false;			/* subquery RTEs are descended into by range_table_walker */
	}
	if (IsA(node, Query))
		return asa_depends_query((Query *) node, st);
	return expression_tree_walker(node, asa_depends_walker, st);
}

/* ---------------------------------------------------------------------------
 * Masks and aggregates
 * ------------------------------------------------------------------------- */

static Oid
asa_func(AsaCtx *ctx, const char *name, int nargs, Oid *argtypes)
{
	List	   *qualified = list_make2(makeString(get_namespace_name(ctx->meta->schema)),
									   makeString(pstrdup(name)));

	return LookupFuncName(qualified, nargs, argtypes, false);
}

static Const *
asa_query_hash_const(AsaCtx *ctx)
{
	return makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum((int64) ctx->stmt->query_hash), false,
					 FLOAT8PASSBYVAL);
}

/* priv_hash(asa_hash(key), query_hash); smallint keys hash like integers (as DuckDB's hash()). */
static Expr *
asa_key_mask(AsaCtx *ctx, Expr *key)
{
	Oid			type = exprType((Node *) key);
	Oid			priv_args[2] = {INT8OID, INT8OID};
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
	return (Expr *) makeFuncExpr(asa_func(ctx, "priv_hash", 2, priv_args), INT8OID,
								 list_make2(hash, asa_query_hash_const(ctx)), InvalidOid, InvalidOid,
								 COERCE_EXPLICIT_CALL);
}

static Expr *
asa_and_masks(AsaCtx *ctx, Expr *a, Expr *b)
{
	if (!a)
		return b;
	return (Expr *) make_op(ctx->pstate, list_make2(makeString("pg_catalog"), makeString("&")), (Node *) a,
							(Node *) b, NULL, -1);
}

static bool
asa_supported_value_type(Oid type)
{
	return type == INT2OID || type == INT4OID || type == INT8OID || type == FLOAT4OID || type == FLOAT8OID ||
		type == NUMERICOID;
}

typedef enum AsaAggForm
{
	ASA_AGG_COUNTERS,			/* as_*(mask [, value]): world counters */
	ASA_AGG_QCOUNTERS,			/* asa_counters_*(mask [, value], query_hash): the same, sharing ... */
	ASA_AGG_NOISED				/* ... the transition state of asa_noised_*(...): PAC release */
} AsaAggForm;

/* The stochastic counterpart of agg over rows with membership mask. */
static Aggref *
asa_make_aggref(AsaCtx *ctx, Aggref *agg, Expr *mask, AsaAggForm form)
{
	char	   *name = get_func_name(agg->aggfnoid);
	static const char *const supported[] = {"count", "sum", "avg", "min", "max"};
	const char *base = NULL;
	char		fname[NAMEDATALEN];
	List	   *args = list_make1(makeTargetEntry((Expr *) copyObject(mask), 1, NULL, false));
	List	   *types = list_make1_oid(INT8OID);
	Oid			argtypes[3] = {INT8OID, InvalidOid, InvalidOid};
	int			nargs = 1;
	Aggref	   *out;
	Oid			fn;

	if (get_func_namespace(agg->aggfnoid) == PG_CATALOG_NAMESPACE)
		for (int i = 0; i < lengthof(supported); i++)
			if (strcmp(name, supported[i]) == 0)
				base = supported[i];
	if (!base)
		ASA_UNSUPPORTED("aggregate %s has no stochastic counterpart", format_procedure(agg->aggfnoid));
	if (agg->aggkind != AGGKIND_NORMAL || agg->agglevelsup != 0 || agg->aggdirectargs)
		ASA_UNSUPPORTED("ordered-set and outer-level aggregates are not supported");
	if (agg->aggdistinct)
		ASA_UNSUPPORTED("%s(DISTINCT ...) is not supported", name);
	if (agg->aggorder)
		ASA_UNSUPPORTED("ORDER BY inside %s(...) is not supported", name);
	snprintf(fname, sizeof(fname), form == ASA_AGG_COUNTERS ? "as_%s" : form == ASA_AGG_QCOUNTERS ?
			 "asa_counters_%s" : "asa_noised_%s", base);

	if (!agg->aggstar)
	{
		TargetEntry *value_te;
		Oid			value_type;

		if (list_length(agg->args) != 1)
			ASA_UNSUPPORTED("multi-argument aggregates are not supported");
		value_te = copyObject(linitial_node(TargetEntry, agg->args));
		value_type = exprType((Node *) value_te->expr);
		if (strcmp(base, "count") == 0)
			argtypes[nargs] = ANYOID;
		else if (asa_supported_value_type(value_type))
			argtypes[nargs] = value_type;
		else
			ASA_UNSUPPORTED("%s(%s) is not supported (smallint, integer, bigint, real, double precision, numeric)",
							name, format_type_be(value_type));
		value_te->resno = ++nargs;
		args = lappend(args, value_te);
		types = lappend_oid(types, value_type);
	}
	if (form != ASA_AGG_COUNTERS)
	{
		argtypes[nargs++] = INT8OID;
		args = lappend(args, makeTargetEntry((Expr *) asa_query_hash_const(ctx), nargs, NULL, false));
		types = lappend_oid(types, INT8OID);
	}
	fn = asa_func(ctx, fname, nargs, argtypes);

	out = copyObject(agg);
	out->aggfnoid = fn;
	out->args = args;
	out->aggargtypes = types;
	out->aggstar = false;
	if (form == ASA_AGG_NOISED)
	{
		if (get_func_rettype(fn) != agg->aggtype)
			ASA_UNSUPPORTED("%s returns %s, not %s", format_procedure(fn), format_type_be(get_func_rettype(fn)),
							format_type_be(agg->aggtype));
	}
	else
	{
		out->aggtype = FLOAT4ARRAYOID;
		out->aggcollid = InvalidOid;
	}
	out->aggtranstype = InvalidOid;
	out->aggsplit = AGGSPLIT_SIMPLE;
	return out;
}

/* ---------------------------------------------------------------------------
 * Per-world (lane-wise) evaluation
 * ------------------------------------------------------------------------- */

/* A per-world value: an aggregate of this level, or a predicate sub-select's world counters. */
static bool
asa_is_lane_source(AsaCtx *ctx, Node *node)
{
	if (IsA(node, Aggref))
		return ((Aggref *) node)->agglevelsup == 0;
	return IsA(node, SubLink) && list_member_ptr(ctx->lane_sublinks, node);
}

typedef struct AsaCountCtx
{
	AsaCtx	   *ctx;
	int			count;
} AsaCountCtx;

static bool
asa_count_lanes_walker(Node *node, AsaCountCtx *c)
{
	if (node == NULL)
		return false;
	if (asa_is_lane_source(c->ctx, node))
	{
		c->count++;
		return false;
	}
	if (IsA(node, Aggref))
		ASA_UNSUPPORTED("outer-level aggregates are not supported");
	if (IsA(node, SubLink))
		return asa_count_lanes_walker(((SubLink *) node)->testexpr, c); /* not into the sub-select */
	if (IsA(node, Query))
		return false;
	return expression_tree_walker(node, asa_count_lanes_walker, c);
}

/* Number of per-world values in node. */
static int
asa_count_lanes(AsaCtx *ctx, Node *node)
{
	AsaCountCtx c = {ctx, 0};

	asa_count_lanes_walker(node, &c);
	return c.count;
}

/* The argument of a cast, or NULL. */
static Node *
asa_cast_arg(Node *node)
{
	if (IsA(node, RelabelType))
		return (Node *) ((RelabelType *) node)->arg;
	if (IsA(node, FuncExpr) && ((FuncExpr *) node)->args != NIL &&
		(((FuncExpr *) node)->funcformat == COERCE_IMPLICIT_CAST ||
		 ((FuncExpr *) node)->funcformat == COERCE_EXPLICIT_CAST))
		return linitial(((FuncExpr *) node)->args);
	return NULL;
}

/* The lane function of + - * / or unary minus, else NULL. */
static const char *
asa_lane_arith(Node *node)
{
	static const char *const ops[][3] = {
		{"+", "asa_lane_add", "2"}, {"-", "asa_lane_sub", "2"}, {"*", "asa_lane_mul", "2"},
		{"/", "asa_lane_div", "2"}, {"-", "asa_lane_neg", "1"}};
	OpExpr	   *op = (OpExpr *) node;
	char	   *opname;

	if (!IsA(node, OpExpr))
		return NULL;
	opname = get_opname(op->opno);
	for (int i = 0; i < lengthof(ops); i++)
		if (opname && strcmp(opname, ops[i][0]) == 0 && list_length(op->args) == atoi(ops[i][2]))
			return ops[i][1];
	return NULL;
}

/* The lane comparison of < <= > >= = <>, else NULL. */
static const char *
asa_lane_cmp(Node *node)
{
	static const char *const ops[][2] = {
		{"<", "asa_lane_lt"}, {"<=", "asa_lane_le"}, {">", "asa_lane_gt"},
		{">=", "asa_lane_ge"}, {"=", "asa_lane_eq"}, {"<>", "asa_lane_ne"}};
	OpExpr	   *op = (OpExpr *) node;
	char	   *opname;

	if (!IsA(node, OpExpr) || list_length(op->args) != 2)
		return NULL;
	opname = get_opname(op->opno);
	for (int i = 0; i < lengthof(ops); i++)
		if (opname && strcmp(opname, ops[i][0]) == 0)
			return ops[i][1];
	return NULL;
}

/* Whether node can be evaluated per world: per-world values under + - * / and casts. */
static bool
asa_lane_able(AsaCtx *ctx, Node *node)
{
	Node	   *arg;
	ListCell   *lc;

	if (asa_count_lanes(ctx, node) == 0 || asa_is_lane_source(ctx, node))
		return true;
	if ((arg = asa_cast_arg(node)) != NULL)
		return asa_lane_able(ctx, arg);
	if (!asa_lane_arith(node))
		return false;
	foreach(lc, ((OpExpr *) node)->args)
		if (!asa_lane_able(ctx, lfirst(lc)))
			return false;
	return true;
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

static Expr *
asa_call(AsaCtx *ctx, const char *name, List *args, Oid rettype)
{
	Oid			argtypes[3];
	ListCell   *lc;
	int			n = 0;

	foreach(lc, args)
		argtypes[n++] = exprType((Node *) lfirst(lc));
	return (Expr *) makeFuncExpr(asa_func(ctx, name, n, argtypes), rettype, args, InvalidOid, InvalidOid,
								 COERCE_EXPLICIT_CALL);
}

/*
 * Rewrites a lane-able expression into its per-world evaluation (real[64]): aggregates
 * become counter aggregates of the given form, + - * / become lane-wise operations, other
 * operands are broadcast, and casts are dropped (worlds are real). *lanes tells whether
 * the result is per-world.
 */
static Node *
asa_lanewise(AsaCtx *ctx, Node *node, Expr *mask, AsaAggForm form, bool *lanes)
{
	Node	   *arg;
	const char *fn;

	*lanes = false;
	if (node == NULL || asa_count_lanes(ctx, node) == 0)
		return node;
	*lanes = true;
	if (IsA(node, Aggref))
		return (Node *) asa_make_aggref(ctx, (Aggref *) node, mask, form);
	if (IsA(node, SubLink))
		return node;			/* a predicate sub-select, already returning real[] */
	if ((arg = asa_cast_arg(node)) != NULL)
		return asa_lanewise(ctx, arg, mask, form, lanes);
	if ((fn = asa_lane_arith(node)) != NULL)
	{
		List	   *args = NIL;
		ListCell   *lc;

		foreach(lc, ((OpExpr *) node)->args)
		{
			bool		arg_lanes;
			Node	   *a = asa_lanewise(ctx, lfirst(lc), mask, form, &arg_lanes);

			args = lappend(args, arg_lanes ? a : asa_to_float8(ctx, a));
		}
		return (Node *) asa_call(ctx, fn, args, FLOAT4ARRAYOID);
	}
	ASA_UNSUPPORTED("this expression cannot be evaluated per world (supported: + - * / and casts)");
	return NULL;				/* keep compiler quiet */
}

/* ---------------------------------------------------------------------------
 * Predicates over worlds
 * ------------------------------------------------------------------------- */

/* Whether pred is a comparison (or AND / OR / NOT of comparisons) of per-world values. */
static bool
asa_world_mask_able(AsaCtx *ctx, Node *pred)
{
	ListCell   *lc;

	if (IsA(pred, BoolExpr))
	{
		foreach(lc, ((BoolExpr *) pred)->args)
			if (asa_count_lanes(ctx, lfirst(lc)) == 0 || !asa_world_mask_able(ctx, lfirst(lc)))
				return false;
		return true;
	}
	if (!asa_lane_cmp(pred) || asa_count_lanes(ctx, pred) == 0)
		return false;
	foreach(lc, ((OpExpr *) pred)->args)
		if (!asa_lane_able(ctx, lfirst(lc)))
			return false;
	return true;
}

/* The mask of the worlds in which pred holds (NULL worlds: false). */
static Expr *
asa_world_mask(AsaCtx *ctx, Node *pred, Expr *mask, AsaAggForm form)
{
	List	   *args = NIL;
	ListCell   *lc;

	if (IsA(pred, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) pred;
		Expr	   *out = NULL;

		foreach(lc, b->args)
		{
			Expr	   *m = asa_world_mask(ctx, lfirst(lc), mask, form);

			if (b->boolop == NOT_EXPR)
				return (Expr *) make_op(ctx->pstate, list_make2(makeString("pg_catalog"), makeString("~")), NULL,
										(Node *) m, NULL, -1);
			out = !out ? m : (Expr *) make_op(ctx->pstate,
											  list_make2(makeString("pg_catalog"),
														 makeString(b->boolop == AND_EXPR ? "&" : "|")),
											  (Node *) out, (Node *) m, NULL, -1);
		}
		return out;
	}
	foreach(lc, ((OpExpr *) pred)->args)
	{
		bool		lanes;
		Node	   *a = asa_lanewise(ctx, lfirst(lc), mask, form, &lanes);

		args = lappend(args, lanes ? a : asa_to_float8(ctx, a));
	}
	return asa_call(ctx, asa_lane_cmp(pred), args, INT8OID);
}

/* Whether a row whose predicate holds in the worlds of mask passes (DuckDB's priv_filter). */
static Expr *
asa_filter_call(AsaCtx *ctx, Expr *mask)
{
	return asa_call(ctx, "asa_filter", list_make2(mask, asa_query_hash_const(ctx)), BOOLOID);
}

typedef struct AsaSublinkCtx
{
	AsaCtx	   *ctx;
	List	   *found;
} AsaSublinkCtx;

/* Scalar sub-selects in node (not nested in others) aggregating over dependent relations. */
static bool
asa_predicate_sublink_walker(Node *node, AsaSublinkCtx *c)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		Query	   *sub = (Query *) sublink->subselect;

		if (sublink->subLinkType == EXPR_SUBLINK && sub->hasAggs && sub->groupClause == NIL &&
			!list_member_ptr(c->ctx->stmt->done, sub) && asa_depends_query(sub, c->ctx->stmt))
			c->found = lappend(c->found, sublink);
		return false;
	}
	if (IsA(node, Query))
		return false;
	return expression_tree_walker(node, asa_predicate_sublink_walker, c);
}

static AsaLevelResult asa_level(AsaStmt *st, Query *query, AsaKind kind, bool consumed, bool consumer_grouped);

/*
 * A WHERE conjunct comparing with scalar sub-selects that aggregate over entity-dependent
 * relations (TPC-H Q17: l_quantity < (SELECT 0.2 * avg(l_quantity) ...)) is a predicate over
 * worlds: the sub-selects return their world counters, and the conjunct becomes the mask of
 * the worlds in which it holds. Returns NULL if the conjunct is an ordinary predicate.
 */
static Expr *
asa_world_predicate(AsaCtx *ctx, Node *conjunct)
{
	AsaSublinkCtx c = {ctx, NIL};
	List	   *saved = ctx->lane_sublinks;
	ListCell   *lc;

	asa_predicate_sublink_walker(conjunct, &c);
	if (c.found == NIL)
		return NULL;
	ctx->lane_sublinks = list_concat_copy(saved, c.found);
	if (!asa_world_mask_able(ctx, conjunct))
	{
		ctx->lane_sublinks = saved; /* their release is compared instead */
		return NULL;
	}
	foreach(lc, c.found)
		if (!asa_level(ctx->stmt, (Query *) ((SubLink *) lfirst(lc))->subselect, ASA_PREDICATE, false,
					   false).rewritten)
			ASA_UNSUPPORTED("this sub-select cannot be evaluated per world");
	return asa_world_mask(ctx, conjunct, NULL, ASA_AGG_COUNTERS);
}

/* ---------------------------------------------------------------------------
 * Release
 * ------------------------------------------------------------------------- */

/* asa_noised(lanes) cast to the type of the expression it releases. */
static Expr *
asa_noised_lanes(AsaCtx *ctx, Node *lanes, Oid type, int32 typmod)
{
	Expr	   *noised = asa_call(ctx, "asa_noised", list_make2(lanes, asa_query_hash_const(ctx)), FLOAT8OID);
	Node	   *out = coerce_to_target_type(ctx->pstate, (Node *) noised, FLOAT8OID, type, typmod,
											COERCION_EXPLICIT, COERCE_IMPLICIT_CAST, -1);

	if (!out)
		ASA_UNSUPPORTED("cannot release a value of type %s", format_type_be(type));
	return (Expr *) out;
}

/*
 * A release unit: an aggregate (a fused noised aggregate), or an arithmetic expression over
 * several aggregates that is evaluated per world and released once (DuckDB:
 * as_noised(list_transform(...)), TPC-H Q8, Q14). Other expressions over aggregates apply to
 * the released values. Where the mask holds predicates over worlds, DuckDB's categorical
 * rewrite releases every arithmetic expression over aggregates per world (TPC-H Q17).
 */
static bool
asa_is_unit(AsaCtx *ctx, Node *node)
{
	if (IsA(node, Aggref))
		return asa_is_lane_source(ctx, node);
	return asa_count_lanes(ctx, node) >= (ctx->categorical ? 1 : 2) && asa_lane_able(ctx, node);
}

typedef struct AsaReleaseCtx
{
	AsaCtx	   *ctx;
	Expr	   *mask;
} AsaReleaseCtx;

static Node *
asa_release_mutator(Node *node, AsaReleaseCtx *rc)
{
	if (node == NULL || asa_count_lanes(rc->ctx, node) == 0)
		return node;
	if (IsA(node, Aggref) && !rc->ctx->categorical)
		return (Node *) asa_make_aggref(rc->ctx, (Aggref *) node, rc->mask, ASA_AGG_NOISED);
	if (asa_is_unit(rc->ctx, node))
	{
		bool		lanes;

		return (Node *) asa_noised_lanes(rc->ctx, asa_lanewise(rc->ctx, node, rc->mask, ASA_AGG_QCOUNTERS, &lanes),
										 exprType(node), exprTypmod(node));
	}
	return expression_tree_mutator(node, asa_release_mutator, rc);
}

/* HAVING: conjuncts over aggregates decide per group whether it passes (priv_filter). */
static Node *
asa_release_having(AsaCtx *ctx, Node *qual, Expr *mask)
{
	List	   *out = NIL;
	ListCell   *lc;

	foreach(lc, make_ands_implicit((Expr *) qual))
	{
		Node	   *conjunct = lfirst(lc);

		if (asa_count_lanes(ctx, conjunct) == 0)
			out = lappend(out, conjunct);
		else if (asa_world_mask_able(ctx, conjunct))
			out = lappend(out, asa_filter_call(ctx, asa_world_mask(ctx, conjunct, mask, ASA_AGG_QCOUNTERS)));
		else
		{
			AsaReleaseCtx rc = {ctx, mask};

			out = lappend(out, asa_release_mutator(conjunct, &rc));
		}
	}
	return (Node *) make_ands_explicit(out);
}

/* ORDER BY / DISTINCT on retyped columns: re-derive the operators, keeping the direction. */
static void
asa_retype_sortgroup(List *clauses, List *targetlist, Bitmapset *retyped, Oid type)
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

		if (!bms_is_member(tle->resno, retyped))
			continue;
		if (OidIsValid(sgc->sortop) &&
			get_ordering_op_properties(sgc->sortop, &opfamily, &opcintype, &strategy))
			reverse = strategy == BTGreaterStrategyNumber;
		get_sort_group_operators(type, OidIsValid(sgc->sortop), true, false,
								 OidIsValid(sgc->sortop) ? &sgc->sortop : NULL, &sgc->eqop, NULL, &sgc->hashable);
		if (reverse)
			sgc->sortop = get_commutator(sgc->sortop);
	}
}

typedef struct AsaUnitsCtx
{
	AsaCtx	   *ctx;
	List	   *units;
} AsaUnitsCtx;

static bool
asa_collect_units_walker(Node *node, AsaUnitsCtx *u)
{
	if (node == NULL || asa_count_lanes(u->ctx, node) == 0)
		return false;
	if (asa_is_unit(u->ctx, node))
	{
		u->units = lappend(u->units, node);
		return false;
	}
	if (IsA(node, Query))
		return false;
	return expression_tree_walker(node, asa_collect_units_walker, u);
}

typedef struct AsaReplaceCtx
{
	List	   *from;
	List	   *to;
} AsaReplaceCtx;

/* Replaces the nodes in from (by identity) with copies of the nodes in to. */
static Node *
asa_replace_mutator(Node *node, AsaReplaceCtx *r)
{
	ListCell   *a;
	ListCell   *b;

	if (node == NULL)
		return NULL;
	forboth(a, r->from, b, r->to)
		if (lfirst(a) == node)
		return copyObject(lfirst(b));
	return expression_tree_mutator(node, asa_replace_mutator, r);
}

static Expr *
asa_coerce(AsaCtx *ctx, Expr *expr, Oid type, int32 typmod)
{
	Node	   *out = coerce_to_target_type(ctx->pstate, (Node *) expr, exprType((Node *) expr), type, typmod,
											COERCION_EXPLICIT, COERCE_IMPLICIT_CAST, -1);

	if (!out)
		ASA_UNSUPPORTED("cannot convert double precision to %s", format_type_be(type));
	return (Expr *) out;
}

/*
 * Top-k pushdown (pac_topk_rewriter.cpp): ORDER BY ... LIMIT over aggregates ranks the groups
 * by the mean of their worlds (priv_mean) and releases only the selected ones. The query
 * becomes
 *
 *   SELECT <outputs, units released: asa_noised(counters)> FROM
 *     (SELECT <outputs, units ranked by priv_mean(counters)>, <unit counters> ...
 *      ORDER BY ... LIMIT k) asa_topk
 *   ORDER BY <the same keys>
 *
 * Applies when every output over aggregates is a function of its release units alone.
 */
static bool
asa_topk(AsaCtx *ctx, Expr *mask)
{
	Query	   *q = ctx->query;
	Query	   *inner;
	Query	   *outer;
	List	   *units_per_tle = NIL;	/* List * of units, per output (NIL: none) */
	List	   *attnos_per_tle = NIL;	/* List * of counter column attnos, per output */
	List	   *inner_tl = NIL;
	List	   *outer_tl = NIL;
	List	   *outer_sort = NIL;
	Bitmapset  *retyped = NULL;
	AttrNumber	resno;
	Index		sortref = 0;
	bool		ranked = false;
	ParseState *ps;
	ParseNamespaceItem *nsitem;
	RangeTblRef *rtr;
	ListCell   *lc;
	ListCell   *lc2;
	ListCell   *lc3;

	if (!asa_pushdown_topk || !q->limitCount || q->limitOption == LIMIT_OPTION_WITH_TIES || q->distinctClause ||
		q->hasDistinctOn || q->rowMarks || q->hasTargetSRFs || q->hasWindowFuncs)
		return false;
	foreach(lc, q->sortClause)
		ranked |= asa_count_lanes(ctx, get_sortgroupclause_expr(lfirst(lc), q->targetList)) > 0;
	if (!ranked)
		return false;
	foreach(lc, q->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		AsaUnitsCtx u = {ctx, NIL};
		AsaReplaceCtx r = {NIL, NIL};
		ListCell   *lcu;
		Node	   *rest;

		asa_collect_units_walker((Node *) tle->expr, &u);
		foreach(lcu, u.units)
		{
			r.from = lappend(r.from, lfirst(lcu));
			r.to = lappend(r.to, makeNullConst(exprType(lfirst(lcu)), exprTypmod(lfirst(lcu)),
											   exprCollation(lfirst(lcu))));
		}
		rest = asa_replace_mutator((Node *) tle->expr, &r);
		if (u.units && (contain_vars_of_level(rest, 0) || checkExprHasSubLink(rest)))
			return false;		/* also reads columns: release in place instead */
		units_per_tle = lappend(units_per_tle, u.units);
	}

	/* inner: outputs (all visible, units ranked by their mean), then the unit counters */
	resno = list_length(q->targetList);
	forboth(lc, q->targetList, lc2, units_per_tle)
	{
		TargetEntry *tle = flatCopyTargetEntry((TargetEntry *) lfirst(lc));
		List	   *units = (List *) lfirst(lc2);
		List	   *attnos = NIL;
		AsaReplaceCtx means = {NIL, NIL};
		ListCell   *lcu;

		tle->resjunk = false;
		if (!tle->resname)
			tle->resname = pstrdup("?column?");
		foreach(lcu, units)
		{
			bool		lanes;
			Node	   *unit = lfirst(lcu);
			Node	   *counters = asa_lanewise(ctx, unit, mask, ASA_AGG_QCOUNTERS, &lanes);
			Expr	   *mean = asa_call(ctx, "priv_mean", list_make1(copyObject(counters)), FLOAT8OID);

			attnos = lappend_int(attnos, ++resno);
			inner_tl = lappend(inner_tl, makeTargetEntry((Expr *) counters, resno, pstrdup("asa_counters"), false));
			means.from = lappend(means.from, unit);
			means.to = lappend(means.to, (unit == (Node *) tle->expr) ? mean :
							   asa_coerce(ctx, mean, exprType(unit), exprTypmod(unit)));
		}
		if (units)
		{
			if (linitial(units) == (Node *) tle->expr)
				retyped = bms_add_member(retyped, tle->resno);	/* ranked by the double mean */
			tle->expr = (Expr *) asa_replace_mutator((Node *) tle->expr, &means);
		}
		attnos_per_tle = lappend(attnos_per_tle, attnos);
		inner_tl = lappend(inner_tl, tle);
	}
	/* outputs first, in their original order and resnos */
	{
		List	   *outputs = NIL;
		List	   *counters = NIL;

		foreach(lc, inner_tl)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);

			if (tle->resno <= list_length(q->targetList))
				outputs = lappend(outputs, tle);
			else
				counters = lappend(counters, tle);
		}
		inner_tl = list_concat(outputs, counters);
	}
	inner = makeNode(Query);
	memcpy(inner, q, sizeof(Query));
	inner->targetList = inner_tl;
	inner->sortClause = copyObject(q->sortClause);
	asa_retype_sortgroup(inner->sortClause, inner->targetList, retyped, FLOAT8OID);

	/* outer: release the units of the selected groups, in the same order */
	ps = make_parsestate(NULL);
	ps->p_sourcetext = ctx->stmt->sourcetext;
	nsitem = addRangeTableEntryForSubquery(ps, inner, makeAlias("asa_topk", NIL), false, true);
	forthree(lc, q->targetList, lc2, units_per_tle, lc3, attnos_per_tle)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		TargetEntry *out;
		Expr	   *expr;

		if (tle->resjunk)
			continue;
		if (lfirst(lc2))
		{
			AsaReplaceCtx r = {NIL, NIL};
			ListCell   *lcu;
			ListCell   *lca;

			forboth(lcu, (List *) lfirst(lc2), lca, (List *) lfirst(lc3))
			{
				Node	   *unit = lfirst(lcu);
				Var		   *counters = makeVar(nsitem->p_rtindex, lfirst_int(lca), FLOAT4ARRAYOID, -1, InvalidOid, 0);

				r.from = lappend(r.from, unit);
				r.to = lappend(r.to, asa_noised_lanes(ctx, (Node *) counters, exprType(unit), exprTypmod(unit)));
			}
			expr = (Expr *) asa_replace_mutator((Node *) tle->expr, &r);
		}
		else
			expr = (Expr *) makeVar(nsitem->p_rtindex, tle->resno, exprType((Node *) tle->expr),
									exprTypmod((Node *) tle->expr), exprCollation((Node *) tle->expr), 0);
		out = makeTargetEntry(expr, tle->resno, tle->resname, false);
		out->resorigtbl = tle->resorigtbl;
		out->resorigcol = tle->resorigcol;
		outer_tl = lappend(outer_tl, out);
	}
	foreach(lc, inner->sortClause)
	{
		SortGroupClause *sgc = copyObject((SortGroupClause *) lfirst(lc));
		TargetEntry *key = get_sortgroupclause_tle(sgc, inner->targetList);
		TargetEntry *tle = makeTargetEntry((Expr *) makeVar(nsitem->p_rtindex, key->resno,
														  exprType((Node *) key->expr), exprTypmod((Node *) key->expr),
														  exprCollation((Node *) key->expr), 0),
										   list_length(outer_tl) + 1, NULL, true);

		tle->ressortgroupref = sgc->tleSortGroupRef = ++sortref;
		outer_tl = lappend(outer_tl, tle);
		outer_sort = lappend(outer_sort, sgc);
	}
	outer = makeNode(Query);
	outer->commandType = CMD_SELECT;
	outer->querySource = q->querySource;
	outer->queryId = q->queryId;
	outer->canSetTag = q->canSetTag;
	outer->rtable = ps->p_rtable;
	outer->rteperminfos = ps->p_rteperminfos;
	rtr = makeNode(RangeTblRef);
	rtr->rtindex = nsitem->p_rtindex;
	outer->jointree = makeFromExpr(list_make1(rtr), NULL);
	outer->targetList = outer_tl;
	outer->sortClause = outer_sort;
	outer->stmt_location = q->stmt_location;
	outer->stmt_len = q->stmt_len;
	memcpy(q, outer, sizeof(Query));
	return true;
}

/* Releases the aggregates of an aggregating level (the last step on that level). */
static void
asa_rewrite_release(AsaCtx *ctx, Expr *mask, bool top)
{
	AsaReleaseCtx rc = {ctx, mask};
	ListCell   *lc;

	if (asa_count_lanes(ctx, ctx->query->havingQual) > 0)
		ctx->query->havingQual = asa_release_having(ctx, ctx->query->havingQual, mask);
	if (top && asa_topk(ctx, mask))
		return;
	foreach(lc, ctx->query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		tle->expr = (Expr *) asa_release_mutator((Node *) tle->expr, &rc);
	}
}

/*
 * Counter arrays instead of releases: as_* (simd_asa.counters), or asa_counters_* in
 * predicate sub-selects (NULL over no rows, as DuckDB's decorrelated sub-selects).
 */
static void
asa_rewrite_counters(AsaCtx *ctx, Expr *mask, AsaAggForm form)
{
	Bitmapset  *rewritten = NULL;
	ListCell   *lc;

	if (asa_count_lanes(ctx, ctx->query->havingQual) > 0)
		ASA_UNSUPPORTED("aggregates in HAVING need a release (simd_asa.counters = off)");
	foreach(lc, ctx->query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		bool		lanes;

		if (asa_count_lanes(ctx, (Node *) tle->expr) > 0 && !asa_lane_able(ctx, (Node *) tle->expr))
			ASA_UNSUPPORTED("this expression over aggregates cannot be evaluated per world "
							"(supported: + - * / and casts)");
		tle->expr = (Expr *) asa_lanewise(ctx, (Node *) tle->expr, mask, form, &lanes);
		if (lanes)
			rewritten = bms_add_member(rewritten, tle->resno);
	}
	asa_retype_sortgroup(ctx->query->sortClause, ctx->query->targetList, rewritten, FLOAT4ARRAYOID);
	asa_retype_sortgroup(ctx->query->distinctClause, ctx->query->targetList, rewritten, FLOAT4ARRAYOID);
}

/* ---------------------------------------------------------------------------
 * Levels grouped by the entity
 * ------------------------------------------------------------------------- */

/* All equality conjuncts between two Vars of the level, outer-join conditions included. */
static void
asa_collect_all_eqs(Node *jtnode, List **eqs)
{
	ListCell   *lc;

	if (jtnode == NULL || IsA(jtnode, RangeTblRef))
		return;
	if (IsA(jtnode, FromExpr))
	{
		asa_add_eqs(((FromExpr *) jtnode)->quals, eqs);
		foreach(lc, ((FromExpr *) jtnode)->fromlist)
			asa_collect_all_eqs(lfirst(lc), eqs);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		asa_add_eqs(((JoinExpr *) jtnode)->quals, eqs);
		asa_collect_all_eqs(((JoinExpr *) jtnode)->larg, eqs);
		asa_collect_all_eqs(((JoinExpr *) jtnode)->rarg, eqs);
	}
}

/*
 * Whether column attno of rte determines its entity: the entity key, the relation's link
 * towards the entity, or a column a link references (unique). *is_key: it holds the key.
 */
static bool
asa_determines_entity(AsaMeta *meta, RangeTblEntry *rte, AttrNumber attno, bool *is_key)
{
	AsaLink    *link;
	ListCell   *lc;

	*is_key = false;
	if (rte->relid == meta->entity_relid)
		return *is_key = (attno == meta->entity_attnum);
	link = asa_link_towards_entity(meta, rte->relid, 0);
	if (!link)
		return false;
	if (link->attnum == attno)
	{
		*is_key = link->ref_relid == meta->entity_relid;
		return true;
	}
	foreach(lc, meta->links)
	{
		AsaLink    *other = (AsaLink *) lfirst(lc);

		if (other->ref_relid == rte->relid && other->ref_attnum == attno)
			return true;
	}
	return false;
}

static bool
asa_eq_between(List *eqs, int r1, AttrNumber a1, int r2, AttrNumber a2)
{
	ListCell   *lc;

	foreach(lc, eqs)
	{
		AsaEq	   *eq = (AsaEq *) lfirst(lc);

		if ((asa_var_is(eq->a, r1, a1) && asa_var_is(eq->b, r2, a2)) ||
			(asa_var_is(eq->b, r1, a1) && asa_var_is(eq->a, r2, a2)))
			return true;
	}
	return false;
}

/* Whether dependent relation r joins a tied one along a link (in either direction). */
static bool
asa_tied(AsaCtx *ctx, List *eqs, int r, Bitmapset *tied)
{
	RangeTblEntry *rte = rt_fetch(r, ctx->query->rtable);
	AsaLink    *lr = rte->relid == ctx->meta->entity_relid ? NULL : asa_link_towards_entity(ctx->meta, rte->relid, 0);
	int			t = -1;

	while ((t = bms_next_member(tied, t)) >= 0)
	{
		RangeTblEntry *trte = rt_fetch(t, ctx->query->rtable);
		AsaLink    *lt = trte->relid == ctx->meta->entity_relid ? NULL :
			asa_link_towards_entity(ctx->meta, trte->relid, 0);

		if (lr && lr->ref_relid == trte->relid && asa_eq_between(eqs, r, lr->attnum, t, lr->ref_attnum))
			return true;
		if (lt && lt->ref_relid == rte->relid && asa_eq_between(eqs, t, lt->attnum, r, lt->ref_attnum))
			return true;
	}
	return false;
}

/*
 * Whether an aggregating level groups by the entity: a GROUP BY column determines the
 * entity of a dependent relation, and every other dependent relation joins it along links,
 * so each group holds rows of a single entity and is the same in every world it exists in
 * (TPC-H Q13: GROUP BY c_custkey; Q18: GROUP BY l_orderkey). *key: the entity key, when the
 * column holds it.
 */
static bool
asa_per_entity(AsaCtx *ctx, Expr **key)
{
	Query	   *q = ctx->query;
	List	   *dependent = NIL;
	List	   *eqs = NIL;
	AsaSublinkCtx predicates = {ctx, NIL};
	ListCell   *lc;
	int			rtindex = 0;

	*key = NULL;
	if (q->groupClause == NIL || q->groupingSets)
		return false;
	foreach(lc, q->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		rtindex++;
		if (rte->rtekind == RTE_RELATION && rte->relkind != RELKIND_VIEW)
		{
			if (asa_is_dependent(ctx->meta, rte->relid))
				dependent = lappend_int(dependent, rtindex);
		}
		else if (asa_depends_walker((Node *) rte, ctx->stmt) ||
				 (rte->rtekind == RTE_SUBQUERY && asa_depends_query(rte->subquery, ctx->stmt)))
			return false;		/* rows from subqueries, CTEs or views */
	}
	if (dependent == NIL)
		return false;
	asa_predicate_sublink_walker(q->jointree->quals, &predicates);
	if (predicates.found)
		return false;			/* world-dependent groups */
	asa_collect_all_eqs((Node *) q->jointree, &eqs);
	foreach(lc, q->groupClause)
	{
		TargetEntry *tle = get_sortgroupclause_tle(lfirst(lc), q->targetList);
		Var		   *var = asa_strip_var((Node *) tle->expr);
		Bitmapset  *tied;
		bool		is_key;
		bool		changed = true;

		if (!var || !list_member_int(dependent, var->varno) ||
			!asa_determines_entity(ctx->meta, rt_fetch(var->varno, q->rtable), var->varattno, &is_key))
			continue;
		tied = bms_make_singleton(var->varno);
		while (changed)
		{
			ListCell   *lcd;

			changed = false;
			foreach(lcd, dependent)
				if (!bms_is_member(lfirst_int(lcd), tied) && asa_tied(ctx, eqs, lfirst_int(lcd), tied))
				{
					tied = bms_add_member(tied, lfirst_int(lcd));
					changed = true;
				}
		}
		if (bms_num_members(tied) != list_length(dependent))
			continue;
		*key = is_key ? (Expr *) copyObject(var) : NULL;
		return true;
	}
	return false;
}

/* ---------------------------------------------------------------------------
 * Levels
 * ------------------------------------------------------------------------- */

/* Adds mask as the last visible output column of query; returns its attno. */
static AttrNumber
asa_expose_mask(Query *query, Expr *mask)
{
	List	   *visible = NIL;
	List	   *junk = NIL;
	AttrNumber	attno;
	AttrNumber	resno;
	ListCell   *lc;

	foreach(lc, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (tle->resjunk)
			junk = lappend(junk, tle);
		else
			visible = lappend(visible, tle);
	}
	attno = list_length(visible) + 1;
	visible = lappend(visible, makeTargetEntry(mask, attno, pstrdup("asa_mask"), false));
	resno = attno;
	foreach(lc, junk)
		((TargetEntry *) lfirst(lc))->resno = ++resno;
	query->targetList = list_concat(visible, junk);
	return attno;
}

static void
asa_init_ctx(AsaCtx *ctx, AsaStmt *st, Query *query)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->stmt = st;
	ctx->meta = st->meta;
	ctx->query = query;
	ctx->pstate = make_parsestate(NULL);
	ctx->pstate->p_sourcetext = st->sourcetext;
	ctx->pstate->p_rtable = query->rtable;
	ctx->pstate->p_rteperminfos = query->rteperminfos;
	asa_mark_nullable((Node *) query->jointree, false, &ctx->nullable);
	asa_collect_eqs((Node *) query->jointree, false, &ctx->eqs);
	ctx->max_rtindex = list_length(query->rtable) + ASA_MAX_INJECTED_JOINS;
	ctx->keys = (Expr **) palloc0((ctx->max_rtindex + 1) * sizeof(Expr *));
}

/* Processes the CTEs of a level: aggregating ones are released in place (TPC-H Q15). */
static void
asa_level_ctes(AsaStmt *st, Query *query)
{
	ListCell   *lc;

	foreach(lc, query->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);
		Query	   *cte_query = (Query *) cte->ctequery;
		AsaCte	   *c = (AsaCte *) palloc0(sizeof(AsaCte));
		bool		depends = asa_depends_query(cte_query, st);

		c->cte = cte;
		if (depends && (cte->cterecursive || cte_query->commandType != CMD_SELECT))
			ASA_UNSUPPORTED("recursive or data-modifying CTEs over entity-dependent relations are not supported");
		if (depends)
			c->released = asa_level(st, cte_query, ASA_DERIVED, false, false).rewritten;
		c->dependent = depends && !c->released;
		st->ctes = lappend(st->ctes, c);
	}
}

/*
 * Processes the FROM-clause subqueries of a level; returns the masks of those passing one up.
 * below: the level's rows are aggregated (here or above), so they need masks.
 */
static List *
asa_level_children(AsaCtx *ctx, bool below, bool grouped)
{
	/* ctx->categorical: set when a child's mask holds predicates over worlds */
	List	   *masks = NIL;
	int			rtindex = 0;
	int			n = list_length(ctx->query->rtable);
	ListCell   *lc;

	foreach(lc, ctx->query->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (++rtindex > n)
			break;
		if (rte->rtekind == RTE_SUBQUERY && !list_member_ptr(ctx->stmt->done, rte->subquery))
		{
			AsaLevelResult r = asa_level(ctx->stmt, rte->subquery, ASA_DERIVED, below, grouped);

			if (r.mask_attno)
			{
				if (bms_is_member(rtindex, ctx->nullable))
					ASA_UNSUPPORTED("an entity-dependent subquery on the nullable side of an outer join is not supported");
				rte->eref->colnames = lappend(rte->eref->colnames, makeString(pstrdup("asa_mask")));
				masks = lappend(masks, makeVar(rtindex, r.mask_attno, INT8OID, -1, InvalidOid, 0));
				ctx->categorical |= r.categorical;
			}
		}
		else if (below && rte->rtekind == RTE_CTE)
		{
			AsaCte	   *cte = asa_find_cte(ctx->stmt, rte);

			if (cte && cte->dependent)
				ASA_UNSUPPORTED("CTE \"%s\" reads entity-dependent relations without aggregating them; "
								"write it as a subquery in FROM", rte->ctename);
		}
		else if (below && rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_VIEW &&
				 asa_depends_query(asa_view_query(rte->relid), ctx->stmt))
			ASA_UNSUPPORTED("views over entity-dependent relations are not supported; use a subquery or CTE");
	}
	return masks;
}

static bool
asa_sublinks_walker(Node *node, AsaCtx *ctx)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		Query	   *sub = (Query *) sublink->subselect;

		if (!list_member_ptr(ctx->stmt->done, sub))
			asa_level(ctx->stmt, sub, (sublink->subLinkType == EXPR_SUBLINK ||
									   sublink->subLinkType == ARRAY_SUBLINK) ? ASA_SCALAR : ASA_FILTER,
					  false, false);
		return asa_sublinks_walker(sublink->testexpr, ctx);
	}
	if (IsA(node, Query))
		return false;
	return expression_tree_walker(node, asa_sublinks_walker, ctx);
}

/* Processes the sub-selects in the expressions of a level (EXISTS / IN filters, scalars). */
static void
asa_level_sublinks(AsaCtx *ctx)
{
	asa_sublinks_walker((Node *) ctx->query->targetList, ctx);
	asa_sublinks_walker(ctx->query->havingQual, ctx);
	asa_sublinks_walker((Node *) ctx->query->jointree, ctx);
	asa_sublinks_walker(ctx->query->limitCount, ctx);
	asa_sublinks_walker(ctx->query->limitOffset, ctx);
}

/* Masks of the level's own entity-dependent relations (one per distinct entity key). */
static List *
asa_level_key_masks(AsaCtx *ctx)
{
	List	   *keys = NIL;
	List	   *masks = NIL;
	int			n = list_length(ctx->query->rtable);
	ListCell   *lc;

	for (int rtindex = 1; rtindex <= n; rtindex++)
	{
		RangeTblEntry *rte = rt_fetch(rtindex, ctx->query->rtable);
		Expr	   *key;

		if (rte->rtekind != RTE_RELATION || rte->relkind == RELKIND_VIEW || !asa_is_dependent(ctx->meta, rte->relid))
			continue;
		if (bms_is_member(rtindex, ctx->nullable))
			ASA_UNSUPPORTED("entity-dependent relation \"%s\" is on the nullable side of an outer join",
							get_rel_name(rte->relid));
		key = asa_entity_key(ctx, rtindex, 0);
		if (!list_member(keys, key))
			keys = lappend(keys, key);
	}
	foreach(lc, keys)
		masks = lappend(masks, asa_key_mask(ctx, (Expr *) lfirst(lc)));
	return masks;
}

/*
 * Rewrites one query level and, recursively, the levels below it.
 *   kind: TOP (the statement), DERIVED (FROM-clause subquery or CTE), PREDICATE (scalar
 *         sub-select of a predicate over worlds: returns world counters), SCALAR (other
 *         scalar sub-select: released), FILTER (EXISTS / IN / ANY / ALL sub-select).
 *   consumed: the level's rows are aggregated above, so it passes its row mask up.
 *   consumer_grouped: ... by a grouped aggregate.
 */
static AsaLevelResult
asa_level(AsaStmt *st, Query *query, AsaKind kind, bool consumed, bool consumer_grouped)
{
	AsaLevelResult result = {false, 0};
	AsaCtx		ctx;
	bool		aggregating = query->hasAggs || query->groupClause != NIL || query->groupingSets != NIL;
	bool		below;
	bool		grouped;
	List	   *predicates = NIL;
	List	   *masks;
	Expr	   *mask = NULL;
	Expr	   *key;
	ListCell   *lc;

	st->done = lappend(st->done, query);
	if (query->commandType != CMD_SELECT)
		return result;
	if (query->setOperations)
	{
		if (asa_depends_query(query, st))
			ASA_UNSUPPORTED("set operations over entity-dependent relations are not supported");
		return result;
	}
	asa_level_ctes(st, query);
	asa_init_ctx(&ctx, st, query);

	/* grouped by the entity: every group aggregates one entity's rows, exactly */
	if (aggregating && kind != ASA_TOP && kind != ASA_PREDICATE && asa_per_entity(&ctx, &key))
	{
		asa_level_children(&ctx, false, false);
		asa_level_sublinks(&ctx);
		if (consumed)
		{
			if (!key)
				ASA_UNSUPPORTED("a subquery aggregated further must group by the sampling-entity key itself");
			result.mask_attno = asa_expose_mask(query, asa_key_mask(&ctx, key));
		}
		return result;
	}

	below = aggregating || consumed;
	grouped = aggregating ? query->groupClause != NIL : consumer_grouped;
	masks = asa_level_children(&ctx, below, grouped);

	/* WHERE: predicates over worlds become masks (below aggregates) or filters */
	if (query->jointree->quals)
	{
		List	   *keep = NIL;

		foreach(lc, make_ands_implicit((Expr *) query->jointree->quals))
		{
			Expr	   *predicate = asa_world_predicate(&ctx, lfirst(lc));

			if (!predicate)
				keep = lappend(keep, lfirst(lc));
			else if (below)
				predicates = lappend(predicates, predicate);
			else if (st->counters)
				ASA_UNSUPPORTED("a predicate over worlds on rows that are not aggregated needs a release "
								"(simd_asa.counters = off)");
			else
				keep = lappend(keep, asa_filter_call(&ctx, predicate));
		}
		query->jointree->quals = keep ? (Node *) make_ands_explicit(keep) : NULL;
	}
	asa_level_sublinks(&ctx);
	if (!below)
		return result;			/* rows leave unaggregated */

	masks = list_concat(masks, asa_level_key_masks(&ctx));
	foreach(lc, masks)
		mask = asa_and_masks(&ctx, mask, (Expr *) lfirst(lc));
	foreach(lc, predicates)
		mask = asa_and_masks(&ctx, mask, (Expr *) lfirst(lc));
	if (!mask)
		return result;			/* entity-independent rows */
	ctx.categorical |= predicates != NIL;
	if (query->hasWindowFuncs)
		ASA_UNSUPPORTED("window functions over entity-dependent rows are not supported");
	if (query->groupingSets)
		ASA_UNSUPPORTED("GROUPING SETS, ROLLUP and CUBE are not supported");

	/* rows in no world do not form groups (DuckDB filters pac_pu <> 0) */
	if (predicates && grouped)
		query->jointree->quals = make_and_qual(query->jointree->quals,
											   (Node *) make_op(ctx.pstate, list_make2(makeString("pg_catalog"),
																					   makeString("<>")),
																(Node *) copyObject(mask),
																(Node *) makeConst(INT8OID, -1, InvalidOid,
																				   sizeof(int64), Int64GetDatum(0),
																				   false, FLOAT8PASSBYVAL),
																NULL, -1));
	if (!aggregating)
	{
		if (query->distinctClause)
			ASA_UNSUPPORTED("DISTINCT over entity-dependent rows below an aggregate is not supported");
		result.mask_attno = asa_expose_mask(query, mask);
		result.categorical = ctx.categorical;
		return result;
	}
	result.rewritten = true;
	if (kind == ASA_PREDICATE || st->counters)
	{
		if (kind != ASA_TOP && kind != ASA_PREDICATE)
			ASA_UNSUPPORTED("aggregates over entity-dependent rows in subqueries need a release "
							"(simd_asa.counters = off)");
		asa_rewrite_counters(&ctx, mask, kind == ASA_PREDICATE ? ASA_AGG_QCOUNTERS : ASA_AGG_COUNTERS);
	}
	else if (kind == ASA_FILTER)
		ASA_UNSUPPORTED("an IN / EXISTS sub-select may aggregate over entity-dependent rows only when grouped by "
						"the sampling entity (or a column that determines it)");
	else
		asa_rewrite_release(&ctx, mask, kind == ASA_TOP);
	return result;
}

/* ---------------------------------------------------------------------------
 * Statement
 * ------------------------------------------------------------------------- */

/*
 * True if the statement already uses this extension's functions: written by hand, or
 * rewritten before (EXPLAIN analyzes its query, then ExplainQuery calls the hook again).
 */
static bool
asa_uses_extension_walker(Node *node, Oid *extension)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref) && getExtensionOfObject(ProcedureRelationId, ((Aggref *) node)->aggfnoid) == *extension)
		return true;
	if (IsA(node, FuncExpr) && getExtensionOfObject(ProcedureRelationId, ((FuncExpr *) node)->funcid) == *extension)
		return true;
	if (IsA(node, Query))
		return query_tree_walker((Query *) node, asa_uses_extension_walker, extension, 0);
	return expression_tree_walker(node, asa_uses_extension_walker, extension);
}

/* User-defined functions may hide reads or computation that is not world-aware. */
static bool
asa_reject_udfs_walker(Node *node, Oid *extension)
{
	Oid function = InvalidOid;

	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr))
		function = ((FuncExpr *) node)->funcid;
	else if (IsA(node, Aggref))
		function = ((Aggref *) node)->aggfnoid;
	if (OidIsValid(function) && get_func_namespace(function) != PG_CATALOG_NAMESPACE &&
		getExtensionOfObject(ProcedureRelationId, function) != *extension)
		ASA_UNSUPPORTED("user-defined function or aggregate \"%s\" is not supported", get_func_name(function));
	if (IsA(node, Query))
		return query_tree_walker((Query *) node, asa_reject_udfs_walker, extension, 0);
	return expression_tree_walker(node, asa_reject_udfs_walker, extension);
}

static void
asa_rewrite_query(ParseState *pstate, Query *query)
{
	AsaStmt		st = {0};
	ListCell   *lc;
	bool		has_nested = false;
	Oid			extension = get_extension_oid("simd_asa", true);

	/* a query without aggregates matters only if a sub-query (or view) might aggregate */
	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		has_nested |= rte->rtekind == RTE_SUBQUERY || (rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_VIEW);
	}
	if (!query->hasAggs && !has_nested && !query->hasSubLinks && query->cteList == NIL)
		return;
	st.meta = asa_load_meta();
	if (!st.meta)
		return;
	asa_bind_ctes_walker((Node *) query, &st);
	st.counters = asa_counters_setting;
	st.sourcetext = pstate->p_sourcetext;
	if (!asa_depends_query(query, &st))
		return;
	asa_reject_udfs_walker((Node *) query, &extension);
	if (asa_uses_extension_walker((Node *) query, &extension))
		return;
	st.query_hash = asa_statement_query_hash(asa_noise_active() && !st.counters);
	asa_level(&st, query, ASA_TOP, false, false);
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
							 "Rewrite queries over the sampling entity into SIMD-ASA aggregates.",
							 "Aggregates then return PAC-noised values (see simd_asa.noise, simd_asa.counters).",
							 &asa_rewrite_enabled, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("simd_asa.counters",
							 "Rewritten aggregates return their 64 world counters (real[]) instead of a release.",
							 "Queries that need a release (HAVING, filters over worlds, aggregating subqueries) "
							 "are then rejected.",
							 &asa_counters_setting, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("simd_asa.pushdown_topk",
							 "ORDER BY ... LIMIT over aggregates ranks groups by the mean of their worlds and "
							 "releases only the selected ones.",
							 "pac_pushdown_topk.",
							 &asa_pushdown_topk, true, PGC_USERSET, 0, NULL, NULL, NULL);
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
