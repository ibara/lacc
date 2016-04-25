#include "expression.h"
#include "declaration.h"
#include "eval.h"
#include "parse.h"
#include "symtab.h"
#include "type.h"
#include <lacc/token.h>
#include <lacc/cli.h>

#include <assert.h>

#define FIRST_type_qualifier \
    CONST: case VOLATILE

#define FIRST_type_specifier \
    VOID: case CHAR: case SHORT: case INT: case LONG: case FLOAT: case DOUBLE: \
    case SIGNED: case UNSIGNED: case STRUCT: case UNION: case ENUM

#define FIRST_type_name \
    FIRST_type_qualifier: \
    case FIRST_type_specifier

#define FIRST(s) FIRST_ ## s

static struct block *cast_expression(
    struct definition *def,
    struct block *block);

/* Parse call to builtin symbol __builtin_va_start, which is the result
 * of calling va_start(arg, s). Return type depends on second input
 * argument.
 */
static struct block *parse__builtin_va_start(
    struct definition *def,
    struct block *block)
{
    const struct typetree *type;
    struct symbol *sym;
    struct token param;
    int is_invalid;

    consume('(');
    block = assignment_expression(def, block);
    consume(',');
    param = consume(IDENTIFIER);
    sym = sym_lookup(&ns_ident, param.d.string.str);

    type = &def->symbol->type;
    is_invalid = !sym || sym->depth != 1 || !is_function(type);
    is_invalid = is_invalid || !nmembers(type) || strcmp(
        get_member(type, nmembers(type) - 1)->name, param.d.string.str);

    if (is_invalid) {
        error("Second parameter of va_start must be last function argument.");
        exit(1);
    }

    consume(')');
    block->expr = eval__builtin_va_start(block, block->expr);
    return block;
}

/* Parse call to builtin symbol __builtin_va_arg, which is the result of
 * calling va_arg(arg, T). Return type depends on second input argument.
 */
static struct block *parse__builtin_va_arg(
    struct definition *def,
    struct block *block)
{
    struct typetree *type;

    consume('(');
    block = assignment_expression(def, block);
    consume(',');
    type = declaration_specifiers(NULL);
    if (peek().token != ')') {
        type = declarator(type, NULL);
    }
    consume(')');
    block->expr = eval__builtin_va_arg(def, block, block->expr, type);
    return block;
}

static struct block *primary_expression(
    struct definition *def,
    struct block *block)
{
    const struct symbol *sym;
    struct token tok;

    switch ((tok = next()).token) {
    case IDENTIFIER:
        sym = sym_lookup(&ns_ident, tok.d.string.str);
        if (!sym) {
            error("Undefined symbol '%s'.", tok.d.string.str);
            exit(1);
        }
        /* Special handling for builtin pseudo functions. These are
         * expected to behave as macros, thus should be no problem
         * parsing as function call in primary expression. Constructs
         * like (va_arg)(args, int) will not work with this scheme. */
        if (!strcmp("__builtin_va_start", sym->name)) {
            block = parse__builtin_va_start(def, block);
        } else if (!strcmp("__builtin_va_arg", sym->name)) {
            block = parse__builtin_va_arg(def, block);
        } else {
            block->expr = var_direct(sym);
        }
        break;
    case NUMBER:
        block->expr = var_numeric(tok.d.number);
        break;
    case '(':
        block = expression(def, block);
        consume(')');
        break;
    case STRING:
        sym =
            sym_add(&ns_ident,
                ".LC",
                type_init(T_ARRAY, &basic_type__char, tok.d.string.len + 1),
                SYM_STRING_VALUE,
                LINK_INTERN);

        /* Store string value directly on symbol, memory ownership is in
         * string table from previously called str_register. The symbol
         * now exists as if declared static char .LC[] = "...". */
        ((struct symbol *) sym)->string_value = tok.d.string;

        /* Result is an IMMEDIATE of type [] char, with a reference to
         * the new symbol containing the string literal. Will decay into
         * char * on evaluation. */
        block->expr = var_direct(sym);
        assert(block->expr.kind == IMMEDIATE);
        break;
    default:
        error("Unexpected '%s', not a valid primary expression.",
            tok.d.string.str);
        exit(1);
    }

