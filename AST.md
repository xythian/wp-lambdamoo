The AST extension
=================

`--enable-ast` adds three builtins that expose the server's own parser and
unparser, so MOO code can work on programs as data:

* `ast_parse(code [, options])` turns source into a tree.
* `ast_unparse(tree [, options])` turns a tree back into source.
* `ast_info()` describes every kind of node.

When it is built in, `"ast"` appears in `server_version("features")`.  The
extension needs no library, works with Unicode on or off, and changes
neither the database format nor how verbs are compiled.

This page is the specification of the tree format.  An in-db parser that
stands in for the builtins on servers without the extension must produce
exactly these trees.

Builtins
--------

### `ast_parse(LIST|STR code [, LIST options])` => `{tree, diagnostics}`

Parses `code` as `set_verb_code()` and `eval()` do.  `code` is a list of
lines, or a single string for a one-line program.  `tree` is
a `program` node, or 0 if the code does not compile.  `diagnostics` lists
the errors and warnings, each `{severity, line, message}` where `severity`
is `"error"` or `"warning"` and `message` is the text that follows
`Line N:  ` in `eval()`'s error strings.  The parser stops at the first
syntax error, so there is usually at most one error.

In the default strict mode a tree comes back exactly when
`set_verb_code()` would accept the code.  Stored verbs are read with
`ast_parse(verb_code(object, verb))`.

Options:

| Option       | Values                    | Default    |
|--------------|---------------------------|------------|
| `"builtins"` | `"strict"` or `"lenient"` | `"strict"` |

With `"strict"`, a call to a builtin the server lacks is an error, as in
`set_verb_code()`.  With `"lenient"` it is a warning and the call stays an
`expr_call` with its name, so tools can read code written for another
server.  Every other warning is a warning too in lenient mode.

Requires a programmer.  Raises `E_TYPE` if `code` is not a string or a
list of strings, and `E_INVARG` for a bad option.

### `ast_unparse(LIST tree [, LIST options])` => list of strings

Prints a `program` node as `verb_code()` prints a verb, one statement per
line.  It checks the whole tree against the format below first, and raises
`E_INVARG` if any node is malformed: an unknown kind, the wrong number of
fields, a field of the wrong type, a name that is not an identifier, or
nesting deeper than any parsed tree can be.  A tree whose printed form
would exceed the server's string limit (`max_string_concat`) raises
`E_QUOTA`; that covers a sublist shared many times over.

The output is not compiled.  The check is for shape only: a tree can be
well formed and still not compile, such as a `break` outside a loop.  A
tool saves a tree with `set_verb_code(o, v, ast_unparse(tree))`, which
reports any errors.

Options:

| Option     | Values | Default | Like                       |
|------------|--------|---------|----------------------------|
| `"parens"` | 0 or 1 | 0       | `verb_code()`'s argument 3 |
| `"indent"` | 0 or 1 | 1       | `verb_code()`'s argument 4 |

Requires a programmer.  Raises `E_INVARG` for a bad option.

For any verb,

    ast_unparse(ast_parse(verb_code(o, v))[1]) == verb_code(o, v)

and the same with the paren and indent options on both sides.

### `ast_info()` => `{format_version, kinds}`

`format_version` is 1 for the format on this page; any change to the kinds
or fields bumps it.  `kinds` has one entry per node kind this server's
parser can produce, each `{kind, class, fields, operator}`:

* `class` is `"program"`, `"stmt"`, `"expr"` or `"part"`.  Parts are the
  nodes that only appear inside others: `cond_arm`, `except_arm`, the
  `scat_` kinds and `arg_splice`.
* `fields` is a list of `{name, type}`, with the types below.
* `operator` is 0, or `{spelling, precedence, associativity}` for
  operators.  Higher precedence binds tighter, and associativity is
  `"left"`, `"right"`, `"none"` or `"prefix"`.

Without bitwise operators (`BITWISE_OPERATORS`), the bitwise kinds are
left out.  No permission is needed.

Tree format
-----------

Every node is a list `{kind, line, field, ...}`.  `kind` is a string,
`line` an integer, and each kind has a fixed number of fields in a fixed
order, so a node can be taken apart by scattering assignment:

    {kind, line, lhs, rhs} = node;

Kinds are the lowercased names of the enums in `ast.h` (`EXPR_PLUS` is
`"expr_plus"`) and fields the names of the struct fields; the helper
structs without an enum are named the same way.  `ast_unparse()` matches
kinds without regard to case, as MOO compares strings.

Field types, as `ast_info()` names them:

| Type          | Holds                                                     |
|---------------|-----------------------------------------------------------|
| `expr`        | an expression node                                        |
| `expr\|0`     | an expression node, or 0 when absent                      |
| `body`        | a list of statement nodes, possibly empty                 |
| `args`        | a list of expression and `arg_splice` nodes               |
| `codes`       | 0 for `ANY`, or a non-empty `args` list                   |
| `name`        | a variable, loop or builtin name as a string              |
| `name\|0`     | a name, or 0 when absent                                  |
| `value`       | an integer, float, string, object or error                |
| `cond_arms`   | a non-empty list of `cond_arm` nodes                      |
| `except_arms` | a non-empty list of `except_arm` nodes                    |
| `scatter`     | a non-empty list of `scat_` nodes                         |
| `0`           | always 0                                                  |

