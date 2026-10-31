# E1.1 — Is HandleContainer::m_ObjectID stable across respawn and streaming?

Seat: BIN (binary truth). Date: 2026-10-29. Binary sha256 `bc11ad79…` (base 0x400000,
no DYNAMICBASE, no .reloc). All addresses are VAs on this exact build. Every
statement below cites an address and the bytes/decompiled structure read from the
IDB this session (session `bin-identity`, opened via idb_open, closed save=false).

---

## 1. Mechanism — what sub_A200D0 and its callees actually do

The thunk at `0x9F0B50` is 15 bytes (`0x9F0B50`–`0x9F0B5E`; the project note says
"0x10 bytes" — that rounds up, the real span is 0xF):

```
9f0b50  push 1                      ; arg8 = allowAuthored = 1
9f0b52  push 0                      ; arg4 = allowWrite = 0
9f0b54  push ecx                    ; arg0 = obj (the ManagedObject in ECX)
9f0b55  mov  ecx, offset unk_4DD9824 ; this = HandleContainer registry
9f0b5a  call sub_A200D0
9f0b5f  retn
```

So `GetObjectID(obj)` == `sub_A200D0(registry=&unk_4DD9824, obj, allowWrite=0, allowAuthored=1)`.
Note the **thunk never mints**: `allowWrite=0`. The IDA prototype for sub_A200D0
says `signed __int32`, but the disasm `mov edx, edi / mov eax, esi` at
`0xA20123`/`0xA20126` returns a **u64 in EDX:EAX** (hi in EDX, lo in EAX). The
prototype is wrong; the u64 is real.

`sub_A200D0` (0xA200D0, 0xC9 bytes, ends `retn 0Ch` at 0xA2012D and 0xA20199):

1. `0xA200DE test ebx,ebx / jz` — obj==0 → return 0.
2. `0xA200EB call sub_D587F0(&unk_4DD601C)` — enter critical section (registry lock).
3. `0xA200F0 cmp byte_27F4AC6,0 / jz` — registry not initialised → return 0.
4. `0xA200FD cmp [ebp+arg_8],0 / jz` — **allowAuthored** branch (the thunk sets 1):
   - `0xA2010B call sub_A1E8D0(obj,0,0)` — **Path 1: inline 16-byte-record table**.
     `sub_A1E8D0` scans `this+16` (record base) up to `this+22` (u16 count); each
     record is 16 bytes and the match is `record[2] == obj` (object pointer stored
     at record offset +8). On hit it returns the record; the caller reads
     `record[0]` (lo) and `record[1]` (hi) at `0xA20114`/`0xA20116`. This is the
     **authored / persistent id** table.
   - `0xA20130 mov eax,[ebx+4]` then `test eax,3FFFFh / jz` and
     `test eax,10000000h / jz` — **Path 2 gate**: if `(obj+4 & 0x3FFFF) != 0` **and**
     `(obj+4 & 0x10000000) != 0`:
     - `0xA20147 call sub_A076A0(obj)` — **paged handle table** reader
       (`this=&byte_4DD5FA0`). `idx = (obj+4 & 0x3FFFF) - 1`;
       `dir = *(this+0x98)`; page desc `= dir[(idx>>14)*3]` (12-byte stride);
       entry stride `0x14` (20 bytes); returns u64 = `entry+0xC` (lo) / `entry+0x10` (hi).
       `0x4000` slots/page, 16 pages max.
5. `0xA20153 lea ecx,[esi+4] / call sub_A1EC40(obj)` — **Path 3: pointer→id map**
   (`sub_A5B4F0` over `this+4`). Returns u64; if nonzero, returned at `0xA2015F or eax,edi / jnz`.
6. `0xA20163 cmp [ebp+arg_4],al / jz` — **allowWrite** branch (the thunk sets 0, so
   the read path never reaches here):
   - `0xA20173 lock xadd [this+8],1` then `inc esi` → **Path 4: mint**. `this+8` ==
     `&unk_4DD9824 + 8` == `0x4DD982C`. The new id is `counter + 1` (hi=0).
   - `0xA2017F call sub_A1F2F0(obj, new_id, 0, 0, 0)` — **RegisterId** (below).
