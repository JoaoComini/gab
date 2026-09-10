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
        if (type->qualifier) {
            fprintf(out, "%s::", type->qualifier->name->data);
        }

        fprintf(out, "%s", type->name->name->data);
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
static void print_params(FILE *out, ASTIdent *const *names, TypeExpr *const *bounds, size_t count) {
    if (!count) {
        return;
    }

    fputc('<', out);

    for (size_t i = 0; i < count; i++) {
        if (i) {
            fprintf(out, ", ");
        }

        fprintf(out, "%s", names[i]->name->data);

        if (bounds && bounds[i]) {
            fprintf(out, ": ");
            print_type(out, bounds[i]);
        }
    }

    fputc('>', out);
}

static void print_block(FILE *out, const Facts *facts, const ASTStmt *stmt, int depth);

/* A generic is instantiated by whoever names it, so its interface carries the body rather than a symbol
 * to link against: nothing was compiled for arguments the declaring unit never saw. */
static bool carries_body(const ASTFuncDecl *func) { return func->body && func->type_param_count > 0; }

/* How deep a member sits, which its body's statements continue from. */
static size_t indent_depth(const char *indent) { return strlen(indent) / 4; }

/* A signature and never a body: what an interface file states is what a caller must know. */
static void print_func(FILE *out, const Facts *facts, const ASTFuncDecl *func, const char *indent,
                       size_t inherited, bool states_only) {
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

    fprintf(out, "func %s", func->name->name->data);

    /* A member's own parameters follow the ones its block gave it, which the block already states. */
    print_params(out, func->type_params + inherited, func->type_param_bounds + inherited,
                 func->type_param_count - inherited);

    fputc('(', out);

    for (size_t i = 0; i < func->params.size; i++) {
        const ASTField *param = func->params.data[i];

        if (i) {
            fprintf(out, ", ");
        }

        fprintf(out, "%s: ", param->name->name->data);
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
    print_block(out, facts, func->body, (int)indent_depth(indent));
    fputc('\n', out);
}

static void print_expr(FILE *out, const Facts *facts, const ASTExpr *expr);

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

static void print_args(FILE *out, const Facts *facts, const ASTExprList *args, size_t from) {
    fputc('(', out);

    for (size_t i = from; i < args->size; i++) {
        if (i > from) {
            fprintf(out, ", ");
        }

        print_expr(out, facts, args->data[i]);
    }

    fputc(')', out);
}

static void print_expr(FILE *out, const Facts *facts, const ASTExpr *expr) {
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
        print_expr(out, facts, expr->bin_op.left);
        fprintf(out, " %s ", bin_op_text(expr->bin_op.op));
        print_expr(out, facts, expr->bin_op.right);
        fputc(')', out);
        return;

    /* The arguments applied to it are held as a written type of its own name, which states both. */
    case EXPR_NAME:
        if (expr->name.owner_type_expr) {
            print_type(out, expr->name.owner_type_expr);
            return;
        }

        fprintf(out, "%s", expr->name.name->name->data);
        return;

    case EXPR_QUALIFIED:
        if (expr->qualified.owner_type_expr) {
            print_type(out, expr->qualified.owner_type_expr);
        } else {
            fprintf(out, "%s", expr->qualified.qualifier->name->data);
        }

        fprintf(out, "::%s", expr->qualified.name->name->data);
        return;

    /* The arguments are held as an application of the builtin's own name, which is written once. */
    case EXPR_BUILTIN:
        fprintf(out, "@%s", expr->builtin.name->name->data);

        if (expr->builtin.type_expr && expr->builtin.type_expr->kind == TYPE_EXPR_APPLY) {
            fputc('<', out);

            for (size_t i = 0; i < expr->builtin.type_expr->apply.args.size; i++) {
                if (i) {
                    fprintf(out, ", ");
                }

                print_type(out, expr->builtin.type_expr->apply.args.data[i]);
            }

            fputc('>', out);
        }

        return;

    case EXPR_CALL: {
        /* A conversion names a type where a call names a function, so the type alone is written. */
        if (fact_call_kind(facts, expr) == CALL_CONVERSION && expr->call.target &&
            expr->call.target->kind == EXPR_NAME && expr->call.target->name.owner_type_expr) {
            print_type(out, expr->call.target->name.owner_type_expr);
            print_args(out, facts, &expr->call.args, 0);
            return;
        }

        print_expr(out, facts, expr->call.target);
        print_args(out, facts, &expr->call.args, 0);
        return;
    }

    case EXPR_FIELD:
        print_expr(out, facts, expr->field.target);
        fprintf(out, ".%s", expr->field.name->name->data);
        return;

    case EXPR_INDEX:
        print_expr(out, facts, expr->index.target);
        fputc('[', out);
        print_expr(out, facts, expr->index.index);
        fputc(']', out);
        return;

    case EXPR_ADDR_OF:
        fputc('&', out);
        print_expr(out, facts, expr->unary.target);
        return;

    case EXPR_DEREF:
        fputc('*', out);
        print_expr(out, facts, expr->unary.target);
        return;

    case EXPR_NEG:
        fprintf(out, "-");
        print_expr(out, facts, expr->unary.target);
        return;

    case EXPR_NOT:
        fputc('!', out);
        print_expr(out, facts, expr->unary.target);
        return;

    case EXPR_BOX:
        fprintf(out, "box ");
        print_expr(out, facts, expr->box_expr.value);
        return;

    case EXPR_ARRAY_LIT:
        fputc('[', out);

        for (size_t i = 0; i < expr->array_lit.elements.size; i++) {
            if (i) {
                fprintf(out, ", ");
            }

            print_expr(out, facts, expr->array_lit.elements.data[i]);
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

            fprintf(out, "%s: ", field->name->name->data);
            print_expr(out, facts, field->value);
        }

        fprintf(out, " }");
        return;
    }
}

