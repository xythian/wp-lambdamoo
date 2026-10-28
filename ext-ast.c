/******************************************************************************
 * AST builtins: the server's parser and unparser, with trees as MOO lists.
 *
 *   ast_parse(LIST|STR code [, LIST options])  => {tree, diagnostics}
 *   ast_unparse(LIST tree [, LIST options])     => list of lines
 *   ast_info()                                  => {version, kinds}
 *
 * The tree format is specified in AST.md.  ast_parse() runs the parser
 * set_verb_code() uses and converts its Stmt tree to lists.  ast_unparse()
 * checks a list tree against the kind table below, builds a Stmt tree and
 * prints it with unparse.c.  The Stmt tree it builds is never compiled.
 *****************************************************************************/

#include "bf_register.h"

#include "my-string.h"

#include "ast.h"
#include "db.h"
#include "execute.h"
#include "functions.h"
#include "keywords.h"
#include "list.h"
#include "server.h"
#include "storage.h"
#include "structures.h"
#include "sym_table.h"
#include "unparse.h"
#include "utf-ctype.h"
#include "utf.h"
#include "utils.h"

/* Bump on any change to the kinds or fields below. */
#define AST_FORMAT_VERSION 1

/* Deepest list nesting ast_unparse() accepts.  The parser's stack holds
 * 10000 entries (bison's and byacc's default), and one entry can add two
 * levels, as each '{' of {{{x}}} does, so a parsed tree stays inside this.
 */
#define AST_MAX_DEPTH 20010

/*********** The kind table ***********/

enum field_type {
    F_EXPR,			/* an expression node */
    F_OPT_EXPR,			/* an expression node or 0 */
    F_BODY,			/* a list of statement nodes */
    F_ARGS,			/* a list of expression and arg_splice nodes */
    F_CODES,			/* 0 for ANY, or a non-empty F_ARGS */
    F_NAME,			/* an identifier */
    F_OPT_NAME,			/* an identifier or 0 */
    F_VALUE,			/* an integer, float, string, object or error */
    F_ARMS,			/* a non-empty list of cond_arm nodes */
    F_EXCEPTS,			/* a non-empty list of except_arm nodes */
    F_SCATTER,			/* a non-empty list of scat_* nodes */
    F_ZERO			/* always 0 */
};

static const char *field_type_names[] = {
    "expr", "expr|0", "body", "args", "codes", "name", "name|0",
    "value", "cond_arms", "except_arms", "scatter", "0"
};

enum kind_class {
    K_PROGRAM, K_STMT, K_EXPR, K_COND_ARM, K_EXCEPT_ARM, K_SCATTER, K_SPLICE
};

static const char *kind_class_names[] = {
    "program", "stmt", "expr", "part", "part", "part", "part"
};

struct field {
    const char *name;
    enum field_type type;
};

struct kind {
    const char *name;
    enum kind_class class;
    int code;			/* Stmt_Kind, Expr_Kind or Scatter_Kind */
    int nfields;
    struct field fields[4];
    /* For operators: */
    const char *spelling;
    int precedence;		/* higher binds tighter */
    const char *assoc;		/* "left", "right", "none" or "prefix" */
};

#define F1(a,t)			1, {{a, t}}
#define F2(a,t,b,u)		2, {{a, t}, {b, u}}
#define F3(a,t,b,u,c,v)		3, {{a, t}, {b, u}, {c, v}}
#define F4(a,t,b,u,c,v,d,w)	4, {{a, t}, {b, u}, {c, v}, {d, w}}
#define BINARY			F2("lhs", F_EXPR, "rhs", F_EXPR)
#define UNARY			F1("expr", F_EXPR)
#define NOT_OP			0, 0, 0

