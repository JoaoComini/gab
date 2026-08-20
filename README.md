# Gab

Gab is a small, statically typed scripting language for embedding in C
programs.

It is built around one idea: **a script struct has the same memory layout as
the equivalent C struct.** There is no marshalling layer and no conversion step
at the boundary — a host passes a `Player` to a script by handing over its
bytes, and the script reads them in place. Static types are what make that
layout knowable ahead of time.

Gab is early. The embedding API is the finished part; the language surface is
still small. See [Features](#features) for what works today.

## Embedding

```c
#include "gab.h"
#include <stdio.h>

typedef struct { int health; int mana; } Player;

int main(void) {
    GabVM *vm = gab_vm_new();
    GabError err;

    const char *src =
        "module game;\n"
        "struct Player { health: int, mana: int }\n"
        "func damage(p: Player, amount: int): int {\n"
        "    let left: int = p.health - amount;\n"
        "    if left < 0 { return 0; }\n"
        "    return left;\n"
        "}\n";

    if (!gab_load(vm, "game.gab", src, &err)) {
        fprintf(stderr, "game.gab:%d: %s\n", err.line, err.message);
        return 1;
    }

    // The script's layout is the C layout. Check it rather than trust it.
    const GabType *type = gab_find_type(vm, "game", "Player");
    if (gab_type_size(type) != sizeof(Player)) {
        return 1;
    }

    // Resolve once; call every frame with no lookup.
    GabFunc *fn = gab_lookup(vm, "game", "damage", &err);
    GabCall *call = gab_call_init(fn, &err);

    Player p = { .health = 100, .mana = 50 };
    gab_arg_struct(call, 0, &p, sizeof p);
    gab_arg_int(call, 1, 30);

    int32_t left = 0;
    if (gab_call(vm, call, &left, &err) == GAB_OK) {
        printf("health: %d\n", left);   // health: 70
    }

    gab_call_free(call);
    gab_vm_free(vm);
    return 0;
}
```

`src/gab.h` is the only header a host includes, and it is the reference
documentation: every call carries a comment saying what it guarantees and what
it does not. Nothing is printed by the library — diagnostics come back through
`GabError` so the host decides where they go.

Four properties are worth knowing before you build against it:

- **Hot reload.** Loading a unit name that is already loaded compiles the new
  source over the old. Handles taken before the reload keep working and call
  the new body. If a reload changes a function's signature, calls staged
  against it report `GAB_ERR_STALE` rather than building a frame from stale
  arguments; the host restages and calls again.
- **Little to free.** The VM owns the units it compiles and the handles it
  hands out. `GabCall` is the one exception, because it holds one caller's
  staged arguments.
- **Layout is checked, not trusted.** `gab_type_size`, `gab_type_align`, and
  `gab_field_offset` exist so a host can assert the script's layout against its
  own `sizeof` and `offsetof`.
- **Objects have one owner.** `gab_new` hands the host the only reference to an
  object, and `gab_free` gives it back. A pointer staged with
  `gab_arg_pointer` is borrowed for the call, so the host goes on owning it; a
  function returning `*T` hands ownership over, and the host frees it.

## The language

```
module game;

struct World {
    tick: int,
}

struct Player {
    health: int,
    mana: int,

    world: ref World,
}

func heal(p: ref Player, amount: int) {
    p.health = p.health + amount;
}

func (p: ref Player) is_alive(): bool {
    return p.health > 0;
}
```

Field access reaches through a pointer the way `->` does in C, so `p.health`
works whether `p` is a `Player`, a `*Player`, or a `ref Player`.

Parameters and receivers borrow. A receiver is `T` or `ref T` — by value, which
copies, or by borrow, which mutates what the caller holds — and a parameter is
`T` or `ref T` the same way. Never `*T`: a callee is handed its arguments for the
duration of the call and frees nothing, so an owning parameter would spell an
ownership it cannot have. An owned `*T` is lent to either, since lending is all
a callee asks for.

`&x` yields `ref T`, because taking an address borrows: the slot it names is
owned by whoever declared it. `ref ref T` is a borrow of a borrow, which is what
`&` applied twice produces.

Taking the address of an *owning* pointer is refused. It would be a `ref *T` — a
borrow of the variable rather than of the object — which is what an out-parameter
needs, and assigning through one would free the caller's old object from inside
the callee. Return ownership instead; the transfer is then visible at the call
site. Writing *through* a borrow is unaffected, so a callee filling in a struct
the caller owns works as it always did.

Memory is uniquely owned. `new T` yields a `*T` that exactly one slot owns and
that is freed where that slot goes out of scope; freeing an object frees what
its fields own. An owning field or variable may only be given a value nothing
else owns — `new`, or a call handing its result over — so `ref T` is how
something is named without being owned, as a child names its parent.

The rule behind all of this: **`*T` marks a slot that can free what it holds.**
That is a `let`, a struct field, and a return type — each outlives the statement
and each is where a free is emitted. A parameter and a receiver are neither, so
they take `ref T`; `&x` is an address rather than an allocation, so it yields one
too.

There is no reference count and no runtime liveness check: a `ref T` whose
object has been freed dangles, exactly as a C pointer would.

What the compiler does catch is a borrow moved somewhere that outlives what it
names — returning a `ref` to a local, or storing one where the pointee dies
first. A borrow handed back by a call is taken to borrow from its
shortest-lived argument, since which one it really came from would need a
per-function summary. What is not caught is a borrow that outlives its pointee
without ever being moved: holding a `ref` while the object's owner frees it is
undefined, as holding a C++ reference across the owner's destruction is.

`for` is the only loop keyword, and spells all three shapes:

```
for { ... }                  // forever, until a 'break'
for ready { ... }            // while the condition holds
for let i: int = 0; i < n; i = i + 1 { ... }
```

The three-clause form scopes its initializer to the loop, and each of its
clauses may be omitted. `break` leaves the loop and `continue` starts the next
iteration — from the post clause, so the counting form still advances. There is
no `while` and no separate infinite-loop keyword: both are `for` with one clause
more or less, and a second spelling would say nothing the first does not.

## Features

| | |
| --- | --- |
| Types | `int` (32-bit), `float` (32-bit), `bool`, structs, pointers `*T`, borrows `ref T` |
| Declarations | `let` with inferred or annotated type, `func`, `struct`, `module` |
| Functions | Parameters and returns of any type, structs by value, methods with a receiver, recursion, forward references |
| Control flow | `if` / `else`, `for` in three forms, `break`, `continue`, `return`, nested blocks with shadowing |
| Operators | `+` `-` `*` `/`, unary `-`, `==` `!=` `<` `>` `<=` `>=`, `&&` `||`, `&` and `*`, field access |
| Memory | Unique ownership, `new`, `ref` borrows, scope-based free |
| Modules | `module` namespaces, resolved per unit with a root fallback |

Not yet implemented:

| | |
| --- | --- |
| Strings | No string type or literals |
| Arrays | No array type or indexing |
| Comments | Not recognised by the lexer |
| Operators | `!`, `%`, compound assignment (`+=`), bitwise |
| Literals | No struct literals (`V{x: 1}`) |
| Conversion | `int` and `float` do not mix; no cast syntax |

## Building

Requires CMake 3.16+ and clang.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

Warnings are errors. To build and run the suite under AddressSanitizer and
UndefinedBehaviorSanitizer:

```sh
cmake -S . -B build-asan -DGAB_SANITIZE=address,undefined
cmake --build build-asan
ctest --test-dir build-asan
```

The suite passes clean under both, so any sanitizer report is a regression.

## Contributing

Run `clang-format` before committing; the config is in `.clang-format`. Add a
test in `test/` for whatever you change, and check that it fails before your
fix as well as passing after it.