A name is an identifier as the parser reads one, and not a keyword.

### Statements

| Kind               | Fields                                   | Source                          |
|--------------------|------------------------------------------|---------------------------------|
| `program`          | body                                     | the root                        |
| `stmt_cond`        | arms (cond_arms), otherwise (body)       | `if ... elseif ... else ... endif` |
| `cond_arm`         | condition (expr), stmt (body)            | one `if` or `elseif` branch     |
| `stmt_list`        | id (name), expr, body                    | `for x in (expr)`               |
| `stmt_range`       | id (name), from, to (expr), body         | `for x in [from..to]`           |
| `stmt_while`       | id (name\|0), condition (expr), body     | `while [name] (condition)`      |
| `stmt_fork`        | id (name\|0), time (expr), body          | `fork [name] (time)`            |
| `stmt_expr`        | expr                                     | `expr;`, including string comments |
| `stmt_return`      | expr (expr\|0)                           | `return [expr];`                |
| `stmt_try_except`  | body, excepts (except_arms)              | `try ... except ... endtry`     |
| `except_arm`       | id (name\|0), codes, stmt (body)         | `except [id] (codes)`           |
| `stmt_try_finally` | body, handler (body)                     | `try ... finally ... endtry`    |
| `stmt_break`       | exit (name\|0)                           | `break [name];`                 |
| `stmt_continue`    | exit (name\|0)                           | `continue [name];`              |

An absent `else` and an empty one are the same: `otherwise` is `{}`.

### Expressions

| Kind            | Fields                                      | Source                       |
|-----------------|---------------------------------------------|------------------------------|
| `expr_var`      | var (value)                                 | a literal                    |
| `expr_id`       | id (name)                                   | a variable                   |
| `expr_prop`     | lhs, rhs (expr)                             | `o.p`, `o.(e)`, `$p`         |
| `expr_verb`     | obj, verb (expr), args                      | `o:v(...)`, `o:(e)(...)`, `$v(...)` |
| `expr_call`     | func (name), args                           | `f(...)`, a builtin call     |
| `expr_index`    | lhs, rhs (expr)                             | `x[i]`                       |
| `expr_range`    | base, from, to (expr)                       | `x[from..to]`                |
| `expr_length`   | none                                        | `$` inside an index          |
| `expr_asgn`     | lhs, rhs (expr)                             | `lhs = rhs`                  |
| `expr_scatter`  | scatter                                     | `{a, ?b = 1, @c}` on the left of `=` |
| `scat_required` | id (name), expr (0)                         | `a`                          |
| `scat_optional` | id (name), expr (expr\|0)                   | `?b`, `?b = default`         |
| `scat_rest`     | id (name), expr (0)                         | `@c`                         |
| `expr_list`     | list (args)                                 | `{...}`                      |
| `arg_splice`    | expr                                        | `@expr` in an argument list  |
| `expr_cond`     | condition, consequent, alternate (expr)     | `a ? b \| c`                 |
| `expr_catch`    | try (expr), codes, except (expr\|0)         | `` `try ! codes => except' `` |
| binary          | lhs, rhs (expr)                             | see below                    |
| unary           | expr                                        | `-x`, `!x`, `~x`             |

The binary kinds are `expr_plus`, `expr_minus`, `expr_times`,
`expr_divide`, `expr_mod`, `expr_exp`, `expr_eq`, `expr_ne`, `expr_lt`,
`expr_le`, `expr_gt`, `expr_ge`, `expr_in`, `expr_and`, `expr_or`, and with
bitwise operators `expr_bitor`, `expr_bitxor`, `expr_bitand`, `expr_shl`,
`expr_shr` and `expr_lshr`.  The unary kinds are `expr_negate`, `expr_not`
and, with bitwise operators, `expr_complement`.

The tree is the parser's, so it folds some spellings together:

* A property or verb name is an `expr_var` string: `o.p` and `o.("p")`
  are the same tree.
* `$p` is `expr_prop` on the object `#0`, and `$v(...)` is `expr_verb` on
  `#0`.  `ast_unparse()` prints both with `$` again.
* A negated number literal is the negative literal: `-5` is
  `{"expr_var", line, -5}`.
* Parentheses leave no trace; `ast_unparse()` adds them where precedence
  needs them.
* `/* ... */` comments are dropped.

Example: `if (x > 0) return $util:clamp(x, 10); endif` on line 1 is

    {"program", 0, {
      {"stmt_cond", 1,
        {{"cond_arm", 1, {"expr_gt", 1, {"expr_id", 1, "x"}, {"expr_var", 1, 0}},
          {{"stmt_return", 1,
            {"expr_verb", 1,
              {"expr_prop", 1, {"expr_var", 1, #0}, {"expr_var", 1, "util"}},
              {"expr_var", 1, "clamp"},
              {{"expr_id", 1, "x"}, {"expr_var", 1, 10}}}}}}},
        {}}}}

### Lines

`line` is the source line of the node's first token, counting the first
line of `code` as 1.  `cond_arm` and `except_arm` take the line of their
`if`, `elseif` or `except`.  The `program` node's line is 0.
`ast_unparse()` ignores lines.

`verb_code()` prints one statement per line, and that is the numbering
tracebacks use, so in `ast_parse(verb_code(o, v))` a statement's line is
the line a traceback names for it.

Columns are not recorded.