/* Precedences follow unparse.c's table. */
static const struct kind kinds[] = {
    {"program", K_PROGRAM, 0, F1("body", F_BODY), NOT_OP},

    {"stmt_cond", K_STMT, STMT_COND,
     F2("arms", F_ARMS, "otherwise", F_BODY), NOT_OP},
    {"cond_arm", K_COND_ARM, 0,
     F2("condition", F_EXPR, "stmt", F_BODY), NOT_OP},
    {"stmt_list", K_STMT, STMT_LIST,
     F3("id", F_NAME, "expr", F_EXPR, "body", F_BODY), NOT_OP},
    {"stmt_range", K_STMT, STMT_RANGE,
     F4("id", F_NAME, "from", F_EXPR, "to", F_EXPR, "body", F_BODY),
     NOT_OP},
    {"stmt_while", K_STMT, STMT_WHILE,
     F3("id", F_OPT_NAME, "condition", F_EXPR, "body", F_BODY), NOT_OP},
    {"stmt_fork", K_STMT, STMT_FORK,
     F3("id", F_OPT_NAME, "time", F_EXPR, "body", F_BODY), NOT_OP},
    {"stmt_expr", K_STMT, STMT_EXPR, F1("expr", F_EXPR), NOT_OP},
    {"stmt_return", K_STMT, STMT_RETURN, F1("expr", F_OPT_EXPR), NOT_OP},
    {"stmt_try_except", K_STMT, STMT_TRY_EXCEPT,
     F2("body", F_BODY, "excepts", F_EXCEPTS), NOT_OP},
    {"except_arm", K_EXCEPT_ARM, 0,
     F3("id", F_OPT_NAME, "codes", F_CODES, "stmt", F_BODY), NOT_OP},
    {"stmt_try_finally", K_STMT, STMT_TRY_FINALLY,
     F2("body", F_BODY, "handler", F_BODY), NOT_OP},
    {"stmt_break", K_STMT, STMT_BREAK, F1("exit", F_OPT_NAME), NOT_OP},
    {"stmt_continue", K_STMT, STMT_CONTINUE, F1("exit", F_OPT_NAME), NOT_OP},

    {"expr_var", K_EXPR, EXPR_VAR, F1("var", F_VALUE), NOT_OP},
    {"expr_id", K_EXPR, EXPR_ID, F1("id", F_NAME), NOT_OP},
    {"expr_prop", K_EXPR, EXPR_PROP, BINARY, NOT_OP},
    {"expr_verb", K_EXPR, EXPR_VERB,
     F3("obj", F_EXPR, "verb", F_EXPR, "args", F_ARGS), NOT_OP},
    {"expr_call", K_EXPR, EXPR_CALL,
     F2("func", F_NAME, "args", F_ARGS), NOT_OP},
    {"expr_index", K_EXPR, EXPR_INDEX, BINARY, NOT_OP},
    {"expr_range", K_EXPR, EXPR_RANGE,
     F3("base", F_EXPR, "from", F_EXPR, "to", F_EXPR), NOT_OP},
    {"expr_length", K_EXPR, EXPR_LENGTH, 0, {{0, 0}}, NOT_OP},
    {"expr_asgn", K_EXPR, EXPR_ASGN, BINARY, "=", 1, "right"},
    {"expr_scatter", K_EXPR, EXPR_SCATTER,
     F1("scatter", F_SCATTER), NOT_OP},
    {"scat_required", K_SCATTER, SCAT_REQUIRED,
     F2("id", F_NAME, "expr", F_ZERO), NOT_OP},
    {"scat_optional", K_SCATTER, SCAT_OPTIONAL,
     F2("id", F_NAME, "expr", F_OPT_EXPR), NOT_OP},
    {"scat_rest", K_SCATTER, SCAT_REST,
     F2("id", F_NAME, "expr", F_ZERO), NOT_OP},
    {"expr_list", K_EXPR, EXPR_LIST, F1("list", F_ARGS), NOT_OP},
    {"arg_splice", K_SPLICE, 0, F1("expr", F_EXPR), NOT_OP},
    {"expr_cond", K_EXPR, EXPR_COND,
     F3("condition", F_EXPR, "consequent", F_EXPR, "alternate", F_EXPR),
     "? |", 2, "none"},
    {"expr_catch", K_EXPR, EXPR_CATCH,
     F3("try", F_EXPR, "codes", F_CODES, "except", F_OPT_EXPR), NOT_OP},

    {"expr_or", K_EXPR, EXPR_OR, BINARY, "||", 3, "left"},
    {"expr_and", K_EXPR, EXPR_AND, BINARY, "&&", 3, "left"},
    {"expr_eq", K_EXPR, EXPR_EQ, BINARY, "==", 4, "left"},
    {"expr_ne", K_EXPR, EXPR_NE, BINARY, "!=", 4, "left"},
    {"expr_lt", K_EXPR, EXPR_LT, BINARY, "<", 4, "left"},
    {"expr_le", K_EXPR, EXPR_LE, BINARY, "<=", 4, "left"},
    {"expr_gt", K_EXPR, EXPR_GT, BINARY, ">", 4, "left"},
    {"expr_ge", K_EXPR, EXPR_GE, BINARY, ">=", 4, "left"},
    {"expr_in", K_EXPR, EXPR_IN, BINARY, "in", 4, "left"},
#if BITWISE_OPERATORS
    {"expr_bitor", K_EXPR, EXPR_BITOR, BINARY, ".|.", 5, "left"},
    {"expr_bitxor", K_EXPR, EXPR_BITXOR, BINARY, ".^.", 6, "left"},
    {"expr_bitand", K_EXPR, EXPR_BITAND, BINARY, ".&.", 7, "left"},
    {"expr_shl", K_EXPR, EXPR_SHL, BINARY, "<<", 8, "left"},
    {"expr_shr", K_EXPR, EXPR_SHR, BINARY, ">>", 8, "left"},
    {"expr_lshr", K_EXPR, EXPR_LSHR, BINARY, ">>>", 8, "left"},
#endif
    {"expr_plus", K_EXPR, EXPR_PLUS, BINARY, "+", 9, "left"},
    {"expr_minus", K_EXPR, EXPR_MINUS, BINARY, "-", 9, "left"},
    {"expr_times", K_EXPR, EXPR_TIMES, BINARY, "*", 10, "left"},
    {"expr_divide", K_EXPR, EXPR_DIVIDE, BINARY, "/", 10, "left"},
    {"expr_mod", K_EXPR, EXPR_MOD, BINARY, "%", 10, "left"},
    {"expr_exp", K_EXPR, EXPR_EXP, BINARY, "^", 11, "right"},
    {"expr_negate", K_EXPR, EXPR_NEGATE, UNARY, "-", 12, "prefix"},
    {"expr_not", K_EXPR, EXPR_NOT, UNARY, "!", 12, "prefix"},
#if BITWISE_OPERATORS
    {"expr_complement", K_EXPR, EXPR_COMPLEMENT, UNARY, "~", 12, "prefix"},
#endif
};

