# When you next launch the game

Everything below is already armed. You do not need to run any tool.

## What happens by itself

`ops/tools/e0b_run.py` is waiting for a **new** `phase1_log_*.txt` containing
`[Net] init=`. Within ~2 s of the game reporting the socket up it will:

1. listen for 6 s to learn your position from the game's own packets,
2. pick the guard nearest you from the log's `[Id]` rows,
3. run `peer9.py --walk --echo-pos --kill <that guard>` for 40 s,
4. write `ops/evidence/E0-B/run-<stamp>.json` and wake the orchestrator.

## What you do

1. Launch, load a save, **get in-world and stand near guards**.
2. Let the ~50 s run finish. You should see a ghost walking a small circle around
   you (that is the peer driving the puppet), and a guard drop.
3. Then, for the animation capture (`[Coop] ActionDeathLog=1` is already on), kill
   **one guard each way**, ~2 s apart:
   low-profile from behind · high-profile standing · air · ledge · corner ·
   combat finisher · ground finisher · pistol.
4. Quit normally.

## What I do with it

| result | next |
|---|---|
| `[Kill] peer DEATH` in the log | the kill lane works cross-process → C2 is one machine away |
| `[AD]` lines with ≥3 distinct `player=` | `ad_table.py --emit` produces `death_clips.h` → deaths stop being one clip |
| crash | the log names it; `0x681817` and friends are guarded, so a new address is a new finding |

## If something looks wrong

* No ghost, but the log shows `[Net] apply` → the puppet is being applied but is
  out of view; the peer's echoed position may have landed before the world loaded.
* Ghost but no `[Net] apply` → the peer is talking and the game is not listening:
  check `[Net] cfg` (ports) and that the game is the **host** (`EntityHost=1`).
* Neither → check the ini still says `LocalPort=47801 PeerPort=47802 PeerIP=127.0.0.1`.

Nothing needs deciding. Leave the machine when you are done; the watcher and the
runner are ordinary processes and will not survive a reboot, which is fine.
