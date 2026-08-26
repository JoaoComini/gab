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

- **A load is all or nothing.** A unit that fails to compile, or that names an
  extern nothing registered, declares nothing and installs nothing — the names
  it got as far as declaring are withdrawn, so the host fixes the source and
  loads it again. There is no reload: a declaration stands once made, and a
  second unit declaring the same name collides.
- **Little to free.** The VM owns the units it compiles and the handles it
  hands out. `GabCall` is the one exception, because it holds one caller's
  staged arguments.
- **Layout is checked, not trusted.** `gab_type_size`, `gab_type_align`, and
  `gab_field_offset` exist so a host can assert the script's layout against its
  own `sizeof` and `offsetof`.
- **Externs are bound at load.** A script declares `extern func f(x: int):
  int;` and the host supplies the body with `gab_extern`. The binding is
  resolved while the unit loads, so an extern nothing supplies is a load
  failure naming the function rather than a trap the first time that branch
  runs. Registrations outlive every load.
- **Objects have one owner.** `gab_new` hands the host the only reference to an
  object, and `gab_free` gives it back. A pointer staged with
  `gab_arg_pointer` is borrowed for the call, so the host goes on owning it; a
  function returning `box T` hands ownership over, and the host frees it.

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
works whether `p` is a `Player`, a `box Player`, or a `ref Player`.

A parameter spells ownership the way a local and a field do: bare `box T` owns what
it is given and frees it when the call ends, `ref T` borrows and frees nothing,
and `T` is by value, which copies. Ownership is therefore part of the signature,
and a call site can tell from the declaration alone whether it must move:

```
func consume(b: box Box): int { ... }   // owns; freed when the call ends
func peek(b: ref Box): int { ... }   // borrows; the caller goes on owning

consume(move a);                     // required: 'a' is dead afterwards
peek(a);                             // no move: nothing changes hands
```

Borrowing is implicit and moving is explicit, which is the reverse of Rust's
rule. Where a `ref T` is expected, whatever is given is borrowed: an owned
`box T` is lent without a move, and a plain `T` has its address taken. Nothing
is spelled at the call site because nothing needs to be — a borrow is not
destructive, so there is no reason to warn the reader. `move` stays explicit for
exactly that reason: it is the operation that ends a variable's life.

```
let b: ref Box = a;    // borrows 'a', whatever 'a' is
peek(a);               // same, at a call
```

Ownership moved into a parameter may be handed back out as an owned return — the
caller gave it up, so returning it transfers rather than duplicating — while a
`ref T` may not become a `box T` return, which would hand out ownership nobody
granted.

A receiver is `T` or `ref T` — by value, which copies, or by borrow, which
mutates what the caller holds. Never `box T`: a method is handed its receiver for
the duration of the call, and there is no call-site spelling that would give
that ownership away.

Only something with a home in memory can be borrowed. A call result is a
temporary with no address to name, so it must be bound to a variable first. A
string literal is the exception: its characters live in the unit's arena, which
outlives every value that reads them, so a literal *is* a `str`.

A `str` is a view of characters: where they are and how many, over bytes
something else keeps alive. That something is often a `String`, which owns its
characters and lends them — but just as often it is the unit's arena behind a
literal, or a host's buffer behind a struct field, with no `String` anywhere. It
is the type most code passes around; `String` is what you reach for when the
characters have to outlive the expression that made them.

The two are separate types, not one type in two states. `ref` means an
indirection wherever it is written, so a `ref String` is the address of a slot
holding a header, and nothing converts it to a `str` implicitly — deref it and
the `String` lends from there. A literal borrows the arena; `..` joins,
allocating a `String` that the slot it lands in frees:

```
func greet(name: str): String {
    return "hello, " .. name;     // allocates; the caller owns what comes back
}

let banner: str = "gab";          // borrows the arena, frees nothing
```

Joining has its own operator rather than overloading `+`. Arithmetic does not
allocate and joining always does, and which happened should be readable without
knowing the operand types. `..` binds looser than arithmetic and tighter than
comparison, and it is the operator an array will join with too.

A join always allocates, whatever its operands are: `"hello, " .. "world"` owns
its characters exactly as a join with a runtime operand does. What a join may
initialise is decided by how it was written rather than by what the compiler
could evaluate early, so a literal is the only string that borrows the arena.