static void print_body_stmt(FILE *out, const Facts *facts, const ASTStmt *stmt, int depth);

static void print_indent(FILE *out, int depth) {
    for (int i = 0; i < depth; i++) {
        fprintf(out, "    ");
    }
}

/* A block prints its own braces, so a statement that holds one does not print them again. */
static void print_block(FILE *out, const Facts *facts, const ASTStmt *stmt, int depth) {
    if (!stmt || stmt->kind != STMT_BLOCK) {
        fprintf(out, "{\n");
        print_body_stmt(out, facts, stmt, depth + 1);
        print_indent(out, depth);
        fprintf(out, "}");
        return;
    }

    fprintf(out, "{\n");

    for (size_t i = 0; i < stmt->block.list.size; i++) {
        print_body_stmt(out, facts, stmt->block.list.data[i], depth + 1);
    }

    print_indent(out, depth);
    fprintf(out, "}");
}

/* The header of a 'for', which prints on one line however many of its three parts were written. */
static void print_for_header(FILE *out, const Facts *facts, const ASTForStmt *loop) {
    if (!loop->init && !loop->post) {
        if (loop->condition) {
            fputc(' ', out);
            print_expr(out, facts, loop->condition);
        }

        fputc(' ', out);
        return;
    }

    fputc(' ', out);

    if (loop->init) {
        /* Written without its own indent or newline, since the header states it inline. */
        if (loop->init->kind == STMT_VAR_DECL) {
            const ASTVarDecl *decl = &loop->init->var_decl;

            fprintf(out, "let %s", decl->name->name->data);

            if (decl->type_expr) {
                fprintf(out, ": ");
                print_type(out, decl->type_expr);
            }

            if (decl->initializer) {
                fprintf(out, " = ");
                print_expr(out, facts, decl->initializer);
            }
        } else if (loop->init->kind == STMT_ASSIGN) {
            print_expr(out, facts, loop->init->assign.target);
            fprintf(out, " = ");
            print_expr(out, facts, loop->init->assign.value);
        }
    }

    fprintf(out, "; ");
    print_expr(out, facts, loop->condition);
    fprintf(out, "; ");

    if (loop->post && loop->post->kind == STMT_ASSIGN) {
        print_expr(out, facts, loop->post->assign.target);
        fprintf(out, " = ");
        print_expr(out, facts, loop->post->assign.value);
    }

    fputc(' ', out);
}

static void print_body_stmt(FILE *out, const Facts *facts, const ASTStmt *stmt, int depth) {
    if (!stmt) {
        return;
    }

    print_indent(out, depth);

    switch (stmt->kind) {
    case STMT_VAR_DECL: {
        const ASTVarDecl *decl = &stmt->var_decl;

        fprintf(out, "let %s", decl->name->name->data);

        if (decl->type_expr) {
            fprintf(out, ": ");
            print_type(out, decl->type_expr);
        }

        if (decl->initializer) {
            fprintf(out, " = ");
            print_expr(out, facts, decl->initializer);
        }

        fprintf(out, ";\n");
        return;
    }

    case STMT_EXPR:
        print_expr(out, facts, stmt->expr.value);
        fprintf(out, ";\n");
        return;

    case STMT_ASSIGN:
        print_expr(out, facts, stmt->assign.target);
        fprintf(out, " = ");
        print_expr(out, facts, stmt->assign.value);
        fprintf(out, ";\n");
        return;

    case STMT_COMPOUND_ASSIGN:
        print_expr(out, facts, stmt->compound_assign.target);
        fprintf(out, " %s= ", bin_op_text(stmt->compound_assign.op));
        print_expr(out, facts, stmt->compound_assign.value);
        fprintf(out, ";\n");
        return;

    case STMT_RETURN:
        fprintf(out, "return");

        if (stmt->ret.result) {
            fputc(' ', out);
            print_expr(out, facts, stmt->ret.result);
        }

        fprintf(out, ";\n");
        return;

    case STMT_JUMP:
        fprintf(out, "%s;\n", stmt->jump.is_break ? "break" : "continue");
        return;

    case STMT_IF:
        fprintf(out, "if ");
        print_expr(out, facts, stmt->ifstmt.condition);
        fputc(' ', out);
        print_block(out, facts, stmt->ifstmt.then_block, depth);

        if (stmt->ifstmt.else_block) {
            fprintf(out, " else ");
            print_block(out, facts, stmt->ifstmt.else_block, depth);
        }

        fputc('\n', out);
        return;

    case STMT_FOR:
        fprintf(out, "for");
        print_for_header(out, facts, &stmt->forstmt);
        print_block(out, facts, stmt->forstmt.body, depth);
        fputc('\n', out);
        return;

    case STMT_BLOCK:
        print_block(out, facts, stmt, depth);
        fputc('\n', out);
        return;

    case STMT_FUNC_DECL:
    case STMT_STRUCT_DECL:
    case STMT_IMPL:
    case STMT_INTERFACE_DECL:
        return;
    }
}

