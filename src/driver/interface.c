#include "driver/interface.h"

#include "ast/stmt.h"

#include "ast/type_expr.h"
#include <stdlib.h>

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
        fprintf(out, "box<");
        print_type(out, type->indirect.inner);
        fputc('>', out);
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

/* A signature and never a body: what an interface file states is what a caller must know. */
static void print_func(FILE *out, const ASTFuncDecl *func, const char *indent, size_t inherited) {
    fprintf(out, "%s", indent);

    /* 'caller' is how a call is made and not where the body is, so it precedes and never replaces. */
    if (func->syntax & FUNC_SYN_CALLER) {
        fprintf(out, "caller ");
    }

    if (func->syntax & FUNC_SYN_FOREIGN) {
        fprintf(out, "extern \"C\" ");
    } else if (func->syntax & FUNC_SYN_INTRINSIC) {
        fprintf(out, "intrinsic ");
    } else if (func->body) {
        /* A body compiled into the library it came from, which a reader links against rather than
         * compiles again. */
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

    fprintf(out, ";\n");
}

static void print_stmt(FILE *out, const ASTStmt *stmt);

static void print_members(FILE *out, const ASTStmtList *members, size_t inherited) {
    for (size_t i = 0; i < members->size; i++) {
        const ASTStmt *member = members->data[i];

        if (member && member->kind == STMT_FUNC_DECL) {
            size_t own = member->func_decl.type_param_count;

            print_func(out, &member->func_decl, "    ", inherited < own ? inherited : own);
        }
    }
}

static void print_stmt(FILE *out, const ASTStmt *stmt) {
    if (!stmt) {
        return;
    }

    switch (stmt->kind) {
    case STMT_FUNC_DECL:
        print_func(out, &stmt->func_decl, "", 0);
        return;

    case STMT_STRUCT_DECL: {
        const ASTStructDecl *decl = &stmt->struct_decl;

        fprintf(out, "struct %.*s", (int)decl->name.length, decl->name.data);
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

        print_members(out, &decl->members, 0);

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

        print_members(out, &impl->members, impl->param_count);

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

bool gab_interface_write(const ASTUnit *unit, const char *path) {
    FILE *out = fopen(path, "w");

    if (!out) {
        return false;
    }

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