#define NUM_KINDS Arraysize(kinds)

static const struct kind *stmt_kinds[STMT_CONTINUE + 1];
static const struct kind *expr_kinds[SizeOf_Expr_Kind];
static const struct kind *program_kind, *cond_arm_kind, *except_arm_kind,
    *splice_kind, *scatter_kinds[SCAT_REST + 1];

static void
init_kinds(void)
{
    unsigned i;

    for (i = 0; i < NUM_KINDS; i++) {
	const struct kind *k = &kinds[i];

	switch (k->class) {
	case K_PROGRAM:
	    program_kind = k;
	    break;
	case K_STMT:
	    stmt_kinds[k->code] = k;
	    break;
	case K_EXPR:
	    expr_kinds[k->code] = k;
	    break;
	case K_COND_ARM:
	    cond_arm_kind = k;
	    break;
	case K_EXCEPT_ARM:
	    except_arm_kind = k;
	    break;
	case K_SCATTER:
	    scatter_kinds[k->code] = k;
	    break;
	case K_SPLICE:
	    splice_kind = k;
	    break;
	}
    }
}

static Var
str_var(const char *s)
{
    Var v;

    v.type = TYPE_STR;
    v.v.str = str_dup(s);
    return v;
}

static Var
int_var(Num n)
{
    Var v;

    v.type = TYPE_INT;
    v.v.num = n;
    return v;
}

/*********** Options ***********/

/* Options are an alist, or absent (any non-list).  Look up NAME in one
 * already checked by check_options(); returns 0 if absent.  The last
 * entry for a name wins.
 */
static Var *
find_option(Var options, const char *name)
{
    Var *found = 0;
    int i;

    if (options.type != TYPE_LIST)
	return 0;
    for (i = 1; i <= options.v.list[0].v.num; i++) {
	Var *entry = options.v.list[i].v.list;

	if (mystrcasecmp(entry[1].v.str, name) == 0)
	    found = &entry[2];
    }
    return found;
}

/* Options must be {{STR name, value}, ...} with names from KNOWN. */
static int
check_options(Var options, const char **known)
{
    int i;

    if (options.type != TYPE_LIST)
	return 1;
    for (i = 1; i <= options.v.list[0].v.num; i++) {
	Var entry = options.v.list[i];
	const char **k;

	if (entry.type != TYPE_LIST || entry.v.list[0].v.num != 2
	    || entry.v.list[1].type != TYPE_STR)
	    return 0;
	for (k = known; *k; k++)
	    if (mystrcasecmp(entry.v.list[1].v.str, *k) == 0)
		break;
	if (!*k)
	    return 0;
    }
    return 1;
}

/* An INT option, as a flag; returns 0 for a bad value. */
static int
flag_option(Var options, const char *name, int dflt, int *result)
{
    Var *v = find_option(options, name);

    if (!v) {
	*result = dflt;
	return 1;
    }
    if (v->type != TYPE_INT)
	return 0;
    *result = v->v.num != 0;
    return 1;
}

/*********** C tree to lists ***********/

static Names *tree_names;

static Var
node_var(const struct kind *k, int line)
{
    Var v = new_list(2 + k->nfields);

    v.v.list[1] = str_var(k->name);
    v.v.list[2] = int_var(line);
    return v;
}

static Var
name_var(int id)
{
    return id >= 0 ? str_var(tree_names->names[id]) : int_var(0);
}

static Var stmts_to_list(Stmt *);
static Var expr_to_list(Expr *);

static Var
args_to_list(Arg_List *args)
{
    Arg_List *a;
    int n = 0;
    Var v;

    for (a = args; a; a = a->next)
	n++;
    v = new_list(n);
    for (a = args, n = 1; a; a = a->next, n++) {
	if (a->kind == ARG_SPLICE) {
	    Var s = node_var(splice_kind, a->line);

	    s.v.list[3] = expr_to_list(a->expr);
	    v.v.list[n] = s;
	} else
	    v.v.list[n] = expr_to_list(a->expr);
    }
    return v;
}

static Var
codes_to_list(Arg_List *codes)
{
    return codes ? args_to_list(codes) : int_var(0);
}

static Var
opt_expr_to_list(Expr *e)
{
    return e ? expr_to_list(e) : int_var(0);
}

