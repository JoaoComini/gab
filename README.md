# Gab

Gab is a small, statically typed language that compiles to native binaries using the LLVM backend.

It is built around a core principle: **ownership is spelled in the type, and the type decides what a binding does.**
A `*T` owns what it holds and frees it at the end of its scope, a `&T` borrows and frees nothing, and a plain `T` copies. Memory safety is guaranteed by the compiler checking control flow for valid borrows.

---

## Language Syntax
Gab uses a clean, functional syntax similar to C or Swift.

```gab
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
```

## Building and Running
Gab requires CMake, Clang, and LLVM.

### Build
```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

### Compile and Run
```sh
gabc hello.gab     # Compile and link
./hello
```

### Compilation Notes
*   `gabc hello.gab` compiles and links the source into a native binary.
*   `gabc -c hello.gab` compiles only, outputting the object files.
*   Modules are imported using `gabc app.gab --import lib=lib.gabi`.

## Testing
Warnings are treated as errors. To verify the compiler and runtime against memory safety issues:

```sh
cmake -S . -B build-asan -DGAB_SANITIZE=address,undefined
cmake --build build-asan
ctest --test-dir build-asan
```

---
**Contributing**
Follow the `AGENTS.md` guide. Always run `clang-format` before committing, and ensure every change is covered by a new test that fails before the fix.