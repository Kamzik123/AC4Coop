# A1 â€” death animation context (why every corpse plays the same clip)

Status: carded 2026-10-29. Owner: AI/anim seat. Depends on: NET's false-death fix.

---

## 1. The problem

Every replicated death plays one clip:

```cpp
// phase1.cpp:4548 â€” "Play the death clip recorded from a real kill (alias 1818493434, captured liveâ€¦)"
ac->SetTargetAction((int)1818493434u, 0, 0, 0xFFFF, ACTLAYER_PRIMARY);
```

That alias was captured from **one** real kill and is now applied to every victim, whatever
killed it. The operator sees the same collapse for an air assassination, a ledge pull, a
finisher and a pistol shot.

The reason is structural: `KillGuardEntity` calls `SetLife(-1)` directly (that is the
working kill â€” D-016) and then *chooses* a clip. It bypasses the engine's reaction
system, which is the thing that normally selects the death animation from the damage
context. So we are not "missing" an animation; we are overriding the chooser.

## 2. What already exists (verified in the source, no RE needed)

| piece | where | status |
|---|---|---|
| set an arbitrary entity's action | `ActionComponent::SetTargetAction(alias,0,transition,0xFFFF,layer)` | used on the puppet and on the victim today |
| read an entity's current action | `ActionComponent::GetCurrentAction(&mirrored, channel, layer)`; alias at `action+0x08` | already read for **every** guard in `EntitySyncTick` (`es.actionAlias = *(u32*)(act+8)`) |
| the wire already carries an action | `EntityState.actionAlias / actionChannel / actionLayer` | present since proto 9 |
| the player's own action | capture hook, `s.actionAlias` | already used for the puppet |
| per-entity action component | same helper the mirror uses | proven on guards |

So the mechanism is: read the killer's alias at the moment of the kill, look up the victim
clip, put it on the wire, apply it with `SetTargetAction`. Three of those four steps are
already in the code.

## 3. Two classes of kill â€” do not conflate them

**Class G (grounded).** Finisher, ground finisher, stealth-from-behind, high-profile
standing, gunshot. The victim stands where it died; only the clip is wrong. Fixing this is
a table plus one field on the wire. Position needs nothing.

**Class P (paired / synchronised).** Air assassination, ledge assassination, corner
assassination, any "grab". These are two-actor sequences: the engine moves *both* bodies
into a fixed relative pose, then plays a matched pair. Replaying only the victim's clip
leaves it playing "pulled over a ledge" while standing in the street. Class P needs the
pair pose as well as the clip.

The MP executable already contains the engine's own protocol for this
(`M2A_StartWaitSynchroSequence`, `R2A_PlayerInPosForSynchroKill`,
`M2A_DispatchSynchroSequenceResult`) â€” that is the long-term answer for Class P and the
reason not to hand-roll it first.

## 4. The missing thing: the pair table

Alias `1818493434` appears in no dumped data file â€” it was captured live. So the table has
to be captured the same way: **from kills the engine performs itself.**

For each native kill, at the frame the victim's alive bit flips 1â†’0, log:

```
[AD] killer=<playerAlias> victim=<victimAlias> ch=<n> layer=<n> dist=<m> type=<label> victimEnt=<ptr>
```

`victim` is the clip we need. `killer` is what we will have on the wire at replication time
(the player's action is already captured every frame).

## 5. Capture procedure (one short run, ~15 minutes)

Do each of these **three times**, on a fresh guard, and note nothing â€” the log is the record:

1. low-profile stealth kill from behind (walk, not run)
2. high-profile assassination (standing, run-up)
3. air assassination (jump from a roof / ledge above)
4. ledge assassination (hang below, pull down)
5. corner / cover assassination
6. combat finisher (beat a guard down, finish)
7. ground finisher (knock down, stab on the ground)
8. pistol shot at close range
9. blowpipe / rope dart if unlocked
10. one "brute"/captain if one is nearby, to see whether they have their own set

After the run the table is built from the log by grouping on `killer` and taking the modal
`victim` per group.

## 6. Acceptance

* Class G: a replicated kill plays the same class of animation as the native kill, for at
  least finisher / stealth-behind / high-profile. Judged by the operator on video, and
  cross-checked in the log (`victim` alias equals the captured one).
* No regression: `SetLife(-1)` still returns 0, the victim still dies, corpse and marker
  unchanged.
* Class P is **not** accepted on clip alone; it stays behind a flag until the pair pose is
  solved.

## 7. Implementation shape (after the capture)

```cpp
// wire: one extra field, already reserved by the EntityState layout
struct EntityState { ... uint32_t deathAlias; ... };   // 0 = use the default clip

// sender: at kill time
es.deathAlias = DeathAliasFor(s.actionAlias);   // table lookup, 0 if unknown

// receiver: apply
int clip = es.deathAlias ? (int)es.deathAlias : 1818493434;
ac->SetTargetAction(clip, 0, 0, 0xFFFF, ACTLAYER_PRIMARY);
```

The table is a generated header (`death_clips.h`) from the capture, so it is data, not
code, and can be extended by running the capture again.

## 8. What this does not fix

* Position for Class P (see Â§3).
* The *sound* of the death, which is chosen by the same bypassed system.
* The corpse's final orientation, which follows the clip â€” so a wrong clip is also a wrong
  body placement.

---

## 9. The pipeline is ready (2026-10-29)

Everything except the capture itself now exists:

| piece | where | state |
|---|---|---|
| `[AD]` capture line | `0802334`, `[Coop] ActionDeathLog` (default 0) | built, deployed (`B0BB3BA3`), **on** in the live ini |
| line format | `[AD] player=<alias> victim=<alias> objId=<id> dist=<m> ent=<ptr>` | emitted at the single death transition |
| table builder | `ops/tools/ad_table.py` | selftest PASS; `--emit death_clips.h` |
| consumer | not written yet | `DeathClipFor(playerAlias)` replaces `1818493434` |

### The capture session, exactly

1. Launch, be the **host**, load a save.
2. Kill **one guard each way**, waiting ~2 s between kills, in this order:
   low-profile from behind · high-profile standing · air (roof/ledge above) ·
   ledge (hanging) · corner/cover · combat finisher · ground finisher · pistol.
3. Quit normally.
4. Then:

```powershell
py ops/tools/ad_table.py --emit ref\AC4Coop\src\death_clips.h
```

The tool prints, per killer action, the victim clip and the vote count; it writes
`death_clips.h` only when at least one pair exists.

### What "good" looks like

* `>= 3` distinct `player=` values (the eight kill types do not all map to distinct
  aliases — several may share one).
* The modal `victim=` differs between at least two of them. If every kill shows the
  same `victim=`, that is real data: the engine may be selecting the death from the
  victim's state rather than the attacker's action, and the conclusion changes.
* `victim=0` lines are expected sometimes and are not failures.

### What it still cannot do

Class P (air, ledge, corner) needs the pair pose as well as the clip. The table
gives the clip; the victim will play it in the wrong place until positioning is
solved. That is deliberately out of scope here.
