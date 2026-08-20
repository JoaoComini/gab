# Agent Instructions

Guidance for AI agents working in this repository. Read it before the first
edit, not after a gate fails.

## When a Gate Fails

Stop and report. Do not work around a failing build, a failing test, or a
sanitizer report by disabling the check, narrowing the assertion, or marking
the test skipped. The gate found something; that is its job.

If you disagree with a gate, say so and let the user decide.

## Before Any Edit

Read the code you are about to change, and the code that calls it. This
codebase is small enough that this is cheap, and its invariants live in
comments next to the code rather than in a design document.

Two questions worth answering before editing:

- **Which stage owns this?** Source flows lexer → parser → AST/resolver →
  codegen → VM. A change in the wrong stage usually compiles and usually is
  wrong. A syntax error belongs in the parser, a type error in the resolver
  (`src/ast/ast.c`), a wrong instruction in codegen, a wrong result in the VM.
- **Does the language already express this?** Top level accepts only
  declarations, so statements must live in a function body. Check what parses
  before assuming a shape is valid.

## Testing

**Add or find a failing test before fixing a bug.** Run it and observe the
expected failure before making the implementation edit. A test written after
the fix proves only that the code does what it does.

**Test in `test/`, never with a scratch binary.** Do not compile a throwaway
program against `libgab.a` to probe behaviour. Write it as a test, run it to
green, and leave it as a regression guard. Ad-hoc binaries also go stale
against a rebuilt library and will lie to you.

**Verify a test is not vacuous.** For a test asserting an optimization or a
guard, disable the thing it covers, confirm the test fails, then restore. A
passing test proves nothing until you have seen it fail.

Helpers live in `test/support/run.h`: `test_run_int`, `test_run_float`,
`test_run_bool` for behaviour, `test_compiles` for programs the resolver must
reject, `test_codegens` for those a rule in codegen rejects, `test_run_status`
for runtime traps, and `test_compile` with `test_count_opcode` /
`test_find_opcode` for asserting on emitted code. Use them rather than
rebuilding a harness.

Register a new test file in `test/CMakeLists.txt`.

### What belongs where

Tests are named for the behaviour they check, not the file they cover, because
most run source text through the whole pipeline.

Assert on behaviour by default. Assert on emitted instructions only for claims
behaviour cannot make — a folded constant, a reclaimed register, a multi-slot
copy that stays one instruction. Those belong in `test/vm/codegen_test.c`.
Never assert on specific register numbers; assert relationally, that one
instruction reads what another wrote.

## Comments

The code must read itself. Comment only when the logic is convoluted or
obscure, or when an important behaviour depends on that specific line: an
ordering whose breach is silent, a guard for a case not visible locally, a
load-bearing literal or encoding choice.

Do not narrate mechanics, restate a signature, or label a section the name
already describes.

**Comments and test names describe the present, never the repository's
history.** No "the regression", "used to", "did in fact", "still works", "no
longer". Those describe a state that was true for one afternoon; once fixed
they document nothing that exists. Whether a behaviour has broken is a
developer's problem at the time it breaks — the test's job is to state what
should be true.

## Before Committing

Run these, in this order:

```sh
cmake --build build && ctest --test-dir build
clang-format -i <files you touched>
```

`clang-format` reads `.clang-format` at the repository root. Run it from the
root: a wrong working directory makes it fail silently on relative paths.

Keep it scoped to the files you touched. Some files carry pre-existing
formatting drift, and a blanket sweep buries a small change behind hundreds of
reformatted lines. A whole-tree format is its own commit.

For changes to reference counting, the arena, the stack, or anything decoding
instructions, also run the sanitized build:

```sh
cmake -S . -B build-asan -DGAB_SANITIZE=address,undefined
cmake --build build-asan && ctest --test-dir build-asan
```

The suite passes clean under ASan and UBSan, so any report is a regression
rather than pre-existing noise.

### Commits

One line, no body, no trailers, no prefix. Start with a capital letter. State
what the change makes true rather than what you did:

```
'-x' negates, and a negated literal costs no instruction
'a >= b' answers what it asks, on ints and floats alike
A wide copy is one instruction, and a narrow one is not widened
A script that divides by zero fails its run instead of killing the host
```

No `feat:` / `fix:` / `chore:` tags. Nothing here consumes them — there is no
changelog generation and no release tooling — and a sentence that says what
changed already says what kind of change it is. The prefix would only spend the
first word, the one that survives truncation in `git log --oneline`, repeating
what the rest of the line makes obvious.

A line starting with a quoted token keeps its lowercase quote: `'-x' negates`,
not `'-X' negates`.

Amend rather than stacking commits while iterating on the same change. Several
commits doing one job should be one commit.

Do not commit or push unless asked.

## Mechanical Rewrites

Use a tool, not hand-editing, for anything repetitive — and verify the result.
Regex and `awk` splices are error-prone here: they leave continuation lines
misaligned, and an off-by-one in a line-range delete silently orphans a
fragment that still compiles. After any scripted edit, read the diff before
trusting it, and confirm the build actually succeeded before reading test
output. A stale binary reports success for code that no longer builds.

## Scope

Do the work asked. If you find an adjacent problem, say so and let the user
decide whether to widen the scope.

Two things the language does not have, which are easy to assume: **loops**
(no `while` or `for`) and **comments** (the lexer does not recognise them).
See the README's feature table before assuming a construct exists.