    return block;
}

static struct block *postfix_expression(
    struct definition *def,
    struct block *block)
{
    struct var root;

    block = primary_expression(def, block);
    root = block->expr;

    while (1) {
        const struct member *mbr;
        const struct typetree *type;
        struct var expr, copy, *arg;
        struct token tok;
        int i, j;

        switch ((tok = peek()).token) {
        case '[':
            do {
                /* Evaluate a[b] = *(a + b). The semantics of pointer
                 * arithmetic takes care of multiplying b with the
                 * correct width. */
                consume('[');
                block = expression(def, block);
                root = eval_expr(def, block, IR_OP_ADD, root, block->expr);
                root = eval_deref(def, block, root);
                consume(']');
            } while (peek().token == '[');
            break;
        case '(':
            type = root.type;
            if (is_pointer(root.type) && is_function(root.type->next))
                type = type_deref(root.type);
            else if (!is_function(root.type)) {
                error("Expression must have type pointer to function, was %t.",
                    root.type);
                exit(1);
            }
            consume('(');
            arg = calloc(nmembers(type), sizeof(*arg));
            for (i = 0; i < nmembers(type); ++i) {
                if (peek().token == ')') {
                    error("Too few arguments, expected %d but got %d.",
                        nmembers(type), i);
                    exit(1);
                }
                block = assignment_expression(def, block);
                arg[i] = block->expr;
                /* todo: type check here. */
                if (i < nmembers(type) - 1) {
                    consume(',');
                }
            }
            while (is_vararg(type) && peek().token != ')') {
                consume(',');
                arg = realloc(arg, (i + 1) * sizeof(*arg));
                block = assignment_expression(def, block);
                arg[i] = block->expr;
                i++;
            }
            consume(')');
            for (j = 0; j < i; ++j)
                param(def, block, arg[j]);
            free(arg);
            root = eval_call(def, block, root);
            break;
        case '.':
            consume('.');
            tok = consume(IDENTIFIER);
            mbr = find_type_member(root.type, tok.d.string.str);
            if (!mbr) {
                error("Invalid access, no member named '%s'.",
                    tok.d.string.str);
                exit(1);
            }
            root.type = mbr->type;
            root.width = mbr->width;
            root.offset += mbr->offset;
            break;
        case ARROW:
            consume(ARROW);
            tok = consume(IDENTIFIER);
            if (is_pointer(root.type) && is_struct_or_union(root.type->next)) {
                mbr = find_type_member(type_deref(root.type), tok.d.string.str);
                if (!mbr) {
                    error("Invalid access, no member named '%s'.",
                        tok.d.string.str);
                    exit(1);
                }

                /* Make it look like a pointer to the member type, then
                 * perform normal dereferencing. */
                root.type = type_init(T_POINTER, mbr->type);
                root = eval_deref(def, block, root);
                root.offset = mbr->offset;
                root.width = mbr->width;
            } else {
                error("Invalid member access.");
                exit(1);
            }
            break;
        case INCREMENT:
            consume(INCREMENT);
            copy = eval_copy(def, block, root);
            expr = eval_expr(def, block, IR_OP_ADD, root, var_int(1));
            eval_assign(def, block, root, expr);
            root = copy;
            break;
        case DECREMENT:
            consume(DECREMENT);
            copy = eval_copy(def, block, root);
            expr = eval_expr(def, block, IR_OP_SUB, root, var_int(1));
            eval_assign(def, block, root, expr);
            root = copy;
            break;
        default:
            block->expr = root;
            return block;
        }
    }
}

static struct block *unary_expression(
    struct definition *def,
    struct block *block)
{
    struct var value;

