#include "driver/interface.h"

#include "ast/stmt.h"

#include "ast/expr.h"
#include "ast/type_expr.h"
#include <stdlib.h>
#include <string.h>

static void print_type(FILE *out, const TypeExpr *type) {
    if (!type) {
        return;
    }

    switch (type->kind) {
    case TYPE_EXPR_NAME:
        fprintf(out, "%.*s", (int)type->name.length, type->name.data);
        return;

    case TYPE_EXPR_REF:
        fputc('&', out);
        print_type(out, type->indirect.inner);
        return;

    case TYPE_EXPR_BOX:
        fputc('*', out);
        print_type(out, type->indirect.inner);
        return;

    case TYPE_EXPR_CONST:
        fprintf(out, "%d", type->constant);
        return;

    case TYPE_EXPR_APPLY:
        print_type(out, type->apply.base);
        fputc('<', out);

        for (size_t i = 0; i < type->apply.args.size; i++) {
            if (i) {
                fprintf(out, ", ");
            }

            print_type(out, type->apply.args.data[i]);
        }

        fputc('>', out);
        return;
    }
}

/* The parameters a declaration takes, with the bound each was written with. */
static void print_params(FILE *out, const StringRef *names, TypeExpr *const *bounds, size_t count) {
    if (!count) {
        return;
    }

    fputc('<', out);

    for (size_t i = 0; i < count; i++) {
        if (i) {
            fprintf(out, ", ");
        }

        fprintf(out, "%.*s", (int)names[i].length, names[i].data);

        if (bounds && bounds[i]) {
            fprintf(out, ": ");
            print_type(out, bounds[i]);
        }
    }

    fputc('>', out);
}

static void print_block(FILE *out, const ASTStmt *stmt, int depth);

/* A generic is instantiated by whoever names it, so its interface carries the body rather than a symbol
 * to link against: nothing was compiled for arguments the declaring unit never saw. */
static bool carries_body(const ASTFuncDecl *func) { return func->body && func->type_param_count > 0; }

/* How deep a member sits, which its body's statements continue from. */
static size_t indent_depth(const char *indent) { return strlen(indent) / 4; }

/* A signature and never a body: what an interface file states is what a caller must know. */
static void print_func(FILE *out, const ASTFuncDecl *func, const char *indent, size_t inherited,
                       bool states_only) {
    fprintf(out, "%s", indent);

    /* 'caller' is how a call is made and not where the body is, so it precedes and never replaces. */
    if (func->syntax & FUNC_SYN_CALLER) {
        fprintf(out, "caller ");
    }

    if (func->syntax & FUNC_SYN_FOREIGN) {
        fprintf(out, "extern \"C\" ");
    } else if (func->syntax & FUNC_SYN_INTRINSIC) {
        fprintf(out, "intrinsic ");
    } else if (!states_only && !carries_body(func) && (func->body || (func->syntax & FUNC_SYN_EXTERN))) {
        /* A body compiled into the library it came from, which a reader links against rather than
         * compiles again. What was read as 'extern' is still one, though it arrived without a body.
         * An interface states signatures alone, where saying so again would not parse. */
        fprintf(out, "extern ");
    }

    fprintf(out, "func %.*s", (int)func->name.length, func->name.data);

    /* A member's own parameters follow the ones its block gave it, which the block already states. */
    print_params(out, func->type_params + inherited, func->type_param_bounds + inherited,
                 func->type_param_count - inherited);

    fputc('(', out);

    for (size_t i = 0; i < func->params.size; i++) {
        const ASTField *param = func->params.data[i];

        if (i) {
            fprintf(out, ", ");
        }

        fprintf(out, "%.*s: ", (int)param->name.length, param->name.data);
        print_type(out, param->type_expr);
    }

    fputc(')', out);

    if (func->return_type) {
        fprintf(out, ": ");
        print_type(out, func->return_type);
    }

    if (!carries_body(func)) {
        fprintf(out, ";\n");
        return;
    }

    fputc(' ', out);
    print_block(out, func->body, (int)indent_depth(indent));
    fputc('\n', out);
}

static void print_expr(FILE *out, const ASTExpr *expr);

