# Visual and Weapon Improvements

## Players and damage feedback

- [x] Match other players' normal body color to the cubes.
- [x] Add an explicit black outline around other player models.
- [x] Brighten player models briefly when they are hit.
- [x] Show a durable first-person border and center hit indicator when the local player is hurt.

## Weapons and recoil

- [x] Render guns for other players with their player position and aim rotation.
- [x] Use one pose-driven gun geometry renderer for local and remote weapons.
- [x] Give the gun renderer position, yaw, pitch, roll, scale, recoil, slide recoil, and firing state.
- [x] Move the top slide sharply backward when firing without moving the grip by the same amount.
- [x] Apply smaller spring-damped recoil to the complete gun.
- [x] Add mouse-look inertia, with faster look motion producing a larger gun response.

## Bullets and shell casings

- [x] Replace the chunky bullet appearance with a longer, slimmer pointed cylinder.
- [x] Spawn shell casings from the ejection port at an outward and upward angle.
- [x] Simulate casing position, velocity, gravity, floor bounce, yaw, pitch, and roll.
- [x] Replicate casing transforms so shell casings are visible to every player.
- [x] Keep projectile, impact-mark, and casing counts within the fixed ECS and snapshot limits.

## Project constraints and documentation

- [x] Raise the `GameState` compile-time budget from 2 KiB to 4 KiB.
- [x] Update the README for shared gun rendering, casings, and the 4 KiB state budget.
- [x] Build with the project's strict C11 warning flags.