    switch (peek().token) {
    case '&':
        consume('&');
        block = cast_expression(def, block);
        block->expr = eval_addr(def, block, block->expr);
        break;
    case '*':
        consume('*');
        block = cast_expression(def, block);
        block->expr = eval_deref(def, block, block->expr);
        break;
    case '!':
        consume('!');
        block = cast_expression(def, block);
        block->expr = eval_expr(def, block, IR_OP_EQ, var_int(0), block->expr);
        break;
    case '~':
        consume('~');
        block = cast_expression(def, block);
        block->expr = eval_expr(def, block, IR_NOT, block->expr);
        break;
    case '+':
        consume('+');
        block = cast_expression(def, block);
        block->expr.lvalue = 0;
        break;
    case '-':
        consume('-');
        block = cast_expression(def, block);
        block->expr = eval_expr(def, block, IR_OP_SUB, var_int(0), block->expr);
        break;
    case SIZEOF: {
        struct typetree *type;
        struct block *head = cfg_block_init(def), *tail;
        consume(SIZEOF);
        if (peek().token == '(') {
            switch (peekn(2).token) {
            case FIRST(type_name):
                consume('(');
                type = declaration_specifiers(NULL);
                if (peek().token != ')') {
                    type = declarator(type, NULL);
                }
                consume(')');
                break;
            default:
                tail = unary_expression(def, head);
                type = (struct typetree *) tail->expr.type;
                break;
            }
        } else {
            tail = unary_expression(def, head);
            type = (struct typetree *) tail->expr.type;
        }
        if (is_function(type)) {
            error("Cannot apply 'sizeof' to function type.");
        }
        if (!size_of(type)) {
            error("Cannot apply 'sizeof' to incomplete type.");
        }
        block->expr = var_int(size_of(type));
        break;
    }
    case INCREMENT:
        consume(INCREMENT);
        block = unary_expression(def, block);
        value = block->expr;
        block->expr = eval_expr(def, block, IR_OP_ADD, value, var_int(1));
        block->expr = eval_assign(def, block, value, block->expr);
        break;
    case DECREMENT:
        consume(DECREMENT);
        block = unary_expression(def, block);
        value = block->expr;
        block->expr = eval_expr(def, block, IR_OP_SUB, value, var_int(1));
        block->expr = eval_assign(def, block, value, block->expr);
        break;
    default:
        block = postfix_expression(def, block);
        break;
    }

    return block;
}

static struct block *cast_expression(
    struct definition *def,
    struct block *block)
{
    struct typetree *type;
    struct token tok;
    struct symbol *sym;

    /* This rule needs two lookahead; to see beyond the initial
     * parenthesis if it is actually a cast or an expression. */
    if (peek().token == '(') {
        tok = peekn(2);
        switch (tok.token) {
        case IDENTIFIER:
            sym = sym_lookup(&ns_ident, tok.d.string.str);
            if (!sym || sym->symtype != SYM_TYPEDEF)
                break;
        case FIRST(type_name):
            consume('(');
            type = declaration_specifiers(NULL);
            if (peek().token != ')') {
                type = declarator(type, NULL);
            }
            consume(')');
            block = cast_expression(def, block);
            block->expr = eval_cast(def, block, block->expr, type);
            return block;
        default:
            break;
        }
    }

    return unary_expression(def, block);
}

static struct block *multiplicative_expression(
    struct definition *def,
    struct block *block)
{
    struct var value;

    block = cast_expression(def, block);
    while (1) {
        value = block->expr;
        if (peek().token == '*') {
            consume('*');
            block = cast_expression(def, block);
            block->expr = eval_expr(def, block, IR_OP_MUL, value, block->expr);
        } else if (peek().token == '/') {
            consume('/');
            block = cast_expression(def, block);
            block->expr = eval_expr(def, block, IR_OP_DIV, value, block->expr);
        } else if (peek().token == '%') {
            consume('%');
            block = cast_expression(def, block);
            block->expr = eval_expr(def, block, IR_OP_MOD, value, block->expr);
        } else break;
    }

    return block;
}