static Var
scatter_to_list(Scatter *scatter)
{
    Scatter *sc;
    int n = 0;
    Var v;

    for (sc = scatter; sc; sc = sc->next)
	n++;
    v = new_list(n);
    for (sc = scatter, n = 1; sc; sc = sc->next, n++) {
	Var item = node_var(scatter_kinds[sc->kind], sc->line);

	item.v.list[3] = name_var(sc->id);
	item.v.list[4] = opt_expr_to_list(sc->expr);
	v.v.list[n] = item;
    }
    return v;
}

static Var
expr_to_list(Expr *e)
{
    Var v = node_var(expr_kinds[e->kind], e->line);
    Var *f = v.v.list + 2;

    switch (e->kind) {
    case EXPR_VAR:
	f[1] = (e->e.var.type == TYPE_STR
		? str_var(e->e.var.v.str)
		: var_ref(e->e.var));
	break;
    case EXPR_ID:
	f[1] = name_var(e->e.id);
	break;
    case EXPR_VERB:
	f[1] = expr_to_list(e->e.verb.obj);
	f[2] = expr_to_list(e->e.verb.verb);
	f[3] = args_to_list(e->e.verb.args);
	break;
    case EXPR_CALL:
	if (e->e.call.func == FUNC_NOT_FOUND) {
	    /* lenient parse: the name is the first argument */
	    Arg_List *a = e->e.call.args;

	    f[1] = str_var(a->expr->e.var.v.str);
	    f[2] = args_to_list(a->next);
	} else {
	    f[1] = str_var(name_func_by_num(e->e.call.func));
	    f[2] = args_to_list(e->e.call.args);
	}
	break;
    case EXPR_RANGE:
	f[1] = expr_to_list(e->e.range.base);
	f[2] = expr_to_list(e->e.range.from);
	f[3] = expr_to_list(e->e.range.to);
	break;
    case EXPR_LENGTH:
	break;
    case EXPR_SCATTER:
	f[1] = scatter_to_list(e->e.scatter);
	break;
    case EXPR_LIST:
	f[1] = args_to_list(e->e.list);
	break;
    case EXPR_COND:
	f[1] = expr_to_list(e->e.cond.condition);
	f[2] = expr_to_list(e->e.cond.consequent);
	f[3] = expr_to_list(e->e.cond.alternate);
	break;
    case EXPR_CATCH:
	f[1] = expr_to_list(e->e.catch.try);
	f[2] = codes_to_list(e->e.catch.codes);
	f[3] = opt_expr_to_list(e->e.catch.except);
	break;
    case EXPR_NEGATE:
    case EXPR_NOT:
    case EXPR_COMPLEMENT:
	f[1] = expr_to_list(e->e.expr);
	break;
    default:			/* binary */
	f[1] = expr_to_list(e->e.bin.lhs);
	f[2] = expr_to_list(e->e.bin.rhs);
	break;
    }
    return v;
}

static Var
stmt_to_list(Stmt *s)
{
    Var v = node_var(stmt_kinds[s->kind], s->line);
    Var *f = v.v.list + 2;

    switch (s->kind) {
    case STMT_COND:
	{
	    Cond_Arm *arm;
	    int n = 0;

	    for (arm = s->s.cond.arms; arm; arm = arm->next)
		n++;
	    f[1] = new_list(n);
	    for (arm = s->s.cond.arms, n = 1; arm; arm = arm->next, n++) {
		Var a = node_var(cond_arm_kind, arm->line);

		a.v.list[3] = expr_to_list(arm->condition);
		a.v.list[4] = stmts_to_list(arm->stmt);
		f[1].v.list[n] = a;
	    }
	    f[2] = stmts_to_list(s->s.cond.otherwise);
	}
	break;
    case STMT_LIST:
	f[1] = name_var(s->s.list.id);
	f[2] = expr_to_list(s->s.list.expr);
	f[3] = stmts_to_list(s->s.list.body);
	break;
    case STMT_RANGE:
	f[1] = name_var(s->s.range.id);
	f[2] = expr_to_list(s->s.range.from);
	f[3] = expr_to_list(s->s.range.to);
	f[4] = stmts_to_list(s->s.range.body);
	break;
    case STMT_WHILE:
	f[1] = name_var(s->s.loop.id);
	f[2] = expr_to_list(s->s.loop.condition);
	f[3] = stmts_to_list(s->s.loop.body);
	break;
    case STMT_FORK:
	f[1] = name_var(s->s.fork.id);
	f[2] = expr_to_list(s->s.fork.time);
	f[3] = stmts_to_list(s->s.fork.body);
	break;
    case STMT_EXPR:
	f[1] = expr_to_list(s->s.expr);
	break;
    case STMT_RETURN:
	f[1] = opt_expr_to_list(s->s.expr);
	break;
    case STMT_TRY_EXCEPT:
	{
	    Except_Arm *ex;
	    int n = 0;

	    f[1] = stmts_to_list(s->s.catch.body);
	    for (ex = s->s.catch.excepts; ex; ex = ex->next)
		n++;
	    f[2] = new_list(n);
	    for (ex = s->s.catch.excepts, n = 1; ex; ex = ex->next, n++) {
		Var a = node_var(except_arm_kind, ex->line);

		a.v.list[3] = name_var(ex->id);
		a.v.list[4] = codes_to_list(ex->codes);
		a.v.list[5] = stmts_to_list(ex->stmt);
		f[2].v.list[n] = a;
	    }
	}
	break;
    case STMT_TRY_FINALLY:
	f[1] = stmts_to_list(s->s.finally.body);
	f[2] = stmts_to_list(s->s.finally.handler);
	break;
    case STMT_BREAK:
    case STMT_CONTINUE:
	f[1] = name_var(s->s.exit);
	break;
    }
    return v;
}