/* Spelled as the source spells it, so what is read back binds the same operator to the same operands. */
static const char *bin_op_text(BinOp op) {
    switch (op) {
    case BIN_OP_ADD:
        return "+";
    case BIN_OP_SUB:
        return "-";
    case BIN_OP_MUL:
        return "*";
    case BIN_OP_DIV:
        return "/";
    case BIN_OP_MOD:
        return "%";
    case BIN_OP_LESS:
        return "<";
    case BIN_OP_GREATER:
        return ">";
    case BIN_OP_EQUAL:
        return "==";
    case BIN_OP_NEQUAL:
        return "!=";
    case BIN_OP_LEQUAL:
        return "<=";
    case BIN_OP_GEQUAL:
        return ">=";
    case BIN_OP_AND:
        return "&&";
    case BIN_OP_OR:
        return "||";
    }

    return "";
}

static void print_literal(FILE *out, const Literal *literal) {
    switch (literal->kind) {
    case LITERAL_INT:
        fprintf(out, "%lld", (long long)literal->as_int);
        return;

    case LITERAL_FLOAT:
        /* Enough digits to name the same float again, which a shorter spelling would round away. */
        fprintf(out, "%.9g", (double)literal->as_float);
        return;

    case LITERAL_BOOL:
        fprintf(out, "%s", literal->as_bool ? "true" : "false");
        return;

    case LITERAL_STRING:
        fprintf(out, "\"%s\"", literal->as_string->data);
        return;
    }
}

static void print_args(FILE *out, const ASTExprList *args) {
    fputc('(', out);

    for (size_t i = 0; i < args->size; i++) {
        if (i) {
            fprintf(out, ", ");
        }

        print_expr(out, args->data[i]);
    }

    fputc(')', out);
}

static void print_expr(FILE *out, const ASTExpr *expr) {
    if (!expr) {
        return;
    }

    switch (expr->kind) {
    case EXPR_LITERAL:
        print_literal(out, &expr->lit);
        return;

    /* Parenthesized rather than spelled by precedence, so reading it back groups it as it was written. */
    case EXPR_BIN_OP:
        fputc('(', out);
        print_expr(out, expr->bin_op.left);
        fprintf(out, " %s ", bin_op_text(expr->bin_op.op));
        print_expr(out, expr->bin_op.right);
        fputc(')', out);
        return;

    case EXPR_VARIABLE:
        if (expr->var.owner_type_expr) {
            print_type(out, expr->var.owner_type_expr);
            fprintf(out, "::");
        }

        fprintf(out, "%.*s", (int)expr->var.name.length, expr->var.name.data);
        return;

    case EXPR_BUILTIN:
        fprintf(out, "@%.*s", (int)expr->builtin.name.length, expr->builtin.name.data);

        if (expr->builtin.type_expr) {
            fputc('<', out);
            print_type(out, expr->builtin.type_expr);
            fputc('>', out);
        }

        fprintf(out, "()");
        return;

    case EXPR_CALL:
        print_expr(out, expr->call.target);
        print_args(out, &expr->call.args);
        return;

    case EXPR_FIELD:
        print_expr(out, expr->field.target);
        fprintf(out, ".%.*s", (int)expr->field.name.length, expr->field.name.data);
        return;

    case EXPR_INDEX:
        print_expr(out, expr->index.target);
        fputc('[', out);
        print_expr(out, expr->index.index);
        fputc(']', out);
        return;

    case EXPR_ADDR_OF:
        fputc('&', out);
        print_expr(out, expr->unary.target);
        return;

    case EXPR_DEREF:
        fputc('*', out);
        print_expr(out, expr->unary.target);
        return;

    case EXPR_NEG:
        fprintf(out, "-");
        print_expr(out, expr->unary.target);
        return;

    case EXPR_NOT:
        fputc('!', out);
        print_expr(out, expr->unary.target);
        return;

    case EXPR_BOX:
        fprintf(out, "box ");
        print_expr(out, expr->box_expr.value);
        return;

    case EXPR_ARRAY_LIT:
        fputc('[', out);

        for (size_t i = 0; i < expr->array_lit.elements.size; i++) {
            if (i) {
                fprintf(out, ", ");
            }

            print_expr(out, expr->array_lit.elements.data[i]);
        }

        fputc(']', out);
        return;

    case EXPR_STRUCT_LIT:
        print_type(out, expr->struct_lit.type_expr);
        fprintf(out, " { ");

        for (size_t i = 0; i < expr->struct_lit.fields.size; i++) {
            const ASTFieldInit *field = &expr->struct_lit.fields.data[i];

            if (i) {
                fprintf(out, ", ");
            }

            fprintf(out, "%.*s: ", (int)field->name.length, field->name.data);
            print_expr(out, field->value);
        }

        fprintf(out, " }");
        return;
    }
}

static void print_body_stmt(FILE *out, const ASTStmt *stmt, int depth);

static void print_indent(FILE *out, int depth) {
    for (int i = 0; i < depth; i++) {
        fprintf(out, "    ");
    }
}