static struct block *additive_expression(
    struct definition *def,
    struct block *block)
{
    struct var value;

    block = multiplicative_expression(def, block);
    while (1) {
        value = block->expr;
        if (peek().token == '+') {
            consume('+');
            block = multiplicative_expression(def, block);
            block->expr = eval_expr(def, block, IR_OP_ADD, value, block->expr);
        } else if (peek().token == '-') {
            consume('-');
            block = multiplicative_expression(def, block);
            block->expr = eval_expr(def, block, IR_OP_SUB, value, block->expr);
        } else break;
    }

    return block;
}

static struct block *shift_expression(
    struct definition *def,
    struct block *block)
{
    struct var value;

    block = additive_expression(def, block);
    while (1) {
        value = block->expr;
        if (peek().token == LSHIFT) {
            consume(LSHIFT);
            block = additive_expression(def, block);
            block->expr = eval_expr(def, block, IR_OP_SHL, value, block->expr);
        } else if (peek().token == RSHIFT) {
            consume(RSHIFT);
            block = additive_expression(def, block);
            block->expr = eval_expr(def, block, IR_OP_SHR, value, block->expr);
        } else break;
    }

    return block;
}

static struct block *relational_expression(
    struct definition *def,
    struct block *block)
{
    struct var value;

    block = shift_expression(def, block);
    while (1) {
        value = block->expr;
        switch (peek().token) {
        case '<':
            consume('<');
            block = shift_expression(def, block);
            block->expr = eval_expr(def, block, IR_OP_GT, block->expr, value);
            break;
        case '>':
            consume('>');
            block = shift_expression(def, block);
            block->expr = eval_expr(def, block, IR_OP_GT, value, block->expr);
            break;
        case LEQ:
            consume(LEQ);
            block = shift_expression(def, block);
            block->expr = eval_expr(def, block, IR_OP_GE, block->expr, value);
            break;
        case GEQ:
            consume(GEQ);
            block = shift_expression(def, block);
            block->expr = eval_expr(def, block, IR_OP_GE, value, block->expr);
            break;
        default:
            return block;
        }
    }
}

static struct block *equality_expression(
    struct definition *def,
    struct block *block)
{
    struct var value;

    block = relational_expression(def, block);
    while (1) {
        value = block->expr;
        if (peek().token == EQ) {
            consume(EQ);
            block = relational_expression(def, block);
            block->expr = eval_expr(def, block, IR_OP_EQ, value, block->expr);
        } else if (peek().token == NEQ) {
            consume(NEQ);
            block = relational_expression(def, block);
            block->expr = 
                eval_expr(def, block, IR_OP_EQ, var_int(0),
                    eval_expr(def, block, IR_OP_EQ, value, block->expr));
        } else break;
    }

    return block;
}

static struct block *and_expression(
    struct definition *def,
    struct block *block)
{
    struct var value;

    block = equality_expression(def, block);
    while (peek().token == '&') {
        consume('&');
        value = block->expr;
        block = equality_expression(def, block);
        block->expr = eval_expr(def, block, IR_OP_AND, value, block->expr);
    }

    return block;
}

static struct block *exclusive_or_expression(
    struct definition *def,
    struct block *block)
{
    struct var value;

    block = and_expression(def, block);
    while (peek().token == '^') {
        consume('^');
        value = block->expr;
        block = and_expression(def, block);
        block->expr = eval_expr(def, block, IR_OP_XOR, value, block->expr);
    }

    return block;
}

static struct block *inclusive_or_expression(
    struct definition *def,
    struct block *block)
{
    struct var value;

    block = exclusive_or_expression(def, block);
    while (peek().token == '|') {
        consume('|');
        value = block->expr;
        block = exclusive_or_expression(def, block);
        block->expr = eval_expr(def, block, IR_OP_OR, value, block->expr);
    }

    return block;
}

static struct block *logical_and_expression(
    struct definition *def,
    struct block *block)
{
    struct block *right;