static Var
stmts_to_list(Stmt *stmts)
{
    Stmt *s;
    int n = 0;
    Var v;

    for (s = stmts; s; s = s->next)
	n++;
    v = new_list(n);
    for (s = stmts, n = 1; s; s = s->next, n++)
	v.v.list[n] = stmt_to_list(s);
    return v;
}

/*********** Lists to C tree ***********/

/* Walk the whole value without recursion before building anything, so
 * that the builder below can recurse safely: reject nesting deeper than
 * AST_MAX_DEPTH, and a tree whose printed form would be larger than a
 * string may be, counting a shared sublist once per use.
 */
enum measure { TREE_OK, TREE_TOO_DEEP, TREE_TOO_BIG, TREE_TIMED_OUT };

static enum measure
measure_tree(Var tree)
{
    struct frame {
	Var *items;
	int next, count;
    } *stack;
    int depth = 0, steps = 0;
    size_t bytes = 0;
    size_t limit = server_int_option_cached(SVO_MAX_STRING_CONCAT);
    enum measure m = TREE_OK;

    stack = mymalloc(sizeof(struct frame) * AST_MAX_DEPTH, M_AST);
    stack[0].items = tree.v.list;
    stack[0].next = 1;
    stack[0].count = tree.v.list[0].v.num;

    while (depth >= 0) {
	struct frame *top = &stack[depth];
	Var item;

	if (top->next > top->count) {
	    depth--;
	    continue;
	}
	item = top->items[top->next++];
	if (++steps % 4096 == 0 && task_timed_out) {
	    m = TREE_TIMED_OUT;
	    break;
	}
	bytes += 8;
	if (item.type == TYPE_STR)
	    bytes += memo_strlen(item.v.str);
	if (bytes > limit) {
	    m = TREE_TOO_BIG;
	    break;
	}
	if (item.type == TYPE_LIST) {
	    if (depth + 1 >= AST_MAX_DEPTH) {
		m = TREE_TOO_DEEP;
		break;
	    }
	    depth++;
	    stack[depth].items = item.v.list;
	    stack[depth].next = 1;
	    stack[depth].count = item.v.list[0].v.num;
	}
    }

    myfree(stack, M_AST);
    return m;
}

/* The slot table for the tree being built: one slot per name use. */
static const char **build_names;
static unsigned build_num_names, build_max_names;

static int
valid_name(const char *name)
{
    const char *p = name;
    uint32_t c = get_utf(&p);

    if (!(my_is_xid_start(c) || c == '_'))
	return 0;
    while (*p) {
	c = get_utf(&p);
	if (!(my_is_xid_cont(c) || c == '_'))
	    return 0;
    }
    return !find_keyword(name, strlen(name));
}

static int
add_name(const char *name)
{
    if (build_num_names == build_max_names) {
	unsigned new_max = build_max_names ? 2 * build_max_names : 16;
	const char **new_names = mymalloc(sizeof(char *) * new_max, M_NAMES);

	if (build_names) {
	    memcpy(new_names, build_names, sizeof(char *) * build_num_names);
	    myfree(build_names, M_NAMES);
	}
	build_names = new_names;
	build_max_names = new_max;
    }
    build_names[build_num_names] = name;
    return build_num_names++;
}

/* Each builder returns 0 for a malformed tree.  The nodes it allocates
 * are freed together by end_code_allocation().
 */

/* V is a node of a class in CLASSES (a bit set); sets *KIND. */
static Var *
check_node(Var v, unsigned classes, const struct kind **kind)
{
    unsigned i;

    if (v.type != TYPE_LIST || v.v.list[0].v.num < 2
	|| v.v.list[1].type != TYPE_STR || v.v.list[2].type != TYPE_INT)
	return 0;
    for (i = 0; i < NUM_KINDS; i++)
	if (mystrcasecmp(kinds[i].name, v.v.list[1].v.str) == 0)
	    break;
    if (i == NUM_KINDS || !(classes & (1u << kinds[i].class))
	|| v.v.list[0].v.num != 2 + kinds[i].nfields)
	return 0;
    *kind = &kinds[i];
    return v.v.list + 2;
}

static int
is_zero(Var v)
{
    return v.type == TYPE_INT && v.v.num == 0;
}

static int
build_name(Var v, int optional, int *id)
{
    if (optional && is_zero(v)) {
	*id = -1;
	return 1;
    }
    if (v.type != TYPE_STR || !valid_name(v.v.str))
	return 0;
    *id = add_name(v.v.str);
    return 1;
}

static Expr *build_expr(Var);
static int build_body(Var, Stmt **);

