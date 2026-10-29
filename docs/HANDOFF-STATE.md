# Handoff state (compact)

## Where we are
- Repo: `C:\Users\Administrator\Documents\Default Project\bf-coop\ref\AC4Coop`, remote `fork` only.
- Verified build: tag `build-verified-2026-10-18` on `fix/live-game-udp-bind`. That tag is the
  previous known-good .asi; **do not move or delete it**.
- Migration: complete and merged into `fix/live-game-udp-bind` (fast-forward, both branches at the same
  commit). Every file under `src/` differs from the upstream initial commit.
- **Deployed 2026-10-18 ~09:46**: the migration build is the `.asi` in the game folder now. Previous
  .asi + ini backed up to `bf-coop\backups\20261018-094641\`.
- Live check of the deployed build (2026-10-18): game boots, world/player/18 entities enumerated, F9 scene
  probe answers, and F6 built + integrated a puppet (`BuildEntityWithSelector rc=0 entity=2D1C1220`,
  `AddEntity issued`, 0 exceptions). The spawn was done with `[Net] Enabled=0`; with net on, the frame
  path returns before the spawn request, so F6 is a single-machine check only.
- Tester package: `bf-coop\dist\AC4Coop-tester-2026-10-18.zip`
  (sha256 `AD541BCB6AB141FEF39F2A1BA57E50889C07A97A3A0FDAC93FAE6630916A6F5B`), .asi inside is the
  deployed build (sha256 `B5A8ABBC76BC1ED1B3355DB51BE114C1C0734EF87AEBDB2C2202E8DECC5D7093`).
- Deploy protocol: only after build **and** the live check for the feature.

## Facts that matter
- Wire protocol: `NET_PROTO_VER = 9` (netstate.h:116). Both machines must run the same .asi.
- Kill recipe (verified live): `SetLife(hp,-1)` -> `hp+0x60 |= 0x01000000` ->
  `sub_10EF8C0(entity)[+4] |= 0x1000` (combat is-alive) -> `ai+0x8C = 1` (AI-side only) ->
  death clip `1818493434` -> `SetLayer(&cc->m_FilterInfo, 0x29)`.
- Health: `ai->[+0x70]` array (count `+0x76`), vtable `0x02712F60`; Life `+0x5C`, Max `+0x5E`, flags `+0x60`.
- Identity: `HandleContainer::m_ObjectID` (u64); host-assigned, peer echoes it.
- Dead NPCs leave the snapshot enumeration -> deaths/damage are pushed as events (`kind=3`), not inferred.
- Is-alive: `sub_133F550(entity)` = `(ai+0xB8 & 0xC)==4` OR service `0x5EB03CEC` bit `0x1000`.
- `ai+0x98` == `health+0x63`. AI has two service arrays: `+0x5C` and `+0x70`.
- Input: engine polls pads 0..3; SP **merges** pad input into the local player, so a native second player
  is closed. Our path: drive the puppet's `HumanGroundData` (proven: it walks and collides).
- Decomp: `C:\Users\Administrator\ac4-decomp` (128,340 files, gamedb indexed: 3.47M call edges).

## Next actions (in order)
1. `git checkout migration/off-upstream`.
2. Continue the worklist in `docs/MIGRATION-OFF-UPSTREAM.md` from row 6. Per header: rewrite (own text,
   same declarations) -> build clean -> live check -> commit.
3. Big four at the end, one per pass: `Entity` (spawn puppet + kill), `World` (world loads + mirrors),
   `Human`/`AIActor` (mirror pose + action replication).
4. Then project files, then merge to `fix/live-game-udp-bind` and re-deploy + rebuild the tester package.

## Do not
- Do not rewrite the big headers blind; a wrong offset compiles clean and corrupts state at runtime.
- Do not deploy while on the migration branch.
- Do not touch `VPadEnable` (test scaffolding, default 0) in a normal build.