/* A block prints its own braces, so a statement that holds one does not print them again. */
static void print_block(FILE *out, const ASTStmt *stmt, int depth) {
    if (!stmt || stmt->kind != STMT_BLOCK) {
        fprintf(out, "{\n");
        print_body_stmt(out, stmt, depth + 1);
        print_indent(out, depth);
        fprintf(out, "}");
        return;
    }

    fprintf(out, "{\n");

    for (size_t i = 0; i < stmt->block.list.size; i++) {
        print_body_stmt(out, stmt->block.list.data[i], depth + 1);
    }

    print_indent(out, depth);
    fprintf(out, "}");
}

/* The header of a 'for', which prints on one line however many of its three parts were written. */
static void print_for_header(FILE *out, const ASTForStmt *loop) {
    if (!loop->init && !loop->post) {
        if (loop->condition) {
            fputc(' ', out);
            print_expr(out, loop->condition);
        }

        fputc(' ', out);
        return;
    }

    fputc(' ', out);

    if (loop->init) {
        /* Written without its own indent or newline, since the header states it inline. */
        if (loop->init->kind == STMT_VAR_DECL) {
            const ASTVarDecl *decl = &loop->init->var_decl;

            fprintf(out, "let %.*s", (int)decl->name.length, decl->name.data);

            if (decl->type_expr) {
                fprintf(out, ": ");
                print_type(out, decl->type_expr);
            }

            if (decl->initializer) {
                fprintf(out, " = ");
                print_expr(out, decl->initializer);
            }
        } else if (loop->init->kind == STMT_ASSIGN) {
            print_expr(out, loop->init->assign.target);
            fprintf(out, " = ");
            print_expr(out, loop->init->assign.value);
        }
    }

    fprintf(out, "; ");
    print_expr(out, loop->condition);
    fprintf(out, "; ");

    if (loop->post && loop->post->kind == STMT_ASSIGN) {
        print_expr(out, loop->post->assign.target);
        fprintf(out, " = ");
        print_expr(out, loop->post->assign.value);
    }

    fputc(' ', out);
}

static void print_body_stmt(FILE *out, const ASTStmt *stmt, int depth) {
    if (!stmt) {
        return;
    }

    print_indent(out, depth);

    switch (stmt->kind) {
    case STMT_VAR_DECL: {
        const ASTVarDecl *decl = &stmt->var_decl;

        fprintf(out, "let %.*s", (int)decl->name.length, decl->name.data);

        if (decl->type_expr) {
            fprintf(out, ": ");
            print_type(out, decl->type_expr);
        }

        if (decl->initializer) {
            fprintf(out, " = ");
            print_expr(out, decl->initializer);
        }

        fprintf(out, ";\n");
        return;
    }

    case STMT_EXPR:
        print_expr(out, stmt->expr.value);
        fprintf(out, ";\n");
        return;

    case STMT_ASSIGN:
        print_expr(out, stmt->assign.target);
        fprintf(out, " = ");
        print_expr(out, stmt->assign.value);
        fprintf(out, ";\n");
        return;

    case STMT_COMPOUND_ASSIGN:
        print_expr(out, stmt->compound_assign.target);
        fprintf(out, " %s= ", bin_op_text(stmt->compound_assign.op));
        print_expr(out, stmt->compound_assign.value);
        fprintf(out, ";\n");
        return;

    case STMT_RETURN:
        fprintf(out, "return");

        if (stmt->ret.result) {
            fputc(' ', out);
            print_expr(out, stmt->ret.result);
        }

        fprintf(out, ";\n");
        return;

    case STMT_JUMP:
        fprintf(out, "%s;\n", stmt->jump.is_break ? "break" : "continue");
        return;

    case STMT_IF:
        fprintf(out, "if ");
        print_expr(out, stmt->ifstmt.condition);
        fputc(' ', out);
        print_block(out, stmt->ifstmt.then_block, depth);

        if (stmt->ifstmt.else_block) {
            fprintf(out, " else ");
            print_block(out, stmt->ifstmt.else_block, depth);
        }

        fputc('\n', out);
        return;

    case STMT_FOR:
        fprintf(out, "for");
        print_for_header(out, &stmt->forstmt);
        print_block(out, stmt->forstmt.body, depth);
        fputc('\n', out);
        return;

    case STMT_BLOCK:
        print_block(out, stmt, depth);
        fputc('\n', out);
        return;

    case STMT_FUNC_DECL:
    case STMT_STRUCT_DECL:
    case STMT_IMPL:
    case STMT_INTERFACE_DECL:
        return;
    }
}