7. Return u64 in EDX:EAX.

Callees that matter:

- **`sub_A1C9A0` (MintId)**, `0xA1C9A0`: `return _InterlockedIncrement(this+2)`
  where `this=&unk_4DD9824`, so it is the same counter `0x4DD982C`. Monotonic +1.
- **`sub_A1F2F0` (RegisterId)**, `0xA1F2F0`: first `if (sub_A1F160(id_lo,id_hi))`
  → **if the id already maps to an object, return early (no rewrite)**. Else a CAS
  loop at `0xA1F376` sets `obj+4 |= 0x10000000` ("id registered" flag), then
  `sub_A1EAE0` (id→obj map) and `sub_A1DCF0` (obj→id map, plus
  `CAS obj+4 = (id_hi << 18) | (obj+4 & 0xFF03FFFF)`).
- **`sub_A0BBF0` (HandleContainer::insert)**, `0xA0BBF0`: **the only writer of
  `entry+0xC`/`entry+0x10`** in the paged table (writes at `0xA0BD40`/`0xA0BD46`
  and `0xA0BD65`/`0xA0BD68`). `id = arg` if nonzero, else **minted** via
  `sub_A1C9A0` (`0xA0BCBF`). Then at `0xA0BD81`–`0xA0BDAC` it registers the entry
  into `registry+0xA4` (`sub_A097D0`) **only when the id is in [0xF0000000,0xF8000000)**.
  That range test is inlined as constants: `(v7_lo >= 0xF0000000) && (v7_hi == 0)`
  AND `(unsigned)(v7 + 0x10000000) <= 0x7FFFFFF` (i.e. `v7_lo <= 0xF7FFFFFF`).
- **`sub_A51910` (serialiser)**, `0xA51910`: at `0xA51CCA` it runs the *same*
  range test, and when the id is in the minted range it **zeroes the id**
  (`LABEL_80` at `0xA51CCC`: `v45=0,v25=0,v24=0`). Authored ids (< 0xF0000000)
  are written through. **Consequence: minted ids are not serialised; only authored
  ids persist across save/load.**

Which path is hit for each object kind:

| object kind | path | evidence |
|---|---|---|
| (a) authored NPC | **Path 1** (sub_A1E8D0 authored table) or the paged table with a non-minted id | thunk `push 1` enables allowAuthored; `record[2]==obj` keyed on the object pointer |
| (b) player | **Path 1 / paged table** — the player is an authored, serialised entity | serialiser keeps ids `< 0xF0000000`; player id is authored, not minted |
| (c) runtime-minted entity | **Path 2** (paged table via `obj+4` handle index) | insert with `id==0` mints via sub_A1C9A0; entry lives in the paged table; read path gated on `obj+4 & 0x10000000` and `obj+4 & 0x3FFFF` |

---

## 2. Stability analysis (from code only)

**Q: Is an id ever rewritten for the same object?**
No. Three independent reasons, all read this session:
- `sub_A1F2F0` (RegisterId) returns early at `0xA1F30A` when `sub_A1F160(id)` finds
  the id already bound to an object — it never overwrites an existing id→obj entry.
- `sub_A0BBF0` is the only writer of paged-table `entry+0xC`/`entry+0x10`, and it
  only writes for a **newly allocated** free-list entry (the "else" branch after
  the existing-entry lookup `sub_A07560` at `0xA0BC3B`).
- `sub_A200D0` via the thunk runs with `allowWrite=0`, so the read path never
  reaches the mint at `0xA20173`.

**Q: Is the mint counter monotonic?**
Yes. `sub_A1C9A0` = `_InterlockedIncrement(this+2)` (`0xA1C9AF`), a `lock xadd`.
The only cross-reference to `0x4DD982C` is the static initialiser at `0x1E323C1`
(`mov dword_4DD982C, 0F0000000h`), which runs once at process start (the
immediately-following `0x1E323D5 call _atexit` registers the destructor). No reset
or decrement exists. Minted ids are strictly increasing for the process lifetime.