`to_owned()` copies the characters a borrow names into a string that owns them,
which is how anything arena-backed becomes something a `String` slot may hold.
It is named for what it produces rather than `clone`, since it does not hand
back its own type: a `str` receiver yields a `String`.

```
let borrowed: str = "ab";                // borrows the arena
let owned: String = "a" .. "b";          // allocates; the slot frees it
let copied: String = "ab".to_owned();    // copies the arena's characters
```

`new String` allocates a heap slot holding a header, which zeroed is the empty
string — the same thing `new Player` does for a struct's layout. Only an owned
value may be stored where a string owns, so a `box String` takes a join and
refuses a literal: the slot frees what it holds, and a borrow names characters
it did not allocate.

A host struct holds a `str`: the host allocated those characters and goes
on owning them, so the script reads them and frees nothing. That is what keeps a
string field two words the host can lay out with `offsetof`, and it carries the
same caveat every borrow does — the host must outlive the script's use of it.

`ref` and `box` each qualify the one level they spell, so they nest in either
order and to any depth. A `ref box T` is the interesting one: a borrow of the
*slot* holding an owning pointer, rather than of the object. That is what an
out-parameter needs, and assigning through one repoints the caller's slot —
freeing what it held, since nothing names that object once the store lands:

```
func replace(s: ref box Box): int { *s = new Box; return 0; }

let o: box Box = new Box;
replace(o);            // 'o' now owns the new object; the old one is freed
```

A `box T` satisfies both `ref T` and `ref box T`, and the declaration decides
which: the first borrows the object, the second the slot. Lending walks the
owning levels until one matches what the destination asked for and stops there,
so the same argument reaches a different depth for each declaration.

Field access and method calls reach through every pointer level until they find
the struct, so the levels never have to be spelled: `s.n` and `s.bump()` work
whether `s` is a `Box`, a `box Box`, or a `ref box Box`. Only `*` is explicit,
for when the pointer itself is what you mean.

An `Array T` is a header like a string's -- where the elements are and how many
-- over a block it owns. Its length is fixed when it is allocated, and every
index is checked against it: an index outside the array fails the run rather
than reading past the block.

```
let xs: Array int = Array int[3];
xs[0] = 1;
let n: int = xs.len();
```

No `new`: what a slot holds is the header itself, and `new` is for what a slot
points at. The header owns, so a second binding to one is refused exactly as it
is for any owning value, and `move` is how it changes hands.

Freeing an array frees what its elements own, however deep that goes. How many
elements are live is the count the block carries rather than anything its type
says, which is what lets one block type serve every length.

`new` allocates anything with a layout to fill — a struct, or an owning pointer:

```
let o: box box Box = new box Box;   // a heap slot that owns a pointer
*o = new Box;
```

Not a borrow: a heap slot holding one would outlive what it borrows with nothing
tracking that. Freeing a slot that owns a pointer frees what the pointer names,
so a chain frees all the way down.

Memory is uniquely owned. `new T` yields a `box T` that exactly one slot owns and
that is freed where that slot goes out of scope; freeing an object frees what
its fields own. `ref T` is how something is named without being owned, as a
child names its parent.

Copying is the default and is implicit, and a type is copyable exactly when
nothing it holds transitively owns. That is derived from the type rather than
declared on it: a struct of `int`s copies, and so does one holding a `ref`,
while one holding a `box T` does not — the moment a field owns, the struct does.

A non-copyable value needs an explicit `move`, which transfers ownership:

```
let b = move a;    // 'a' is dead from here; reading it is an error
```

Writing `let b = a;` for a non-copyable `a` is refused, since it would leave two
slots believing they own one object. Assigning something new to a moved-from
slot revives it — deadness is about what the slot holds, not a mark the name
carries forever.

A type may say how it is duplicated by declaring a `clone` method, which takes
nothing but its receiver and returns another of its own type. `String` ships
with one; `str` does not, since a borrow already copies by assignment and what
it needs is `to_owned()`, which changes the type rather than duplicating it. A
struct declares its own:

```
func (h: ref Holder) clone(): Holder { ... }

let g = h.clone();   // 'h' is still live; 'g' owns a separate object
```

`clone` is a reserved method name: a `clone` returning some other type, or
taking parameters, is refused where it is declared rather than surprising a
caller. Declaring one does not make the type implicitly copyable — `let g = h;`
stays refused — so duplicating an owning value is always visible at the point
it happens, and the allocation is spelled as the call it is. The copy
diagnostic names `clone()` as a remedy only for a type that declares one, and
tells the others that they do not.