    block = inclusive_or_expression(def, block);
    if (peek().token == LOGICAL_AND) {
        consume(LOGICAL_AND);
        right = cfg_block_init(def);
        block = eval_logical_and(
            def, block, right, logical_and_expression(def, right));
    }

    return block;
}

static struct block *logical_or_expression(
    struct definition *def,
    struct block *block)
{
    struct block *right;

    block = logical_and_expression(def, block);
    if (peek().token == LOGICAL_OR) {
        consume(LOGICAL_OR);
        right = cfg_block_init(def);
        block = eval_logical_or(
            def, block, right, logical_or_expression(def, right));
    }

    return block;
}

static struct block *conditional_expression(
    struct definition *def,
    struct block *block)
{
    block = logical_or_expression(def, block);
    if (peek().token == '?') {
        struct var condition = block->expr;
        struct block
            *t = cfg_block_init(def),
            *f = cfg_block_init(def),
            *next = cfg_block_init(def);

        consume('?');
        block->jump[0] = f;
        block->jump[1] = t;

        t = expression(def, t);
        t->jump[0] = next;

        consume(':');
        f = conditional_expression(def, f);
        f->jump[0] = next;

        next->expr = eval_conditional(def, condition, t, f);
        if (next->expr.kind == IMMEDIATE && !is_void(next->expr.type)) {
            block->expr = next->expr;
            block->jump[0] = NULL;
            block->jump[1] = NULL;
        } else {
            block = next;
        }
    }

    return block;
}

struct block *assignment_expression(
    struct definition *def,
    struct block *block)
{
    struct var target;

    block = conditional_expression(def, block);
    target = block->expr;
    switch (peek().token) {
    case '=':
        consume('=');
        block = assignment_expression(def, block);
        break;
    case MUL_ASSIGN:
        consume(MUL_ASSIGN);
        block = assignment_expression(def, block);
        block->expr = eval_expr(def, block, IR_OP_MUL, target, block->expr);
        break;
    case DIV_ASSIGN:
        consume(DIV_ASSIGN);
        block = assignment_expression(def, block);
        block->expr = eval_expr(def, block, IR_OP_DIV, target, block->expr);
        break;
    case MOD_ASSIGN:
        consume(MOD_ASSIGN);
        block = assignment_expression(def, block);
        block->expr = eval_expr(def, block, IR_OP_MOD, target, block->expr);
        break;
    case PLUS_ASSIGN:
        consume(PLUS_ASSIGN);
        block = assignment_expression(def, block);
        block->expr = eval_expr(def, block, IR_OP_ADD, target, block->expr);
        break;
    case MINUS_ASSIGN:
        consume(MINUS_ASSIGN);
        block = assignment_expression(def, block);
        block->expr = eval_expr(def, block, IR_OP_SUB, target, block->expr);
        break;
    case AND_ASSIGN:
        consume(AND_ASSIGN);
        block = assignment_expression(def, block);
        block->expr = eval_expr(def, block, IR_OP_AND, target, block->expr);
        break;
    case OR_ASSIGN:
        consume(OR_ASSIGN);
        block = assignment_expression(def, block);
        block->expr = eval_expr(def, block, IR_OP_OR, target, block->expr);
        break;
    case XOR_ASSIGN:
        consume(XOR_ASSIGN);
        block = assignment_expression(def, block);
        block->expr = eval_expr(def, block, IR_OP_XOR, target, block->expr);
        break;
    default:
        return block;
    }

    block->expr = eval_assign(def, block, target, block->expr);
    return block;
}

struct var constant_expression(void)
{
    struct block
        *head = cfg_block_init(NULL),
        *tail;

    tail = conditional_expression(NULL, head);
    if (tail != head || tail->expr.kind != IMMEDIATE) {
        error("Constant expression must be computable at compile time.");
        exit(1);
    }

    return tail->expr;
}

struct block *expression(struct definition *def, struct block *block)
{
    block = assignment_expression(def, block);
    while (peek().token == ',') {
        consume(',');
        block = assignment_expression(def, block);
    }

    return block;
}