**Q: Can an id be reused after despawn/respawn?**
- **Minted ids: NO.** Monotonic counter, never reset (above). A despawned entity's
  minted id is never reissued.
- **Authored ids: UNVERIFIABLE from these functions alone.** The id is a *parameter*
  to the spawner (`sub_A0BBF0`'s `a2`; if `a2==0` it mints instead). Whether a
  respawned authored NPC re-receives the same authored id is decided by *whoever
  calls* `sub_A0BBF0` with a nonzero id — that caller is not in this dispatch's
  budget. **Artifact that would settle it:** the callers of `sub_A0BBF0` (xrefs_to
  `0xA0BBF0`) and what they pass as `a2` — a future BIN pass. The design intent is
  readable though: `sub_A51910` (serialiser) **zeroes minted-range ids** and keeps
  authored ids, i.e. authored ids are the persistent identity and minted ids are
  session-transient.

**Q: Is there a generation counter anywhere?**
UNVERIFIABLE as "a generation counter". The code has no such field. The closest
artefacts are: the handle index `obj+4 & 0x3FFFF` (a *slot index*, not a
generation) and the `obj+4 & 0x10000000` "registered" bit (a boolean, not a counter).

---

## 3. Detectability — telling that a cached id now refers to a different object

`obj+4` field layout (all bits verified against the CASes in `sub_A1F2F0` and
`sub_A1DCF0`):

| bits | meaning | set by |
|---|---|---|
| 0..17 (`0x3FFFF`) | paged-table handle index | insert (`sub_A0BBF0` via `sub_A0BA60`) |
| 28 (`0x10000000`) | "id registered" flag | `sub_A1F2F0` CAS at `0xA1F376` |
| 18..31 | `id_hi` (hi 32 bits of the 64-bit id) | `sub_A1DCF0` CAS `(id_hi << 18) \| (obj+4 & 0xFF03FFFF)` |

Cheapest safe checks, in order:

1. **Resolve id → object and compare the pointer.** `sub_A1F160(id_lo, id_hi)`
   (`0xA1F160`) is the reverse lookup (authored table → paged table via
   `sub_A076F0` → map via `sub_A1DC00`). Cache the object pointer alongside the id;
   on each use, re-resolve and compare. Mismatch/null ⇒ stale.
2. **`obj+4` flags.** `obj+4 & 0x10000000` must still be set, and
   `obj+4 & 0x3FFFF` must equal the handle index you cached at first sight. If the
   index changed, the object was re-inserted (new identity).
3. **Class tag.** `sub_A1D0D0(obj)` (`0xA1D0D0`) returns
   `*(*(vtable+0x14)(obj) + 0x14)` — the "ClassID" the serialiser reads
   (`0xA51967`/`0xA51C4C` label "ClassID"). Compare it to the value cached at first
   sight to catch an id re-bound to a *different type*.

There is **no generation counter to read** — see §2.

---

## 4. Experiment procedure (one machine, a human runs it)

Scenario: kill a guard, wait for respawn/stream-out+stream-in, re-acquire the same
guard, compare the id and the object pointer. You do not need to read the code.

1. Load any save standing next to a patrolling guard in a city (Havana, Kingston,
   or Nassau). Note the guard's name/appearance.
2. In the debug UI, snapshot the guard: log `id_lo:id_hi` (from GetObjectID at
   `0x9F0B50`), the object pointer, `obj+4` as hex, and the class tag
   (`sub_A1D0D0` result).
3. Kill the guard. Wait for the body to be fully gone (corpse despawns / looted).
4. Turn away and walk far enough that the guard's area streams out (cross a zone
   boundary; the audio/music cell change is a useful marker). Wait 60 s.
5. Walk back to the same spot. The engine either respawns the guard or leaves the
   area empty.
