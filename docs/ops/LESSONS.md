# Lessons that cost a session

Short, specific, and each one cost real time. They exist so the next agent does
not re-derive them the same way.

---

## 1. `#pragma pack(1)` does not shrink an over-aligned type

`HookMath::Transform` and `HookMath::Vector4` are `__declspec(align(16))`. In
`PlayerState`:

| field | hand count | compiler |
|---|---|---|
| `matrix` | 28 | **32** |
| `heading` | 104 | **112** |
| `valid` | 544 | **556** |
| `sizeof` | 545 | **560** |

Two people counted 545 independently and agreed. The game sends 560, and
`net.cpp` dispatches on an exact length, so every packet was dropped with no
error. **Never hand-count a packed struct that contains an over-aligned type.**
Ask the compiler:

```powershell
cl /nologo /std:c++17 /EHsc /I <src> probe.cpp
# probe.cpp: printf("%zu\n", sizeof(PlayerState)); offsetof() for every field
```

## 2. A self-test branch that `return`s early can disable production paths

`EntitySyncTick` had:

```cpp
if (s_selfTest) { ...Apply*...; return; }     // returned before this
if (g_entHost)  { ... if (Net::GetEntities(in)) ProcessKillEvents(in); }
```

With `EntitySelfTest=1` the game silently ignored every incoming entity packet.
The symptom is indistinguishable from "the peer is not sending". Any diagnostic
that returns early must return *after* the receive/dispatch work, or the
diagnostic changes the thing it is meant to observe.

## 3. Some "log lines" never reach the log file

`[Net] up: bound:47801 -> â€¦` is written with `OutputDebugString`, not to
`phase1_log_<pid>.txt`. A watcher keyed on it waits forever. The file has
`[Net] init=1  (bound/peer line goes to OutputDebugString)`; key on that.

Rule: before writing a watcher, grep the plugin for the exact line and confirm
which sink (`Log`/`L` vs `dbg`) emits it.

## 4. A restart must not re-seed a watermark

The message watcher seeded its "only newer than now" watermark on every restart.
A reply that arrived while it was down was then classified as old news and never
woke the orchestrator â€” a 40-minute delay that looked like the seat had gone
quiet. Seed once, at the very beginning, and persist the watermark.

## 5. Two sockets on one UDP port "succeed" on Windows

With `SO_REUSEADDR`, a second bind to the same port **succeeds**, and the **first**
binder keeps receiving. Measured: `ops/tools/reuseaddr_test.py` (now deleted; the
record is in `DECISIONS.md` D-018). Any "if bind fails, I am the second instance"
logic is dead code on this platform. Use an explicit marker.

## 6. A hand-written struct mirror is a claim, not a fact

`ref/AC4Coop/src/Scimitar/*.h` mirrors engine layouts. Some of it is right (the
offsets the puppet drive uses), some is stale (`netstate.h:107` documented the
disproved ship-HP offset for weeks). When a struct matters to the wire, verify it
against the compiler and the IDB, and mark every field `verified` or `unverified`.

## 7. PUSHAD order: `this` (ECX) is at +24, not +16

In a stub that does `pushad`, the saved registers are laid out in x86 push order
from the *lowest* address: EDI +0, ESI +4, EBP +8, ESP +12, **EBX +16, EDX +20,
ECX +24, EAX +28**. Reading `this` from +16 logs EBX — which looks plausible in a
log line and is wrong by one register. NET caught this in the combat observer after
my spec said +16; the code uses +24.
