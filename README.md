# Tiny Raylib ECS FPS

A deliberately small raylib C11 demo: first-person movement on a flat plane, procedural cubes, palette-only rendering, shooting, and an authoritative UDP client/server simulation. The supplied `ecs.h` is the gameplay registry; there is no second ECS or heap-backed game container.

## Build

Requires raylib 6 (or a compatible raylib install), a C11 compiler, and POSIX sockets on the host used by this minimal implementation.

```sh
make
```

The build uses `-std=c11 -O2 -Wall -Wextra -Wpedantic` and obtains raylib flags with `pkg-config`.

## Run

Start the authoritative server:

```sh
./tinyfps --server 4242
```

In another terminal, start a client. The client accepts an IPv4 address or `localhost`:

```sh
./tinyfps --client 127.0.0.1 4242 VYSE
```

The final client argument is the player's name and must contain exactly four letters; lowercase input is converted to uppercase. The server has no window and exits cleanly on Ctrl-C. Its authoritative simulation uses a `0.01` second fixed timestep (100 Hz), independently of rendering, with 50 Hz client commands and 20 Hz snapshots. The UDP protocol uses bounded, session-qualified input, chat, disconnect, and snapshot records; rejects packets with the wrong size, magic, type, or session; rejects duplicate/out-of-order chat and snapshots; replaces an old same-name session on reconnect; caps snapshots at 28 entities; and expires peers after 10 seconds without a command. The server owns player movement, jumping, aim, cube holding, bullet spawning, projectile collision, and tick numbers. Clients interpolate authoritative snapshots while applying mouse-look immediately to the local camera. Up to three peers can be active.

## Controls

- `WASD`: move
- mouse: aim (the cursor starts captured)
- `Space`: jump
- `Shift` + `W`: sprint (faster, gun tilts up, ADS blocked while held)
- left mouse: fire
- right mouse: aim down sights (zoom, slower move/look, tighter spread)
- `E`: hold the cube under the crosshair; it slides on the ground where you look (yaw steers, pitch drives distance/scale: look down = close/tiny, look forward/far = distant/giant); release to drop it (scale persists)
- `Enter` or `T`: open chat; `Enter` sends and `Esc` cancels
- `Esc`: release/capture the mouse when chat is closed
- `F1` (hold): fullscreen help overlay, pauses the game while held

## Rendering and palette

The scene and pose-driven gun model render to one exactly `92 x 64` target and the HUD to a second one, then both are presented with integer nearest-neighbor scaling and centered letterboxing in a resizable `920 x 640` window; screen shake offsets only the scene layer. Local and remote weapons share the same per-weapon geometry renderers; position, yaw, pitch, roll, scale, recoil, slide recoil, firing state, and the weapon's own reload animation define each rendered pose. The server replicates quantized ADS/bob/lean/reload/sprint presentation per player, stepped through the same shared helpers the client uses for local prediction, so remotes show the same gun flavor. The render loop has no software FPS target; raylib's vsync hint controls presentation. No external assets are used. Geometry and HUD marks use these exact colors, darkest to lightest, with pure black reserved for the requested remote-player wire outline:

- `#100818` / `(16, 8, 24)`
- `#3c1e66` / `(60, 30, 102)`
- `#9b7bd0` / `(155, 123, 208)`
- `#f6d5e8` / `(246, 213, 232)`

The low-resolution target is point-filtered and MSAA is not enabled. A palette-safe, one-logical-pixel screen-space edge shader produces object outlines before the HUD is drawn; it operates only on the `92 x 64` scene, and presentation is restricted to integer scaling at integer offsets. HUD text uses the bundled CC0 Tom Thumb 3x5 bitmap font from `tom_thumb_3x5.h`. Connection, join/leave, chat notifications, and the active chat input share one compact top message slot; each new message replaces the previous one and remains for five seconds.

## ECS architecture

`main.c` uses the existing fixed-capacity bitmask ECS with these components:

- `COMP_POSITION`: fixed-point world position arrays
- `COMP_VELOCITY`: fixed-point per-tick movement/projectile velocity
- `COMP_AIM`: player yaw/pitch
- `COMP_PLAYER`: identifies controllable players
- `COMP_PROJECTILE`: finite-lived bullets
- `COMP_LIFETIME`: bullet, casing, and impact-mark expiry
- `COMP_RENDERABLE`: render query membership
- `COMP_NETWORKED`: snapshot query membership
- `COMP_SCALE`: world-scale membership (cubes); backing `scale[]` in `GameState`

The input, aim, movement, holding, weapon, projectile, lifetime, render, and network paths use ECS queries. Entity creation, reuse, component add/remove, queries, and safe destruction all go through `ecs_create`, `ecs_add`, `ecs_remove`, `ecs_has`/the ECS query masks, `ecs_query`, `ecs_next`, and `ecs_destroy`. Eight deterministic cubes are server-authoritative ECS entities with `COMP_SCALE` and are networked just like players, bullets, and shell casings; while held with `E` the server slides a cube on the floor around the holder (yaw steers, pitch-driven distance `dist = 2.0 + pitch * 0.02`, clamped 1.0–4.0m) and sets `scale = dist / 2.0`, so looking down shrinks it and looking far grows it, always grounded at half height. Player collision and bullets use the scaled extents, and the scale persists after release. The client world is purely snapshot-driven (`client_init` starts empty, `renew_client_session` clears via `client_clear_world`); `game_init` is server-only, which removes the old ghost-cube duplication at the root. Casings are spawned and simulated authoritatively with ejection velocity, gravity, floor bounce, yaw, pitch, and quantized roll, so every client sees the same falling brass.

## State budget

All mutable gameplay, ECS, input, peer, sequence, and packet-buffer storage is contained in `GameState`. The source contains:

```c
_Static_assert(sizeof(GameState) <= 4096, "game state exceeds 4 KiB");
```

The build-time assertion keeps the complete mutable game state within 4 KiB. This includes the ECS arrays, interpolation state, fixed player names and notification text, input/peer records, and two fixed packet buffers.

`NetInputPacket` is 24 bytes, `NetChatPacket` is 36 bytes, `NetEntity` is 26 bytes (including quantized ADS/bob/lean/reload/sprint presentation per player plus the explicit `scale` field for scaled cubes), and a full fixed `NetSnapshot` is 884 bytes. There is no `malloc`, `calloc`, `realloc`, `free`, `strdup`, VLA, runtime-loaded asset, or application-owned growable container. Raylib's render targets are GPU/render handles and are outside the requested CPU gameplay-state budget.
