# Gab

Gab is a small, statically typed language that compiles through LLVM.

It is built around one idea: **ownership is spelled in the type, and the type
decides what a binding does.** A `*T` owns what it holds and frees it at the end
of its scope, a `&T` borrows and frees nothing, and a plain `T` copies. Nothing
marks a transfer at the site, because the destination's type already says
whether it takes ownership.

Gab is early. A resolved body lowers to MIR, and the LLVM emitter reads MIR to
produce an object file, which links against the core library the compiler emits
alongside it.

## The language

```
module game;

struct World {
    tick: i32,
}

struct Player {
    health: i32,
    world: &World,
}

impl Player {
    func is_alive(self: &Self): bool {
        return self.health > 0;
    }
}

func heal(p: &Player, amount: i32) {
    p.health = p.health + amount;
}
```

Field access reaches through every pointer level, so `p.health` works whether
`p` is a `Player`, a `*Player`, or a `&Player`. A function a type owns lives in
an `impl` block, and one whose first parameter is that type may be called on a
value: `p.is_alive()` is `Player::is_alive(p)`.

### Ownership

```
func consume(b: *Box): i32 { ... }   // owns; freed when the call ends
func peek(b: &Box): i32 { ... }      // borrows; the caller goes on owning

let c: *Box = a;   // takes 'a' over; 'a' is dead from here
peek(a);           // borrows; nothing changes hands
```

Memory is uniquely owned: `box v` yields a `*T` that exactly one slot owns, and
freeing an object frees what its fields own. A type copies exactly when nothing
it holds transitively owns, which is derived from the type rather than declared
on it.

There is no reference count and no runtime check. What the compiler does catch
is a borrow moved somewhere that outlives what it names — returning a `&T` to a
local, or storing one where the pointee dies first. That check follows control
flow, merging what holds on every path and going round a loop until its head
stops changing.

### Generics and interfaces

```
interface Countable {
    func count(self: &Self): i32;
}

impl Bag as Countable {
    func count(self: &Self): i32 { return self.n; }
}

func total<T: Countable>(x: &T): i32 { return x.count(); }
```

A bound is nominal: a type satisfies one by saying `impl T as I`, not by
happening to supply the right methods. A generic body is checked once where it
is written rather than at each call, and every instantiation is monomorphized,
so a call costs what a direct one does.

`Index<T>` is declared in the core, and `xs[i]` is written in terms of it.

### Runs of elements

An `array<T, N>` is a run laid out inline, exactly as a C `T[N]` is, and its
length is part of its type. A `slice<T>` is a run whose count is not in its
type: nothing holds one, and what names them is a `&slice<T>`, which carries the
address and the count side by side.

```
func total(xs: &slice<i32>): i32 {
    let sum: i32 = 0;
    for let i: i32 = 0; i < xs.len(); i = i + 1 { sum = sum + xs[i]; }
    return sum;
}

let a: array<i32, 2> = [1, 2];
let n: i32 = total(a);
```

`str` is the characters themselves, and nothing holds one. What names them is a
`&str`, two words like any other view. It reads as `&slice<u8>` through
`as_bytes`, which is the one primitive the rest of its surface is written over —
`len` is ordinary Gab code in the core.

`for` is the only loop keyword, and spells all three shapes:

```
for { ... }                     // forever, until a 'break'
for ready { ... }               // while the condition holds
for let i: i32 = 0; i < n; i = i + 1 { ... }
```

## Features