Whether a struct lives on the heap or in a frame does not change what it owns:
an owning field is freed when the struct holding it goes out of scope, wherever
that struct sits. A field starts holding nothing, so the first store into it
frees nothing; a later store frees what the field held before.

Because an owning field starts holding nothing, reaching through one before
anything is stored into it is refused — the slot holds null, and the read would
dereference it. Storing into the field is what makes it readable, and that is
tracked per path like everything else here: a field written on only one arm of
an `if` is not readable after the join. This covers the fields a local's own
declaration nulls; a field reached through a pointer is not tracked, since what
it belongs to was not declared here.

A struct moves whole or not at all. `move h.b` is refused — a half-moved struct
would leave the rest of its fields in a state the language says nothing about —
so `move h` is how ownership of what it holds changes hands.

Whether a slot is dead follows control flow the way a borrow's lifetime does. A
slot moved on one arm of an `if` is dead after the join, and moving in a loop
body is refused because the back-edge would move the same slot twice.

The rule behind all of this: **`box T` marks a slot that can free what it holds.**
That is a `let`, a struct field, a parameter, and a return type — each is where
a free may be emitted. A receiver is not one, so it takes `ref T`; a borrow is an
address rather than an allocation, so it yields one too.

There is no reference count and no runtime liveness check: a `ref T` whose
object has been freed dangles, exactly as a C pointer would.

What the compiler does catch is a borrow moved somewhere that outlives what it
names — returning a `ref` to a local, or storing one where the pointee dies
first. A borrow handed back by a call is taken to borrow from its
shortest-lived argument, since which one it really came from would need a
per-function summary. What is not caught is a borrow that outlives its pointee
without ever being moved: holding a `ref` while the object's owner frees it is
undefined, as holding a C++ reference across the owner's destruction is.

That check follows control flow. What a slot names is tracked per path and
merged where paths rejoin, so a borrow is judged by what holds on every route
to its use: a slot given a short-lived borrow on one arm of an `if` is
short-lived after the join, and a borrow taken at the tail of a loop body is
checked against the code that reads it at the head of the next iteration. A
loop is gone round until what holds at its head stops changing, so a nested one
is judged by what reaches it on every iteration of every loop enclosing it. An
arm that cannot fall through — one ending in `return`, `break`, or `continue` —
is not a route to the join that follows it, so what it left behind constrains
nothing there. A `break` is still a route out of its loop, though, so what it
carries is merged into the state after that loop: a slot moved before a `break`
is dead once the loop is left. A slot reassigned something longer-lived is
usable again; the depth is what the last assignment left, not the deepest the
slot ever held.

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
| Types | `int` (32-bit), `float` (32-bit), `bool`, `String` and views of characters `str`, `Array T`, structs, owning `box T`, borrows `ref T` |
| Declarations | `let` with inferred or annotated type, `func`, `struct`, `module` |
| Functions | Parameters and returns of any type, structs by value, methods with a receiver, recursion, forward references |
| Control flow | `if` / `else`, `for` in three forms, `break`, `continue`, `return`, nested blocks with shadowing |
| Operators | `+` `-` `*` `/` `%`, unary `-` `!`, `==` `!=` `<` `>` `<=` `>=`, `&&` `||`, unary `*`, field access, indexing `xs[i]`, `..` joins |
| Conversions | `int(x)` and `float(x)`; nothing converts implicitly |
| Assignment | `=`, and compound `+=` `-=` `*=` `/=` `%=` on any assignable target |
| Memory | Unique ownership, `new`, `ref` borrows, implicit copy, explicit `move`, user-declared `clone`, scope-based free. A top-level variable may not own: nothing closes over it to free what it holds |
| Modules | `module` names the namespace a unit declares into, `import` the ones it may name |
| Externs | `extern func` declares a host body, bound by name at load |
| Comments | `// line` and `/* block */`, which do not nest |

Not yet implemented:

| | |
| --- | --- |
| Strings | No interpolation, no `substring` or case conversion |
| Arrays | Fixed length once allocated: no `push`, no growth, and no slice type |
| Operators | Bitwise |
| Literals | No struct literals (`V{x: 1}`) |

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