6. When the guard reappears, snapshot it again: `id_lo:id_hi`, object pointer,
   `obj+4`, class tag.
7. Log all three snapshots plus a timestamp to `ops/evidence/E1.1/respawn-<t>.log`.
8. Repeat steps 2–7 for **10 distinct guards** and **5 stream-out/in round-trips**
   on the same guard (kill → leave → return, no kill the 2nd–5th time).
9. Record, for each re-acquisition: (id unchanged / id changed) × (pointer
   unchanged / pointer changed) × (class tag unchanged / changed).
10. Attach the logs and fill in the PASS/FAIL table below.

PASS/FAIL table (E1.1):

| # | observation | verdict | meaning |
|---|---|---|---|
| 1 | same id, same pointer, same class tag after respawn | **PASS** | ids are stable; m_ObjectID can be a cross-machine identity for authored NPCs |
| 2 | same id, **new** pointer | PASS-with-caveat | id is authored/stable; the pointer is a session handle — always re-resolve via sub_A1F160 |
| 3 | **new** id (minted range `≥ 0xF0000000`), new pointer | **FAIL** | minted ids are not stable across respawn; authored-vs-minted split is mandatory |
| 4 | id moved into `[0xF0000000,0xF8000000)` after respawn | FAIL | the guard was re-minted, not re-authored — authored id was not preserved |
| 5 | class tag changed for the same id | FAIL | id reused for a different type — alias layer must include classTag |
| 6 | `obj+4 & 0x3FFFF` changed with same id | FAIL | handle re-inserted; index is not a stable identity |

Expected failure modes and what each means:
- **Minted-range id on respawn (rows 3/4):** confirms §2 — only authored ids
  survive; wire format must send only authored ids and treat minted ids as
  session-local.
- **Id reused for different type (row 5):** an id pool or re-mint exists for
  authored slots; EntityRef needs a classTag to disambiguate.
- **Handle index drift (row 6):** the paged table is a storage detail, not identity;
  never use `obj+4 & 0x3FFFF` as a wire id.

---

## 5. Wire-format consequence

Given §2: **only authored ids (< 0xF0000000) are safe to send between machines.**
Minted ids are session-local, monotonic, and explicitly not serialised
(`sub_A51910` zeroes them). Therefore:

- `EntityRef` **must** carry `epoch` (world/mission load counter to invalidate stale
  refs across a load), `objectId` (u64 — but only send it when `< 0xF0000000`), and
  `classTag` (crc32(class name)&0xFFFF, or the `sub_A1D0D0` class id) — exactly as
  `PROGRAM-PLAN.md` §2.1 already sketches. That plan's shape is correct; the
  refinement E1.1 adds is: **the `objectId` field is only meaningful for authored
  entities; minted entities must not use it.**
- Cheapest alias scheme if authored ids prove unstable (rows 3–6 FAIL): maintain a
  per-machine alias table `(epoch, classTag, authoredId) → local object pointer`,
  refreshed by `sub_A1F160(id)` on every event. For runtime-minted entities (which
  have no authored id), assign a plugin-side identity from `(epoch, classTag,
  firstSightTick)` and resolve by pointer, never by engine id. If an authored id is
  ever observed to be re-minted (row 4), add the class tag as a mandatory
  discriminator on both sides before trusting the id alone.

---

## 6. Facts block

Flat JSON in the schema of `audit/ida/ida_findings.json` — see the appended keys in
that file (prefix `objectid.*`). The full list is:

