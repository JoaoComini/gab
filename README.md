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

    if (!gab_vm_load(vm, "game.gab", src, &err)) {
        fprintf(stderr, "game.gab:%d: %s\n", err.line, err.message);
        return 1;
    }

    // The script's layout is the C layout. Check it rather than trust it.
    const GabType *type = gab_vm_find_type(vm, "game", "Player");
    if (gab_type_size(type) != sizeof(Player)) {
        return 1;
    }

    // Resolve once; call every frame with no lookup.
    GabFunc *fn = gab_vm_lookup(vm, "game", "damage", &err);
    GabCall *call = gab_call_init(fn, &err);

    Player p = { .health = 100, .mana = 50 };
    gab_call_struct(call, 0, &p, sizeof p);
    gab_call_int(call, 1, 30);

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
  `gab_type_field_offset` exist so a host can assert the script's layout against its
  own `sizeof` and `offsetof`.
- **Externs are bound at load.** A script declares `extern func f(x: int):
  int;` and the host supplies the body with `gab_extern`. The binding is
  resolved while the unit loads, so an extern nothing supplies is a load
  failure naming the function rather than a trap the first time that branch
  runs. Registrations outlive every load.
- **Objects have one owner.** A host allocates a struct itself, at the size and
  alignment `gab_type_size` and `gab_type_align` report. A pointer staged with
  `gab_call_pointer` is borrowed for the call, so the host goes on owning it.

### Libraries

A host can also declare a type of its own that scripts use as a built-in, which
is how `String` and `Vec` are written — `src/std` builds against `gab.h` and
nothing else. A library names its module, declares its types, binds a body to
each `extern`, then hands over the declarations as source:

```c
GabLib *lib = gab_lib_open(vm, "std", &err);

const GabFieldSpec fields[] = {
    { "data", gab_lib_block_of(lib, gab_lib_param(lib, 0)) },
};

gab_lib_type(lib, &(GabTypeSpec){
    .name = "Vec", .params = 1,
    .fields = fields, .field_count = 1,
}, &err);

gab_lib_bind(lib, "Vec", "push", vec_push, &err);

gab_lib_source(lib,
    "impl<T> Vec<T> {\n"
    "    extern func push(self: &Vec<T>, value: T);\n"
    "}\n", &err);

gab_lib_close(lib);
```

`GabBlock` is the growable buffer those types are built on: a host struct
embedding one has the bytes the VM reads, and `gab_block_reserve` grows it
through the VM's allocator. One body serves every specialization, so it asks
`gab_ctx_type_size` what `T` was instantiated with rather than being compiled
per element type.

## The language

```
module game;

struct World {
    tick: int,
}

struct Player {
    health: int,
    mana: int,

    world: &World,
}

func heal(p: &Player, amount: int) {
    p.health = p.health + amount;
}

impl Player {
    func is_alive(p: &Player): bool {
        return p.health > 0;
    }
}
```

Field access reaches through a pointer the way `->` does in C, so `p.health`
works whether `p` is a `Player`, a `*Player`, or a `&Player`.

A parameter spells ownership the way a local and a field do: bare `*T` owns what
it is given and frees it when the call ends, `&T` borrows and frees nothing,
and `T` is by value, which copies. Ownership is therefore part of the signature,
and the declaration alone decides what a call site does with its argument:

```
func consume(b: *Box): int { ... }   // owns; freed when the call ends
func peek(b: &Box): int { ... }   // borrows; the caller goes on owning

consume(a);                          // hands 'a' over: dead afterwards
peek(a);                             // borrows: nothing changes hands
```

Nothing is written at the site either way. Where a `&T` is expected, whatever
is given is borrowed: an owned `*T` is lent, and a plain `T` has its address
taken. Where an owning slot is expected, whatever is given is handed over. Which
one happens is read off the two types rather than spelled by a keyword, so there
is no `move`: the destination says whether it owns, and that is already enough to
say what the binding does.

```
let b: &Box = a;    // borrows 'a', whatever 'a' is
let c: *Box = a;    // takes 'a' over; 'a' is dead from here
peek(a);               // borrows, at a call
```

Ownership moved into a parameter may be handed back out as an owned return — the
caller gave it up, so returning it transfers rather than duplicating — while a
`&T` may not become a `*T` return, which would hand out ownership nobody
granted.

A function a type owns is declared in an `impl` block for that type, and one
whose first parameter is that type may also be called on a value: `p.is_alive()`
is `Player::is_alive(p)`. The sugar borrows and derefs to reach parameter zero,
but never hands ownership over — a function taking `*T` first consumes it, so it
is called as `Type::name(v)`, where the argument is an argument like any other.

