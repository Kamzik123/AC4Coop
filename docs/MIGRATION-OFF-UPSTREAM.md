# Migration off the upstream skeleton

Goal: the repository contains **nothing** written by the upstream author — only our own code, with the
engine facts regenerated from our decompilation (`C:\Users\Administrator\ac4-decomp`).

Rule for every file: re-derive the layout/behaviour from the decompilation (and confirm live where the
code path matters), then write our own version. Never copy the upstream text; keep only what the engine
requires (offsets, sizes, virtual slots), which are facts about the binary.

## Status

| area | upstream lines | status |
|---|---|---|
| project files (.sln/.vcxproj/.filters/.gitignore) | ~430 | **to replace** (trivial, regenerate from our source list) |
| `src/Hook.{h,cpp}` | ~97 | **DONE** — rewritten from scratch, build verified |
| `src/HookMath.{h,cpp}` | ~645 | **header DONE** (rewritten; build verified). `.cpp` pending — needs a live positional check |
| `src/Scimitar/**` (class declarations + small .cpp) | ~5,100 | **to replace incrementally** — only the headers a change actually touches |
| `src/phase1.cpp` | 1,225 of 4,941 | ~75% ours; remaining early scaffolding rewritten opportunistically |
| `src/net.*`, `netstate.h`, `ship_sync.cpp`, docs | ours | keep |

## Order of work

1. **Project files** — regenerate `.sln/.vcxproj` from our own file list. Removes ~430 upstream lines and
   makes the build independent of their project layout.
2. **Hook** — reimplement the three call helpers we actually use (`CallClassMethod`, `CallCdeclMethod`,
   `CallClassMethod<void,...>` variants) as direct function-pointer casts; no framework needed.
3. **HookMath** — reimplement only the operations used (matrix from/to quaternion, vector ops, transform
   composition). Verify by comparing outputs against the live engine on a known entity.
4. **Scimitar headers** — replace one at a time, starting with the smallest and hottest:
   `ObjectShape`, `CollisionFilterInfo`, `Components/*`, then `Entity`/`World`, then `Human`/`AIActor`.
   For each: dump the vtable + field offsets from the decomp (or live), keep our own names, and compile.
   Do not attempt all at once; a header is only replaced when a change touches it.
5. **phase1.cpp** — as areas are touched, rewrite the early scaffolding rather than editing it.

## Verification for every replacement

- Build clean.
- The specific feature that uses the file still works live (e.g. after replacing `CollisionFilterInfo`,
  re-run the corpse-collision test).
- No upstream text remains: `git grep` for distinctive upstream identifiers/comments.

## Definition of done

`git log` upstream commits remain only as history; every file in `src/` is regenerated from our
decompilation; `CREDITS.md` reflects that the initial skeleton has been fully replaced.

## Live verification (2026-10-18)

`Hook.{h,cpp}` and `HookMath.h` were exercised end-to-end by the real kill path on the rewritten files:

````
[Kill] SELF-TEST peer event objId=4026541036 -> ent=358A9440
[KF] SetLife(-1) rc=0  life 36 -> 65535
[KF] is-alive service=FCB0B828 +4 00000000 -> 00001000
[KF] after : alive=0
[KF] death clip + collision layer -> Disabled(0x29)
````

User-confirmed: guard died, stayed down, no invisible wall. Position read from the matrix was correct
(`35.4,-73.7,3.4`), so the HookMath conversions are behaviourally equivalent.

Status: Hook done + verified; HookMath.h done + verified; HookMath.cpp still pending (next).

## Include-graph audit (2026-10-18)

Every Scimitar header was checked against every `.h`/`.cpp` include in the project:
- 35 headers, **34 referenced**, **1 dead** (`BipedComponent.h`, 21 lines) — deleted.
- Consequence: pruning is not a shortcut. The `Scimitar` block (~5,100 lines) is all load-bearing and has
  to be **replaced**, not removed. Replacement stays incremental, in dependency order:
  `ObjectShape` -> `CollisionFilterInfo` -> `Components/*` -> `Entity` -> `World` -> `Human`/`AIActor`.