```json
{
  "objectid.thunk": "0x9F0B50 -> sub_A200D0 (15 bytes 0x9F0B50..0x9F0B5E); push1=allowAuthored, push0=allowWrite, push ecx=obj, ecx=&unk_4DD9824",
  "objectid.core": "sub_A200D0 0xA200D0 size 0xC9; returns u64 EDX:EAX (IDA proto 'int' is wrong; see mov edx,edi/mov eax,esi @0xA20123)",
  "objectid.path1_authored": "sub_A1E8D0 0xA1E8D0 inline 16B-record table at this+16 (count u16 at this+22); match record[2]==obj; id = record[0](lo)/record[1](hi)",
  "objectid.path2_paged": "sub_A076A0 0xA076A0 this=&byte_4DD5FA0; idx=(obj+4 & 0x3FFFF)-1; dir=*(this+0x98); page desc 12B stride; entry stride 0x14; id=entry+0xC(lo)/entry+0x10(hi); 0x4000 slots/page, 16 pages",
  "objectid.path3_map": "sub_A1EC40 0xA1EC40 -> sub_A5B4F0(this+4, obj); returns u64 id",
  "objectid.path4_mint": "0xA20173 lock xadd [this+8] + inc; counter = 0x4DD982C; new id = counter+1 (hi=0)",
  "objectid.mint_counter": "0x4DD982C init 0xF0000000 at 0x1E323C1 (static init, atexit at 0x1E323D5); sub_A1C9A0 0xA1C9A0 = _InterlockedIncrement(this+2); monotonic, no reset",
  "objectid.mint_bound": "0x4DD9830 = 0xF8000000 written at 0x1E323CB but NEVER read; the [0xF0000000,0xF8000000) range is inlined as constants in sub_A0BBF0 (0xA0BD97) and sub_A51910 (0xA51CCA)",
  "objectid.register": "sub_A1F2F0 0xA1F2F0; early-return if sub_A1F160(id) found (0xA1F30A); CAS obj+4 |= 0x10000000 (0xA1F376); then sub_A1EAE0 (id->obj) + sub_A1DCF0 (obj->id)",
  "objectid.insert": "sub_A0BBF0 0xA0BBF0 = HandleContainer::insert(u64 id, owner); id==0 -> mint via sub_A1C9A0 (0xA0BCBF); ONLY writer of entry+0xC/+0x10 (0xA0BD40/0xA0BD46/0xA0BD65/0xA0BD68); registers into registry+0xA4 (sub_A097D0) only for minted range",
  "objectid.serializer": "sub_A51910 0xA51910; zeroes minted-range ids at 0xA51CCC (LABEL_80), serialises authored ids < 0xF0000000",
  "objectid.obj4_layout": "bits 0..17 handle index (0x3FFFF); bit 28 (0x10000000) = 'id registered' flag (sub_A1F2F0 CAS); bits 18..31 = id_hi (sub_A1DCF0 CAS (id_hi<<18)|(obj+4 & 0xFF03FFFF))",
  "objectid.resolve_id_to_obj": "sub_A1F160 0xA1F160(id_lo,id_hi) -> obj pointer (authored table -> sub_A076F0 paged -> sub_A1DC00 map)",
  "objectid.class_tag": "sub_A1D0D0 0xA1D0D0 = *(*(vtable+0x14)(obj)+0x14); serialiser 'ClassID' at 0xA51C4C",
  "objectid.no_generation_counter": "true (verified: no generation field in sub_A200D0/sub_A1F2F0/sub_A0BBF0/sub_A1DCF0)"
}
```

---

### Contradictions vs the project's existing notes

1. **Thunk size.** Notes say "0x10 bytes"; actual span is **0xF** (`0x9F0B50`–`0x9F0B5E`).
2. **"bound at 0x4DD9830".** The value `0xF8000000` is written there but **never read**;
   the mint range is enforced by inlined constants, not by reading that global. The
   note is address-correct but implies an enforced runtime bound that does not exist
   in code.
3. **No contradiction, but a warning:** the IDA prototype for `sub_A200D0` is
   `signed __int32` while the function genuinely returns u64 in EDX:EAX; any code
   generation off the prototype will truncate. The u64 is the truth.

The core claim — `GetObjectID` thunk → `sub_A200D0`, counter at `0x4DD982C`,
`0x10000000` registered bit at obj+4, paged table stride 0x14 with +0xC lo / +0x10
hi, minted range `[0xF0000000,0xF8000000)` — is **confirmed**.