Inside an `impl` block, `Self` names the type the block is for, so a generic
one does not repeat its own arguments. The name is reserved: nothing else may
be declared `Self`, and naming it where no `impl` block encloses it is an
error saying so.

```
impl<T> Holder<T> {
    func get(self: &Self): T { return self.value; }
}
```

An `interface` names a set of signatures, and a type says it supplies them with
`impl Type as Name`. The implementation is checked where it is written rather
than where it is called, so a missing method, or one whose signature disagrees,
is a compile error naming what differed. `Self` in a signature is the
implementing type:

```
interface Countable {
    func count(self: &Self): int;
}

struct Bag { n: int }

impl Bag as Countable {
    func count(self: &Self): int { return self.n; }
}
```

A type parameter is bounded by an interface as `<T: Countable>`, and inside the
body `T` has exactly the methods that interface declares:

```
func total<T: Countable>(x: &T): int { return x.count(); }
```

A generic body is checked once, where it is written, rather than at each call.
So a body naming a method its bound does not declare is an error even if
nothing ever calls it, and a call is checked against the bound rather than
against whatever type happens to reach it.

A type implements a given interface once, and the methods are called the way
any other method is — an interface adds no representation, so `b.count()`
costs what it did before.

An interface takes type parameters of its own, which its signatures name and an
implementation supplies:

```
interface Holder<T> {
    func get(self: &Self): T;
}

impl IntBox as Holder<int> {
    func get(self: &Self): int { return self.n; }
}

impl<T> Box<T> as Holder<T> {
    func get(self: &Self): T { return self.value; }
}
```

A bound names those arguments too, and the body sees the methods at exactly
what it named: under `<H: Holder<int>>`, `h.get()` is an `int`. An interface is
named with as many arguments as it declares, so `Holder` alone and
`Countable<int>` are both errors.

```
func read<H: Holder<int>>(h: &H): int { return h.get(); }
```

Only something with a home in memory can be borrowed. A call result is a
temporary with no address to name, so it must be bound to a variable first. A
string literal is the exception: its characters live in the unit's arena, which
outlives every value that reads them, so a literal *is* a `&str`.

`str` is the characters themselves, and nothing holds one: how far a run of them
goes is not in its type, so no slot, field or parameter has a width to reserve.
What names them is a `&str`, which carries the address and the count side by
side — two words. It is what most code
passes around; `String` is what you reach for when the characters have to
outlive the expression that made them.

This is the one place `ref` is more than an address. A reference carries
whatever naming its pointee requires, which for characters is how many there
are, and for everything else is nothing at all — so a `&Player` stays one
word and a `&str` is two. What decides is the pointee, never the reference.

A `&String` is therefore a different thing again: `String` is sized, so a
reference to one is the plain address of a slot holding a header. Nothing
converts it to a `&str` implicitly — deref it and the `String` lends from
there. A literal borrows the arena; `String::from()` allocates a `String` that
the slot it lands in frees:

```
func greet(name: &str): String {
    let line: String = String::from("hello, ");
    line.append(name);            // grows in place; the caller owns the result
    return line;
}

let banner: &str = "gab";      // borrows the arena, frees nothing
```

`String::from()` copies the characters a borrow names into a string that owns
them, which is how anything arena-backed becomes something a `String` slot may
hold.

```
let borrowed: &str = "ab";            // borrows the arena
let copied: String = String::from("ab");   // copies the arena's characters
```

A `String` owns a block of characters and counts how many of them are live, the
same two fields a `Vec<T>` holds and freed by the same walk. The block carries a
capacity beside the address, so a string has room past its length to grow into:
`push` adds one character and `append` adds another string's, doubling the block
when the live characters fill it. What it lends out is unchanged — a `&str`
is where the characters are and how many, never the capacity they sit in.

```
let mut: String = String::from("ab");
mut.push(99);                            // 'abc'
mut.append("de");                        // 'abcde'
```

`box String::from("")` allocates a heap slot holding a header. Only an owned
value may be stored where a string owns, so a `*String` takes an owned copy
and refuses a literal: the slot frees what it holds, and a borrow names
characters it did not allocate.

A host struct holds a `&str`: the host allocated those characters and goes
on owning them, so the script reads them and frees nothing. That is what keeps a
string field two words the host can lay out with `offsetof`, and it carries the
same caveat every borrow does — the host must outlive the script's use of it.