static void print_stmt(FILE *out, const ASTStmt *stmt);

static void print_members(FILE *out, const ASTStmtList *members, size_t inherited, bool states_only) {
    for (size_t i = 0; i < members->size; i++) {
        const ASTStmt *member = members->data[i];

        if (member && member->kind == STMT_FUNC_DECL) {
            size_t own = member->func_decl.type_param_count;

            print_func(out, &member->func_decl, "    ", inherited < own ? inherited : own, states_only);
        }
    }
}

static void print_stmt(FILE *out, const ASTStmt *stmt) {
    if (!stmt) {
        return;
    }

    switch (stmt->kind) {
    case STMT_FUNC_DECL:
        print_func(out, &stmt->func_decl, "", 0, false);
        return;

    case STMT_STRUCT_DECL: {
        const ASTStructDecl *decl = &stmt->struct_decl;

        fprintf(out, "%sstruct %.*s", decl->intrinsic ? "intrinsic " : "", (int)decl->name.length,
                decl->name.data);
        print_params(out, decl->params, NULL, decl->param_count);
        fprintf(out, " {\n");

        for (size_t i = 0; i < decl->fields.size; i++) {
            const ASTField *field = decl->fields.data[i];

            fprintf(out, "    %.*s: ", (int)field->name.length, field->name.data);
            print_type(out, field->type_expr);
            fprintf(out, ",\n");
        }

        fprintf(out, "}\n");
        return;
    }

    case STMT_INTERFACE_DECL: {
        const ASTInterfaceDecl *decl = &stmt->interface_decl;

        fprintf(out, "interface %.*s", (int)decl->name.length, decl->name.data);
        print_params(out, decl->params, NULL, decl->param_count);
        fprintf(out, " {\n");

        print_members(out, &decl->members, 0, true);

        fprintf(out, "}\n");
        return;
    }

    case STMT_IMPL: {
        const ASTImplStmt *impl = &stmt->impl;

        fprintf(out, "impl");

        print_params(out, impl->param_names, impl->param_bounds, impl->param_count);

        fputc(' ', out);
        print_type(out, impl->type);

        if (impl->interface_name.length) {
            fprintf(out, " as %.*s", (int)impl->interface_name.length, impl->interface_name.data);

            if (impl->interface_args.size) {
                fputc('<', out);

                for (size_t i = 0; i < impl->interface_args.size; i++) {
                    if (i) {
                        fprintf(out, ", ");
                    }

                    print_type(out, impl->interface_args.data[i]);
                }

                fputc('>', out);
            }
        }

        fprintf(out, " {\n");

        print_members(out, &impl->members, impl->param_count, false);

        fprintf(out, "}\n");
        return;
    }

    default:
        return;
    }
}

/* The declarations hashed, and never the digest line itself: what a reader compiles is what is hashed. */
uint64_t gab_interface_digest(const char *text) {
    uint64_t hash = 1469598103934665603u;

    for (const char *at = text; *at; at++) {
        if (*at == '/' && at[1] == '/') {
            while (*at && *at != '\n') {
                at++;
            }

            if (!*at) {
                break;
            }
        }

        hash = (hash ^ (unsigned char)*at) * 1099511628211u;
    }

    return hash;
}

void gab_interface_symbol(char *out, size_t capacity, const char *module, uint64_t digest) {
    snprintf(out, capacity, "gab.iface.%s.%016llx", module, (unsigned long long)digest);
}

void gab_interface_print(const ASTUnit *unit, FILE *out) {
    fprintf(out, "module %.*s;\n", (int)unit->module_name.length, unit->module_name.data);

    /* What this module imports, so linking against it reaches the objects its bodies call into. */
    for (size_t i = 0; i < unit->imports.size; i++) {
        fprintf(out, "import %.*s;\n", (int)unit->imports.data[i].name.length,
                unit->imports.data[i].name.data);
    }

    fputc('\n', out);

    for (size_t i = 0; i < unit->statements.size; i++) {
        print_stmt(out, unit->statements.data[i]);
    }
}

bool gab_interface_write(const ASTUnit *unit, const char *path) {
    FILE *out = fopen(path, "w");

    if (!out) {
        return false;
    }

    gab_interface_print(unit, out);

    fclose(out);

    return true;
}

char *gab_interface_read(const char *path) {
    FILE *file = fopen(path, "rb");

    if (!file) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *text = malloc((size_t)size + 1);

    size_t read = fread(text, 1, (size_t)size, file);
    text[read] = '\0';

    fclose(file);

    return text;
}
