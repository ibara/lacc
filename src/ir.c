#include "ir.h"
#include "symbol.h"
#include "error.h"

#include <stdlib.h>
#include <stdio.h>

static const char *
mklabel()
{
    static int n;

    char *name = malloc(sizeof(char) * 16);
    snprintf(name, 12, ".L%d", n++);

    return name;
}

/* Keep track of to last created function, adding new block to its internal
 * bookkeeping list. */
static function_t *function;

function_t *
cfg_create(const symbol_t *symbol)
{
    function_t *fun = calloc(1, sizeof(function_t));
    fun->symbol = symbol;

    function = fun;
    return fun;
}

block_t *
block_init()
{
    block_t *block;
    if (!function) {
        error("Internal error, cannot create cfg node without function");
    }
    block = calloc(1, sizeof(struct block));
    block->label = mklabel();

    if (function->size == function->capacity) {
        function->capacity += 16;
        function->nodes = realloc(function->nodes, function->capacity * sizeof(block_t*));
    }
    function->nodes[function->size++] = block;

    return block;
}

void
cfg_finalize(function_t *func)
{
    if (!func) return;
    if (func->capacity) {
        int i;
        for (i = 0; i < func->size; ++i) {
            block_t *block = func->nodes[i];
            if (block->n) free(block->code);
            if (block->label) free((void *) block->label);
            free(block);
        }
        free(func->nodes);
    }
    free(func);
}

void
ir_append(block_t *block, op_t op)
{
    block->n += 1;
    block->code = realloc(block->code, sizeof(struct op) * block->n);
    block->code[block->n - 1] = op;
    return;
}