`&` and `*` each qualify the one level they spell, so they nest in either
order and to any depth. A `&*T` is the interesting one: a borrow of the
*slot* holding an owning pointer, rather than of the object. That is what an
out-parameter needs, and assigning through one repoints the caller's slot —
freeing what it held, since nothing names that object once the store lands:

```
func replace(s: &*Box): int { *s = box Box { n: 0 }; return 0; }

let o: *Box = box Box { n: 0 };
replace(o);            // 'o' now owns the new object; the old one is freed
```

A `*T` satisfies both `&T` and `&*T`, and the declaration decides
which: the first borrows the object, the second the slot. Lending walks the
owning levels until one matches what the destination asked for and stops there,
so the same argument reaches a different depth for each declaration.

Field access and method calls reach through every pointer level until they find
the struct, so the levels never have to be spelled: `s.n` and `s.bump()` work
whether `s` is a `Box`, a `*Box`, or a `&*Box`. Only `*` is explicit,
for when the pointer itself is what you mean.

An `[T; N]` is a run of N elements laid out inline, exactly as a C `T[N]` is.
Its length is part of its type, so `xs.len()` is known where it is written, and
every index is checked against it: an index outside the array fails the run
rather than reading past the last element.

```
let xs: [int; 3];
xs[0] = 1;
let n: int = xs.len();
```

No `new`: the elements are the array, so a slot holding one holds them. An
array owns exactly what its elements do -- a run of ints owns nothing and
copies freely, while a run of `*T` owns each of them and moves rather than
copies.

A `Vec<T>` is what grows. Its length is in the value rather than in its type, so
it holds whatever has been pushed into it, and the block behind it is replaced
as that outgrows it.

```
let xs: Vec<int>;
xs.push(1);
let n: int = xs.len();
let first: &int = xs.at(0);
```

`at` lends the element where it sits rather than copying it out, so reading the
value spells the deref as `*xs.at(0)`, and an element that owns is reached
without moving it out of the vector. The borrow names the block, which a later
`push` may reallocate — as a C pointer into a vector's storage is invalidated
by a growth.

It owns its block and the live elements in it: both go when the vector does.
`Vec<T>` is an instantiation of a generic declaration rather than a kind of its
own, so what it is laid out as, what it owns, and what it refuses are the same
rules any struct answers.

Freeing an array frees what its elements own, however deep that goes. How many
elements are live is the count the block carries rather than anything its type
says, which is what lets one block type serve every length.

A struct is written as a literal, which names every field:

```
struct Point { x: int, y: int }

let p = Point { x: 1, y: 2 };
let q = Point { y: 2, x: 1 };   // the same value; order is the writer's
```

Every field is named because a partial literal would have to invent a value for
what it omits, and `&T` has none — a zeroed borrow names nothing, and nothing
tracks that. So a struct local is written as a literal rather than declared
empty and filled in: `let p: Point;` is refused, and the fields a literal cannot
spell are reached through what does spell them — `box v` for an owning field, a
borrow of a live value for a `ref`, `String::from` and `Vec<T>::new` for the
library's types. Those two live in `std`, so a unit that names them opens with
`import std;`; `str` and the other primitives need no import.

A generic struct names its arguments the way its type does, as
`Holder<int> { value: 4 }`.

An `if` or `for` header reads `Name {` as the block it opens rather than as a
literal, so a literal in one is parenthesized: `if (Point { x: 1 }).x == 1 { }`.

`box` allocates the value it is given, so what lands on the heap is spelled
where it is allocated rather than filled in afterwards:

```
let o: **Box = box (box Box { n: 0 });   // a heap slot that owns a pointer
```

A borrow cannot be boxed: a heap slot holding one would outlive what it borrows
with nothing tracking that. Freeing a slot that owns a pointer frees what the pointer names,
so a chain frees all the way down.

Memory is uniquely owned. `box v` yields a `*T` that exactly one slot owns and
that is freed where that slot goes out of scope; freeing an object frees what
its fields own. `&T` is how something is named without being owned, as a
child names its parent.

Binding a value either copies it or hands it over, and the type decides which. A
type copies exactly when nothing it holds transitively owns, which is derived
from the type rather than declared on it: a struct of `int`s copies, and so does
one holding a `ref`, while one holding a `*T` does not — the moment a field
owns, the struct does.

```
let b = a;    // copies if 'a' copies; otherwise 'a' is dead from here
```

