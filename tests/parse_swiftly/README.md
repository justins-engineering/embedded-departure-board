# Swiftly parser tests (host)

```sh
./tests/parse_swiftly/run.sh
```

Runs from anywhere, needs no board, no Zephyr build and no network. Exits 0
on pass, 1 on a failed assertion, 2 if something could not be built or the
config could not be read.

**These are not wired into anything.** No CI workflow, no pre-commit hook,
nothing runs them but a person choosing to. `.github/workflows/build.yml`
builds the firmware and does not call this. Treat "the tests exist" and "the
tests ran" as separate claims, because only the first is true by default.

## Why a plain host build and not Twister

The value here is that it compiles the **real** `app/src/json/*.c` with the
**same type layout the firmware gets**, using stub Zephyr headers plus
`-fshort-enums`. That combination keeps `parse_swiftly.c`'s own
`BUILD_ASSERT(sizeof(jsmntok_t) == 8)` live and true. A Zephyr or native_sim
build brings its own flags and does not obviously preserve that packing, and
a harness that silently exercises a differently-laid-out parser is worse than
no harness at all — it reports green about code that is not the code shipped.

Move to Twister when a test genuinely needs the kernel. Nothing here does.

## Four things that must survive any change to this directory

1. **`-fshort-enums`.** `jsmntok_t` is an enum plus three `int16_t`. On ARM
   the enum is one byte and the struct packs to 8; a default host build makes
   the enum four and the struct grows. Without the flag the build *fails* on
   the assert above — and the tempting fix, stubbing `BUILD_ASSERT` to
   nothing, converts a loud failure into a silent lie.
2. **`zephyr/kernel.h` keeps `BUILD_ASSERT` as a real `_Static_assert`.** It
   is the thing that catches (1). Do not make it a no-op.
3. **The `CONFIG_*` values come from `app/sign.conf`.** `run.sh` reads them
   out of that file rather than repeating them, and stops if one is missing.
   `sign.conf` is authoritative; the generated `app.config` in a build
   directory is downstream of it and drifts.
4. **Every case asserts the return code *and* `routes_size`.** The second is
   what catches stale state surviving a fetch — the defect where an empty
   response left the previous fetch's routes on the sign while reporting
   success.

## What is covered

| case | asserts |
| --- | --- |
| populated response | ret 0, streak 0, `routes_size` 1 |
| degenerate body (`{}`, under two tokens) | ret 2 — a successful fetch with no departures, not a failure |
| empty `predictionsData` after a populated fetch | ret 0 and `routes_size` **0**, so the sign blanks rather than redrawing stale times |
| captured real payload | ret 0, `routes_size` 3, every `min` -1, nothing drawn |
| either spelling of a route | id and short name both resolve to the same display |
| short name colliding with another route's id | first matching entry wins — the documented cost of accepting both fields |
| wrong direction | no match, direction still gates |

`fixtures/stop1670-20260814T024135Z.json` is a real end-of-service response
captured 2026-08-14T02:41:35Z. It is the ground truth for what "no
departures" actually looks like on this endpoint, and it is not what anyone
guessed: every route is still listed, with its destinations, and
`"predictions": []` sits *inside* each destination.

## `classify_pre`

`run.sh` also builds `classify_pre` — the same cases without `-DFIXED`,
showing the caller-side classification as it behaved before the
no-departures translation was restored. It reverts **only** the caller side.
The parser fixes live in `parse_swiftly.c` itself and are not behind that
macro, so seeing the pre-fix parser behaviour needs a checkout before
`12ac1aa`.