static void print_stmt(FILE *out, const Facts *facts, const ASTStmt *stmt);

static void print_members(FILE *out, const Facts *facts, const ASTStmtList *members, size_t inherited,
                          bool states_only) {
    for (size_t i = 0; i < members->size; i++) {
        const ASTStmt *member = members->data[i];

        if (member && member->kind == STMT_FUNC_DECL) {
            size_t own = member->func_decl.type_param_count;

            print_func(out, facts, &member->func_decl, "    ", inherited < own ? inherited : own,
                       states_only);
        }
    }
}

static void print_stmt(FILE *out, const Facts *facts, const ASTStmt *stmt) {
    if (!stmt) {
        return;
    }

    switch (stmt->kind) {
    case STMT_FUNC_DECL:
        print_func(out, facts, &stmt->func_decl, "", 0, false);
        return;

    case STMT_STRUCT_DECL: {
        const ASTStructDecl *decl = &stmt->struct_decl;

        fprintf(out, "%sstruct %s", decl->intrinsic ? "intrinsic " : "", decl->name->name->data);
        print_params(out, decl->params, NULL, decl->param_count);
        fprintf(out, " {\n");

        for (size_t i = 0; i < decl->fields.size; i++) {
            const ASTField *field = decl->fields.data[i];

            fprintf(out, "    %s: ", field->name->name->data);
            print_type(out, field->type_expr);
            fprintf(out, ",\n");
        }

        fprintf(out, "}\n");
        return;
    }

    case STMT_INTERFACE_DECL: {
        const ASTInterfaceDecl *decl = &stmt->interface_decl;

        fprintf(out, "interface %s", decl->name->name->data);
        print_params(out, decl->params, NULL, decl->param_count);
        fprintf(out, " {\n");

        print_members(out, facts, &decl->members, 0, true);

        fprintf(out, "}\n");
        return;
    }

    case STMT_IMPL: {
        const ASTImplStmt *impl = &stmt->impl;

        fprintf(out, "impl");

        print_params(out, impl->param_names, impl->param_bounds, impl->param_count);

        fputc(' ', out);
        print_type(out, impl->type);

        if (impl->interface_name) {
            fprintf(out, " as %s", impl->interface_name->name->data);

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

        print_members(out, facts, &impl->members, impl->param_count, false);

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

static bool import_stated_before(const ASTModule *module, size_t file, size_t index, const String *name) {
    for (size_t f = 0; f <= file; f++) {
        const ASTImportList *imports = &module->files.data[f]->imports;

        for (size_t i = 0; i < (f == file ? index : imports->size); i++) {
            if (imports->data[i].name->name == name) {
                return true;
            }
        }
    }

    return false;
}

void gab_interface_print(const ASTModule *module, const Facts *facts, FILE *out) {
    fprintf(out, "module %s;\n", module->name->name->data);

    /* What this module imports, so linking against it reaches the objects its bodies call into. An
     * interface states the module, so a name any of its files imports is stated once. */
    for (size_t f = 0; f < module->files.size; f++) {
        const ASTImportList *imports = &module->files.data[f]->imports;

        for (size_t i = 0; i < imports->size; i++) {
            const String *name = imports->data[i].name->name;

            if (!import_stated_before(module, f, i, name)) {
                fprintf(out, "import %s;\n", name->data);
            }
        }
    }

    fputc('\n', out);

    for (size_t f = 0; f < module->files.size; f++) {
        const ASTFile *file = module->files.data[f];

        for (size_t i = 0; i < file->statements.size; i++) {
            print_stmt(out, facts, file->statements.data[i]);
        }
    }
}

bool gab_interface_write(const ASTModule *module, const Facts *facts, const char *path) {
    FILE *out = fopen(path, "w");

    if (!out) {
        return false;
    }

    gab_interface_print(module, facts, out);

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
