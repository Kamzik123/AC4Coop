# Credits

## This project

All gameplay behaviour in this repository — the host-authoritative entity sync, the player puppet, the
combat/kill path (engine-native death via `CSrvNPCHealth::SetLife`, the combat is-alive service flag,
corpse handling), the guest->host kill/damage relay, identity via `HandleContainer::m_ObjectID`, the
decompilation audit and the planning documents — is original work by Davidoff-byte.

## Initial framework

The initial repository skeleton (project files, `Hook`/`HookMath` utilities, and reverse-engineered
`Scimitar` class declarations) came from Kamzik123/AC4Coop's initial commit. It is being replaced
incrementally with independently written equivalents regenerated from our own decompilation of
`AC4BFSP.exe`; see `docs/MIGRATION-OFF-UPSTREAM.md` for the per-file status.

No license file was present in the upstream repository. Until the replacement is complete this is a
private fork for personal use; do not redistribute upstream-derived files without permission.