static int
build_args(Var v, Arg_List **result)
{
    Arg_List **tail = result;
    int i;

    *result = 0;
    if (v.type != TYPE_LIST)
	return 0;
    for (i = 1; i <= v.v.list[0].v.num; i++) {
	Var item = v.v.list[i];
	const struct kind *k;
	Var *f;
	Expr *e;
	enum Arg_Kind ak = ARG_NORMAL;

	if (!(f = check_node(item, 1u << K_EXPR | 1u << K_SPLICE, &k)))
	    return 0;
	if (k->class == K_SPLICE) {
	    ak = ARG_SPLICE;
	    e = build_expr(f[1]);
	} else
	    e = build_expr(item);
	if (!e)
	    return 0;
	*tail = alloc_arg_list(ak, e);
	tail = &(*tail)->next;
    }
    return 1;
}

static int
build_codes(Var v, Arg_List **result)
{
    if (is_zero(v)) {
	*result = 0;
	return 1;
    }
    return (v.type == TYPE_LIST && v.v.list[0].v.num > 0
	    && build_args(v, result));
}

static int
build_opt_expr(Var v, Expr **result)
{
    if (is_zero(v)) {
	*result = 0;
	return 1;
    }
    return (*result = build_expr(v)) != 0;
}

static Scatter *
build_scatter(Var v)
{
    Scatter *first = 0, **tail = &first;
    int i;

    if (v.type != TYPE_LIST || v.v.list[0].v.num == 0)
	return 0;
    for (i = 1; i <= v.v.list[0].v.num; i++) {
	const struct kind *k;
	Var *f = check_node(v.v.list[i], 1u << K_SCATTER, &k);
	Expr *dflt = 0;
	int id;

	if (!f || !build_name(f[1], 0, &id))
	    return 0;
	if (k->code == SCAT_OPTIONAL) {
	    if (!build_opt_expr(f[2], &dflt))
		return 0;
	} else if (!is_zero(f[2]))
	    return 0;
	*tail = alloc_scatter(k->code, id, dflt);
	tail = &(*tail)->next;
    }
    return first;
}

static int
valid_literal(Var v)
{
    switch (v.type) {
    case TYPE_INT:
    case TYPE_FLOAT:
    case TYPE_STR:
    case TYPE_OBJ:
    case TYPE_ERR:
	return 1;
    default:
	return 0;
    }
}

static Expr *
build_expr(Var v)
{
    const struct kind *k;
    Var *f = check_node(v, 1u << K_EXPR, &k);
    Expr *e;

    if (!f)
	return 0;
    e = alloc_expr(k->code);
    switch (e->kind) {
    case EXPR_VAR:
	if (!valid_literal(f[1]))
	    return 0;
	e->e.var = f[1];	/* borrowed: the tree outlives the Expr */
	break;
    case EXPR_ID:
	if (!build_name(f[1], 0, &e->e.id))
	    return 0;
	break;
    case EXPR_VERB:
	if (!(e->e.verb.obj = build_expr(f[1]))
	    || !(e->e.verb.verb = build_expr(f[2]))
	    || !build_args(f[3], &e->e.verb.args))
	    return 0;
	break;
    case EXPR_CALL:
	if (f[1].type != TYPE_STR || !valid_name(f[1].v.str)
	    || !build_args(f[2], &e->e.call.args))
	    return 0;
	e->e.call.func = number_func_by_name(f[1].v.str);
	if (e->e.call.func == FUNC_NOT_FOUND) {
	    /* As a lenient parse leaves it: the name goes first. */
	    Expr *name = alloc_var(TYPE_STR);
	    Arg_List *a = alloc_arg_list(ARG_NORMAL, name);

	    name->e.var.v.str = f[1].v.str;
	    a->next = e->e.call.args;
	    e->e.call.args = a;
	}
	break;
    case EXPR_RANGE:
	if (!(e->e.range.base = build_expr(f[1]))
	    || !(e->e.range.from = build_expr(f[2]))
	    || !(e->e.range.to = build_expr(f[3])))
	    return 0;
	break;
    case EXPR_LENGTH:
	break;
    case EXPR_SCATTER:
	if (!(e->e.scatter = build_scatter(f[1])))
	    return 0;
	break;
    case EXPR_LIST:
	if (!build_args(f[1], &e->e.list))
	    return 0;
	break;
    case EXPR_COND:
	if (!(e->e.cond.condition = build_expr(f[1]))
	    || !(e->e.cond.consequent = build_expr(f[2]))
	    || !(e->e.cond.alternate = build_expr(f[3])))
	    return 0;
	break;
    case EXPR_CATCH:
	if (!(e->e.catch.try = build_expr(f[1]))
	    || !build_codes(f[2], &e->e.catch.codes)
	    || !build_opt_expr(f[3], &e->e.catch.except))
	    return 0;
	break;
    case EXPR_NEGATE:
    case EXPR_NOT:
    case EXPR_COMPLEMENT:
	if (!(e->e.expr = build_expr(f[1])))
	    return 0;
	break;
    default:			/* binary */
	if (!(e->e.bin.lhs = build_expr(f[1]))
	    || !(e->e.bin.rhs = build_expr(f[2])))
	    return 0;
	break;
    }
    return e;
}