Nothing marks the transfer, because the alternative was never available: two
slots cannot both own one object, so a binding that would duplicate an owning
value hands it over instead. Reading the old slot afterwards is the error, and
the diagnostic names the slot that no longer holds a value rather than the
binding that emptied it. Assigning something new to it revives it — deadness is
about what the slot holds, not a mark the name carries forever.

Duplicating an owning value is a call, since it allocates. That is an ordinary
method rather than a language rule: `String` declares `clone`, which returns a
second `String` and leaves the receiver live, and a struct that wants the same
declares its own.

```
let g = h.clone();   // 'h' is still live; 'g' owns a separate object
```

`&str` has no `clone`, since a reference already copies by assignment; what it
needs is `String::from()`, which changes the type rather than duplicating it.

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

A struct moves whole or not at all. Binding an owning field out of one — `g.b =
h.b` — is refused, since a half-moved struct would leave the rest of its fields
in a state the language says nothing about; binding the struct itself is how
ownership of what it holds changes hands.

Whether a slot is dead follows control flow the way a borrow's lifetime does. A
slot moved on one arm of an `if` is dead after the join, and moving in a loop
body is refused because the back-edge would move the same slot twice.

The rule behind all of this: **`*T` marks a slot that can free what it holds.**
That is a `let`, a struct field, a parameter, and a return type — each is where
a free may be emitted. A borrowed parameter is not one, so it takes `&T`; a borrow is an
address rather than an allocation, so it yields one too.

There is no reference count and no runtime liveness check: a `&T` whose
object has been freed dangles, exactly as a C pointer would.

What the compiler does catch is a borrow moved somewhere that outlives what it
names — returning a `ref` to a local, or storing one where the pointee dies
first. A borrow handed back by a call names the arguments it can actually come
from: each function's body says which of its parameters a returned borrow
reaches, and a call site is judged against those alone. Where no body says —
an `extern`, or a function still being summarized when it calls itself — the
result names every argument, so the shortest-lived one bounds it.

What is not caught is a borrow that outlives its pointee without ever being
moved: holding a `ref` while the object's owner frees it is undefined, as
holding a C++ reference across the owner's destruction is.

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
| Types | `int` (32-bit), `float` (32-bit), `bool`, `String` and characters named by `&str`, `[T; N]`, `Vec<T>`, structs, owning `*T`, borrows `&T` |
| Declarations | `let` with inferred or annotated type, `func`, `struct`, `impl`, `interface`, `module`. A struct local is written as a literal |
| Interfaces | `interface` names signatures and may take type parameters, `impl T as I<A>` supplies them and is checked at the declaration. `<T: I<A>>` bounds a type parameter, and a generic body is checked once against its bounds. `Self` is reserved, and names the type an `impl` block is for |
| Generics | Structs, the methods they own, and free functions. A method declares parameters of its own beside its owner's. A call infers its type arguments from what it is given, or names them as `id<int>(x)` |
| Functions | Parameters and returns of any type, structs by value, functions a type owns, recursion, forward references |
| Control flow | `if` / `else`, `for` in three forms, `break`, `continue`, `return`, nested blocks with shadowing |
| Operators | `+` `-` `*` `/` `%`, unary `-` `!`, `==` `!=` `<` `>` `<=` `>=`, `&&` `||`, unary `*`, field access, indexing `xs[i]` |
| Conversions | `int(x)` and `float(x)`; nothing converts implicitly |
| Assignment | `=`, and compound `+=` `-=` `*=` `/=` `%=` on any assignable target |
| Memory | Unique ownership, `box`, `&` borrows, binding that copies or transfers by type, scope-based free. A top-level variable may not own: nothing closes over it to free what it holds |
| Modules | `module` names the namespace a unit declares into, `import` the ones it may name. `std` holds `String` and `Vec<T>` |
| Externs | `extern func` declares a host body, bound by name at load |
| Comments | `// line` and `/* block */`, which do not nest |

Not yet implemented:

| | |
| --- | --- |
| Strings | No interpolation, no `substring` or case conversion |
| Arrays | Fixed length once allocated: no growth and no slice type. `Vec<T>` is what grows |
| Vectors | `new`, `push`, `at` and `len` only: no removal, no iteration, and no literal |
| Borrows | A returned borrow names a parameter or something it reaches; returning one that names a local is refused |
| Generics | A method's own type arguments are inferred from what it is given; there is no `v.method<int>(x)` to write them |
| Interfaces | An interface takes no type parameters of its own; no associated types, no compound bounds, and no dynamic dispatch |
| Operators | Bitwise |

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
