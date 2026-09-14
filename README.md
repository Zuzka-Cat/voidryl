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
- left mouse: fire
- right mouse or `E`: hold the cube in the center view; release to drop it
- `Enter` or `T`: open chat; `Enter` sends and `Esc` cancels
- `Esc`: release/capture the mouse when chat is closed
- `F1`: show/hide the small help overlay

## Rendering and palette

The scene, pose-driven gun model, and HUD are rendered directly to exactly `92 x 64`, then presented with integer nearest-neighbor scaling and centered letterboxing in a resizable `920 x 640` window. Local and remote weapons share the same geometry renderer; position, yaw, pitch, roll, scale, recoil, slide recoil, and firing state define each rendered pose. The render loop has no software FPS target; raylib's vsync hint controls presentation. No external assets are used. Geometry and HUD marks use these exact colors, darkest to lightest, with pure black reserved for the requested remote-player wire outline:

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

The input, aim, movement, holding, weapon, projectile, lifetime, render, and network paths use ECS queries. Entity creation, reuse, component add/remove, queries, and safe destruction all go through `ecs_create`, `ecs_add`, `ecs_remove`, `ecs_has`/the ECS query masks, `ecs_query`, `ecs_next`, and `ecs_destroy`. Eight deterministic cubes are ECS entities and are networked just like players, bullets, and shell casings; the server moves a held cube in front of its player and replicates that position. Casings are spawned and simulated authoritatively with ejection velocity, gravity, floor bounce, yaw, pitch, and quantized roll, so every client sees the same falling brass.

## State budget

All mutable gameplay, ECS, input, peer, sequence, and packet-buffer storage is contained in `GameState`. The source contains:

```c
_Static_assert(sizeof(GameState) <= 4096, "game state exceeds 4 KiB");
```

The build-time assertion keeps the complete mutable game state within 4 KiB. This includes the ECS arrays, interpolation state, fixed player names and notification text, input/peer records, and two fixed packet buffers.

`NetInputPacket` is 20 bytes, `NetChatPacket` is 32 bytes, `NetEntity` is 14 bytes, and a full fixed `NetSnapshot` is 416 bytes. There is no `malloc`, `calloc`, `realloc`, `free`, `strdup`, VLA, runtime-loaded asset, or application-owned growable container. Raylib's render target is a GPU/render handle and is outside the requested CPU gameplay-state budget.