static Stmt *
build_stmt(Var v)
{
    const struct kind *k;
    Var *f = check_node(v, 1u << K_STMT, &k);
    Stmt *s;
    int i;

    if (!f)
	return 0;
    s = alloc_stmt(k->code);
    switch (s->kind) {
    case STMT_COND:
	{
	    Cond_Arm **tail = &s->s.cond.arms;

	    *tail = 0;
	    if (f[1].type != TYPE_LIST || f[1].v.list[0].v.num == 0)
		return 0;
	    for (i = 1; i <= f[1].v.list[0].v.num; i++) {
		const struct kind *ak;
		Var *af = check_node(f[1].v.list[i], 1u << K_COND_ARM, &ak);
		Expr *cond;
		Stmt *body;

		if (!af || !(cond = build_expr(af[1]))
		    || !build_body(af[2], &body))
		    return 0;
		*tail = alloc_cond_arm(cond, body);
		tail = &(*tail)->next;
	    }
	    if (!build_body(f[2], &s->s.cond.otherwise))
		return 0;
	}
	break;
    case STMT_LIST:
	if (!build_name(f[1], 0, &s->s.list.id)
	    || !(s->s.list.expr = build_expr(f[2]))
	    || !build_body(f[3], &s->s.list.body))
	    return 0;
	break;
    case STMT_RANGE:
	if (!build_name(f[1], 0, &s->s.range.id)
	    || !(s->s.range.from = build_expr(f[2]))
	    || !(s->s.range.to = build_expr(f[3]))
	    || !build_body(f[4], &s->s.range.body))
	    return 0;
	break;
    case STMT_WHILE:
	if (!build_name(f[1], 1, &s->s.loop.id)
	    || !(s->s.loop.condition = build_expr(f[2]))
	    || !build_body(f[3], &s->s.loop.body))
	    return 0;
	break;
    case STMT_FORK:
	if (!build_name(f[1], 1, &s->s.fork.id)
	    || !(s->s.fork.time = build_expr(f[2]))
	    || !build_body(f[3], &s->s.fork.body))
	    return 0;
	break;
    case STMT_EXPR:
	if (!(s->s.expr = build_expr(f[1])))
	    return 0;
	break;
    case STMT_RETURN:
	if (!build_opt_expr(f[1], &s->s.expr))
	    return 0;
	break;
    case STMT_TRY_EXCEPT:
	{
	    Except_Arm **tail = &s->s.catch.excepts;

	    *tail = 0;
	    if (!build_body(f[1], &s->s.catch.body)
		|| f[2].type != TYPE_LIST || f[2].v.list[0].v.num == 0)
		return 0;
	    for (i = 1; i <= f[2].v.list[0].v.num; i++) {
		const struct kind *ak;
		Var *af = check_node(f[2].v.list[i], 1u << K_EXCEPT_ARM, &ak);
		Arg_List *codes;
		Stmt *body;
		int id;

		if (!af || !build_name(af[1], 1, &id)
		    || !build_codes(af[2], &codes)
		    || !build_body(af[3], &body))
		    return 0;
		*tail = alloc_except(id, codes, body);
		tail = &(*tail)->next;
	    }
	}
	break;
    case STMT_TRY_FINALLY:
	if (!build_body(f[1], &s->s.finally.body)
	    || !build_body(f[2], &s->s.finally.handler))
	    return 0;
	break;
    case STMT_BREAK:
    case STMT_CONTINUE:
	if (!build_name(f[1], 1, &s->s.exit))
	    return 0;
	break;
    }
    return s;
}

static int
build_body(Var v, Stmt **result)
{
    Stmt **tail = result;
    int i;

    *result = 0;
    if (v.type != TYPE_LIST)
	return 0;
    for (i = 1; i <= v.v.list[0].v.num; i++) {
	if (!(*tail = build_stmt(v.v.list[i])))
	    return 0;
	tail = &(*tail)->next;
    }
    return 1;
}

/*********** Builtins ***********/

static const char *parse_options[] = {"builtins", 0};
static const char *unparse_options[] = {"parens", "indent", 0};