| | |
| --- | --- |
| Types | `i32`, `f32`, `bool`, `u8`, `usize`, characters named by `&str`, `array<T, N>`, elements named by `&slice<T>`, a run of them named by `raw<T>`, structs, owning `*T`, borrows `&T` |
| Declarations | `let` with inferred or annotated type, `func`, `struct`, `impl`, `interface`, `module`. A struct local is written as a literal |
| Interfaces | `interface` names signatures and may take type parameters, `impl T as I<A>` supplies them and is checked at the declaration. `<T: I<A>>` bounds a parameter, and a generic body is checked once against its bounds |
| Generics | Structs, the methods they own, and free functions, monomorphized per instantiation. A call infers its type arguments, or names them as `id<i32>(x)` |
| Control flow | `if` / `else`, `for` in three forms, `break`, `continue`, `return`, nested blocks with shadowing |
| Operators | `+` `-` `*` `/` `%`, unary `-` `!`, `==` `!=` `<` `>` `<=` `>=`, `&&` `\|\|`, unary `*`, field access, indexing `xs[i]`, `@size_of<T>()` |
| Conversions | `i32(x)` and `f32(x)`; `raw<T>(p)` reads a run as a run of another element. Nothing converts implicitly |
| Memory | Unique ownership, `box`, `&` borrows, binding that copies or transfers by type, scope-based free. `impl T as Destroy` names what a type runs as it ends |
| Modules | `module` names the namespace a unit declares into, `import` the ones it may name. `libc` binds the host's allocator, `testing` what a test asserts with |
| Comments | `// line` and `/* block */`, which do not nest |

Not yet implemented:

| | |
| --- | --- |
| Integers | Only `i32`, `u8`, `usize` and `f32` do arithmetic. The rest of the width matrix — `i8`, `i64`, `u32`, `f64` — is named but not built. `usize` is the host's pointer width rather than the target's |
| Strings | `as_bytes` and `len` only: no searching, no interpolation, no indexing |
| Collections | Nothing grows. `array<T, N>` is fixed and `&slice<T>` reads it; there is no vector and no map. `libc::malloc` takes a run, and nothing but the program that took it gives it back |
| Uninitialized locals | A local with no initialiser holds whatever its storage held; nothing zeroes it |
| Operators | Bitwise |

## Building the compiler

Requires CMake 3.16+, clang, and LLVM, which the backend builds its module
through. A distribution passes `-DBUILD_TESTING=OFF`.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build

cmake --install build --prefix ~/.local
```

Installing gives `bin/gabc` and `lib/gab`, which holds the core the compiler
links a program against. The two move together: `gabc` finds them beside itself,
or in `../lib/gab` once installed.

## Compiling a program

```sh
gabc -o hello hello.gab      # compile and link an executable
gabc -c -o hello.o hello.gab # compile only, writing hello.gabi beside it
./hello
```

A program is an ordinary native binary: there is no host API and nothing loads a
unit at runtime. `extern "C" func puts(s: &u8): i32;` declares a C function, and
the linker resolves it like any other symbol.

Linking runs `clang`, which knows where this system keeps libc and the objects a
binary starts in; `GABC_CC` names another. Compiling with `-c` needs neither.

A module is written across the files it is compiled from, and reaches another
module by importing it:

```sh
gabc -c -o lib.o lib.gab            # module lib, writing lib.gabi
gabc -o app app.gab -L .            # app.gab says 'import lib;'
```

`-L` names a directory of interfaces; the compiler also looks beside the source
and beside itself, so a module installed in `lib/gab` needs no flag. Each import
is resolved from the interface alone, and the object beside it is linked in.

## Testing

Warnings are errors. To build and run the suite under AddressSanitizer and
UndefinedBehaviorSanitizer:

```sh
cmake -S . -B build-asan -DGAB_SANITIZE=address,undefined
cmake --build build-asan
ctest --test-dir build-asan
```

The suite passes clean under both, so any sanitizer report is a regression.

A test written in the language is a `.gab` file, and where it lives says what it
claims. A case in `test/run` must compile, link and produce the right answers,
which `assert` states and a failure reports by line. A case in `test/error` must
be rejected, and a trailing `// error` block names every diagnostic it raises:

```
struct Node { self: Node }

// error
// :1:15: type error: struct 'Node' cannot contain itself: 'Node' contains 'Node'
```

The block is the whole expectation, so an error a case does not name fails it.
Both directories are globbed, so a new file runs without being registered.

## Contributing

Run `clang-format` before committing; the config is in `.clang-format`. Add a
test in `test/` for whatever you change, and check that it fails before your fix
as well as passing after it.