static package
bf_ast_parse(Var arglist, Byte next UNUSED_, void *vdata UNUSED_, Objid progr)
{
    Var code = arglist.v.list[1];
    Var options = (arglist.v.list[0].v.num > 1
		   ? arglist.v.list[2] : int_var(0));
    Var *builtins, diags, r;
    int lenient = 0, i, ok;
    Stmt *tree;
    Names *names;

    if (!is_programmer(progr)) {
	free_var(arglist);
	return make_error_pack(E_PERM);
    }
    if (code.type == TYPE_LIST) {
	for (i = 1; i <= code.v.list[0].v.num; i++)
	    if (code.v.list[i].type != TYPE_STR) {
		free_var(arglist);
		return make_error_pack(E_TYPE);
	    }
    } else if (code.type != TYPE_STR) {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    }
    if (!check_options(options, parse_options)) {
	free_var(arglist);
	return make_error_pack(E_INVARG);
    }
    if ((builtins = find_option(options, "builtins"))) {
	if (builtins->type == TYPE_STR
	    && mystrcasecmp(builtins->v.str, "lenient") == 0)
	    lenient = 1;
	else if (builtins->type != TYPE_STR
		 || mystrcasecmp(builtins->v.str, "strict") != 0) {
	    free_var(arglist);
	    return make_error_pack(E_INVARG);
	}
    }

    if (code.type == TYPE_STR) {
	Var line = var_ref(code);

	code = new_list(1);
	code.v.list[1] = line;
    } else
	code = var_ref(code);
    free_var(arglist);
    ok = parse_list_as_tree(code, lenient, &diags, &tree, &names);
    free_var(code);
    if (task_timed_out) {
	if (ok) {
	    free_stmt(tree);
	    free_names(names);
	}
	free_var(diags);
	return make_abort_pack(ABORT_SECONDS);
    }

    r = new_list(2);
    if (ok) {
	Var root = node_var(program_kind, 0);

	tree_names = names;
	root.v.list[3] = stmts_to_list(tree);
	tree_names = 0;
	free_stmt(tree);
	free_names(names);
	r.v.list[1] = root;
    } else
	r.v.list[1] = int_var(0);
    r.v.list[2] = diags;
    return make_var_pack(r);
}

static void
collect_line(void *data, const char *line)
{
    Var *lines = data;

    *lines = listappend(*lines, str_var(line));
}

static package
bf_ast_unparse(Var arglist, Byte next UNUSED_, void *vdata UNUSED_, Objid progr)
{
    Var tree = arglist.v.list[1];
    Var options = (arglist.v.list[0].v.num > 1
		   ? arglist.v.list[2] : int_var(0));
    int parens, indent;
    const struct kind *k;
    Var *f, lines;
    Stmt *body;
    enum measure m;

    if (!is_programmer(progr)) {
	free_var(arglist);
	return make_error_pack(E_PERM);
    }
    if (!check_options(options, unparse_options)
	|| !flag_option(options, "parens", 0, &parens)
	|| !flag_option(options, "indent", 1, &indent)) {
	free_var(arglist);
	return make_error_pack(E_INVARG);
    }
    if ((m = measure_tree(tree)) != TREE_OK) {
	free_var(arglist);
	if (m == TREE_TOO_BIG)
	    return make_space_pack();
	if (m == TREE_TIMED_OUT)
	    return make_abort_pack(ABORT_SECONDS);
	return make_error_pack(E_INVARG);
    }

    build_names = 0;
    build_num_names = build_max_names = 0;
    begin_code_allocation();
    f = check_node(tree, 1u << K_PROGRAM, &k);
    if (!f || !build_body(f[1], &body)) {
	end_code_allocation(1);
	if (build_names)
	    myfree(build_names, M_NAMES);
	free_var(arglist);
	return make_error_pack(E_INVARG);
    }

    lines = new_list(0);
    unparse_tree(body, build_names, build_num_names, collect_line, &lines,
		 parens, indent);
    end_code_allocation(1);
    if (build_names)
	myfree(build_names, M_NAMES);
    free_var(arglist);
    return make_var_pack(lines);
}

static Var
field_list(const struct kind *k)
{
    Var fields = new_list(k->nfields);
    int i;

    for (i = 0; i < k->nfields; i++) {
	Var f = new_list(2);

	f.v.list[1] = str_var(k->fields[i].name);
	f.v.list[2] = str_var(field_type_names[k->fields[i].type]);
	fields.v.list[i + 1] = f;
    }
    return fields;
}

static package
bf_ast_info(Var arglist, Byte next UNUSED_, void *vdata UNUSED_,
	    Objid progr UNUSED_)
{
    Var r = new_list(2), list = new_list(NUM_KINDS);
    unsigned i;

    free_var(arglist);
    for (i = 0; i < NUM_KINDS; i++) {
	const struct kind *k = &kinds[i];
	Var entry = new_list(4);

	entry.v.list[1] = str_var(k->name);
	entry.v.list[2] = str_var(kind_class_names[k->class]);
	entry.v.list[3] = field_list(k);
	if (k->spelling) {
	    Var op = new_list(3);

	    op.v.list[1] = str_var(k->spelling);
	    op.v.list[2] = int_var(k->precedence);
	    op.v.list[3] = str_var(k->assoc);
	    entry.v.list[4] = op;
	} else
	    entry.v.list[4] = int_var(0);
	list.v.list[i + 1] = entry;
    }
    r.v.list[1] = int_var(AST_FORMAT_VERSION);
    r.v.list[2] = list;
    return make_var_pack(r);
}

void
register_ast(void)
{
    init_kinds();
    register_function("ast_parse", 1, 2, bf_ast_parse, TYPE_ANY, TYPE_LIST);
    register_function("ast_unparse", 1, 2, bf_ast_unparse,
		      TYPE_LIST, TYPE_LIST);
    register_function("ast_info", 0, 0, bf_ast_info);
}
