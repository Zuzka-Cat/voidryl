#ifndef SHARED_C
#define SHARED_C

#define _POSIX_C_SOURCE 200809L

#include "raylib.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define u16 uint16_t
#define i16 int16_t
#define i32 int32_t
#define u32 uint32_t
#define i8 int8_t
#define u8 uint8_t
#define f32 float

#define CHAR_IS_LOWER(c) ((c) >= 'a' && (c) <= 'z')
#define CHAR_IS_UPPER(c) ((c) >= 'A' && (c) <= 'Z')
#define CHAR_IS_DIGIT(c) ((c) >= '0' && (c) <= '9')
#define CHAR_TO_UPPER(c) (CHAR_IS_LOWER(c) ? ((c) - ('a' - 'A')) : (c))
#define CHAR_IS_CHAT(c) (CHAR_IS_UPPER(c) || CHAR_IS_DIGIT(c) || (c) == ' ')

typedef struct {
  u8 *items;
  i32 length;
  i32 capacity;
} String;

#define STR(lit)                                                               \
  ((String){(u8 *)(lit), (i32)(sizeof(lit) - 1), (i32)(sizeof(lit) - 1)})

#define LOGICAL_W 92
#define LOGICAL_H 64
#define POS_SCALE 64

#define MAX_PLAYERS 8
#define MAX_CUBES 8
#define MAX_BULLETS 8
#define MAX_CASINGS 8
#define MAX_IMPACT_MARKS 8

#define NET_ENTITY_LIMIT 32
#define NET_MAGIC 0xB7u
enum NetPacketKind {
  NET_PACKET_INPUT = 1,
  NET_PACKET_SNAPSHOT,
  NET_PACKET_CHAT,
  NET_PACKET_DISCONNECT,
  NET_PACKET_NONE = 255
};

#define SIM_HZ 100
#define SIM_DT (1.f / SIM_HZ)
#define SNAPSHOT_HZ 20
#define SNAPSHOT_INTERVAL (SIM_HZ / SNAPSHOT_HZ)
#define COMMAND_HZ 50
#define COMMAND_INTERVAL (SIM_HZ / COMMAND_HZ)

#define CHAT_LENGTH 18
#define PLAYER_NAME_LENGTH 5
#define NOTICE_LENGTH 21

#define BTN_FIRE 1u
#define BTN_GRAB 2u
#define BTN_JUMP 4u
#define BTN_AIM 8u
#define BTN_SPRINT 16u
#define BTN_ALL (BTN_FIRE | BTN_GRAB | BTN_JUMP | BTN_AIM | BTN_SPRINT)

// Weapon catalog: single source of truth for fire rate, mag size, reload and
// recoil. To add a weapon, append a WeaponId and a WeaponDef entry; the
// server, client prediction and snapshot code all read from here so no
// per-weapon branching is needed elsewhere.
enum WeaponId { WEAPON_P99 = 0, WEAPON_COUNT };

typedef struct WeaponDef {
  u8 mag_size;
  u8 cooldown_tap_ticks;
  u8 cooldown_hold_ticks;
  u8 recoil_pitch_tap_deg;
  u8 recoil_pitch_hold_deg;
  u16 reload_ticks;
  f32 slide_tap;
  f32 slide_hold;
  f32 recoil_vel_tap;
  f32 recoil_vel_hold;
  f32 rise_vel_tap;
  f32 rise_vel_hold;
  f32 spread_hip_deg;
  f32 spread_aim_deg;
  f32 hip_fov;
  f32 aim_fov;
  f32 aim_sens_scale;
  f32 aim_move_scale;
  // Viewmodel hip base + ADS cant in degrees, per gun. Full-zoom ADS
  // translation is solved per weapon from its sight tip (see gun_ads_target
  // in client.c), not stored, so sight alignment can't drift from the data.
  f32 hip_off_x;
  f32 hip_off_y;
  f32 ads_cant_deg;
  // Static ADS pitch trim in degrees, blended in with aim. Positive tips
  // the muzzle down, negative raises it. Purely cosmetic (bullets follow
  // the view angles); one number to re-zero the zoomed sight picture.
  f32 ads_pitch_deg;
  // Sprint pose: static pitch added with the sprint blend. Negative raises
  // the muzzle skyward classic-sprint style. Cosmetic only, per gun.
  f32 sprint_tilt_deg;
} WeaponDef;

static const WeaponDef WEAPON_DEFS[WEAPON_COUNT] = {
    [WEAPON_P99] =
        {
            .mag_size = 15,
            .cooldown_tap_ticks = 16,
            .cooldown_hold_ticks = 8,
            .recoil_pitch_tap_deg = 1,
            .recoil_pitch_hold_deg = 2,
            .reload_ticks = 120,
            .slide_tap = 0.09f,
            .slide_hold = 0.126f,
            // Viewmodel impulses (velocity kicks into the client springs in
            // update_viewmodel): sharp straight-back punch + lagging muzzle
            // flip. Peaks ride the spring clamps; the spring rates shape the
            // punch/recover timing, so tune feel there, magnitude here.
            .recoil_vel_tap = 1.05f,
            .recoil_vel_hold = 1.9f,
            .rise_vel_tap = 1.0f,
            .rise_vel_hold = 2.0f,
            .spread_hip_deg = 2.6f,
            .spread_aim_deg = 0.45f,
            .hip_fov = 72.0f,
            .aim_fov = 45.0f,
            .aim_sens_scale = 0.55f,
            .aim_move_scale = 0.5f,
            .hip_off_x = 0.06f,
            .hip_off_y = -0.18f,
            .ads_cant_deg = -12.0f,
            .ads_pitch_deg = -3.0f,
            .sprint_tilt_deg = -32.0f,
        },
};

static const WeaponDef *weapon_def(u8 id) {
  return &WEAPON_DEFS[id < WEAPON_COUNT ? id : WEAPON_P99];
}

// Shell catalog: one entry per WeaponId, parallel to WEAPON_DEFS.
// To add different shells per gun, add one more line here, e.g.
//   [WEAPON_P90] = { .len = 0.06f, ... },
// The server spawns (spawn_casing) and the client renders (draw_casing)
// both read through shell_def(), so no per-shell branching is needed
// elsewhere. Units are meters and m/s; port offsets are in gun_basis
// space (forward, side with +right along gun_basis r, up).
// body_pal/rim_pal select the palette for the brass: 0 dark, 1 purple,
// 2 mid, 3 light. The dark outline is always drawn for readability.
typedef struct ShellDef {
  f32 len;
  f32 radius;
  f32 rim_radius;
  f32 port_fwd;
  f32 port_side;
  f32 port_up;
  f32 eject_right;
  f32 eject_up;
  f32 eject_back;
  f32 jitter;
  u8 body_pal;
  u8 rim_pal;
} ShellDef;

static const ShellDef SHELL_DEFS[WEAPON_COUNT] = {
    [WEAPON_P99] =
        {
            .len = 0.07f,
            .radius = 0.014f,
            .rim_radius = 0.017f,
            .port_fwd = 0.55f,
            .port_side = 0.14f,
            .port_up = -0.06f,
            .eject_right = 2.4f,
            .eject_up = 1.9f,
            .eject_back = 0.55f,
            .jitter = 0.35f,
            .body_pal = 3,
            .rim_pal = 2,
        },
};

static const ShellDef *shell_def(u8 id) {
  return &SHELL_DEFS[id < WEAPON_COUNT ? id : WEAPON_P99];
}

#define PLAYER_FIRE_FLASH_TICKS 6
#define PLAYER_HIT_FLASH_TICKS 24
#define BULLET_LIFETIME_TICKS 128
#define CASING_LIFETIME_TICKS 128
#define IMPACT_MARK_LIFETIME_TICKS 128

#define ARENA 32
#define BULLET_SPEED 32.0f
#define BULLET_ZERO_DISTANCE 12.0f
#define ARENA_POS (15 * POS_SCALE)

// Perspective-hold tuning. While holding E the cube slides on the ground
// around the holder: yaw steers it, pitch drives distance/scale (look down
// brings it close/tiny, looking forward/far pushes it out/giant). Y stays
// grounded at half height. Scale persists after release. The pitch curve is
// deliberately gentle so small look adjustments only nudge the cube.
#define HOLD_BASE_DIST 2.0f
#define HOLD_PITCH_STEP 0.02f
#define HOLD_MIN_DIST 1.0f
#define HOLD_MAX_DIST 4.0f
#define CUBE_SCALE_MIN 0.12f
#define CUBE_SCALE_MAX 2.0f

static f32 hold_dist_for_pitch(i16 pitch_deg) {
  f32 d = HOLD_BASE_DIST + (f32)pitch_deg * HOLD_PITCH_STEP;
  if (d < HOLD_MIN_DIST)
    return HOLD_MIN_DIST;
  if (d > HOLD_MAX_DIST)
    return HOLD_MAX_DIST;
  return d;
}

static u8 scale_encode(f32 s) {
  if (s < CUBE_SCALE_MIN)
    s = CUBE_SCALE_MIN;
  if (s > CUBE_SCALE_MAX)
    s = CUBE_SCALE_MAX;
  int q = (int)lroundf(s * 64.0f);
  if (q < 1)
    q = 1;
  if (q > 255)
    q = 255;
  return (u8)q;
}

static f32 scale_decode(u8 q) {
  if (q == 0)
    return 1.0f;
  return (f32)q / 64.0f;
}

static const Vector3 GUN_MUZZLE_OFFSET = {1.46f, 0.42f, -0.18f};

enum EntityKind {
  KIND_CUBE = 1,
  KIND_PLAYER,
  KIND_BULLET,
  KIND_IMPACT_MARK,
  KIND_CASING
};

enum {
  COMP_POSITION = 0,
  COMP_VELOCITY,
  COMP_AIM,
  COMP_PLAYER,
  COMP_PROJECTILE,
  COMP_LIFETIME,
  COMP_RENDERABLE,
  COMP_NETWORKED,
  COMP_SCALE
} ComponentKind;

typedef struct InputCommand {
  i8 move_x, move_z;
  i16 aim_yaw, aim_pitch;
  u8 buttons;
} InputCommand;

typedef struct Peer {
  u32 address, session, retired_session;
  u16 port, last_sequence, last_chat_sequence, last_seen;
  u8 active, got_sequence, got_chat_sequence, player;
} Peer;

typedef struct NetInputPacket {
  u8 magic, type, claimed_player, buttons;
  u32 session;
  u16 sequence;
  i8 move_x, move_z;
  i16 aim_yaw, aim_pitch;
  u8 name[PLAYER_NAME_LENGTH];
  // Explicit tail: struct would pad 21 -> 24 anyway. Named so
  // initializers zero it deterministically (padding bytes would stay
  // unspecified per C11 6.2.6.1p6). Always send zero, ignore on receive.
  u8 reserved[3];
} NetInputPacket;

typedef struct NetEntity {
  u8 entity, kind, owner, flags, extra, weapon, ammo, reloading;
  // Replicated presentation: server-smoothed pose so remotes show the same
  // ADS/bob/lean/reload/sprint flavor with no second animation codebase.
  u8 aim_q, walk_phase_q, walk_blend_q, reload_q, sprint_q;
  i8 lean_x_q, lean_z_q;
  // World scale for scaled entities (cubes). Quantized like the other
  // presentation fields; 0 means 1.0 for backwards-tolerant decode.
  // Proper protocol field, not padding reuse.
  u8 scale;
  i16 x, y, z, yaw, pitch;
} NetEntity;

typedef struct NetChatPacket {
  u8 magic, type, player, length;
  u32 session;
  u16 sequence;
  u8 name[PLAYER_NAME_LENGTH];
  u8 text[CHAT_LENGTH];
  // Explicit tail: 33 -> 36. See NetInputPacket.reserved.
  u8 reserved[3];
} NetChatPacket;

typedef struct NetSnapshot {
  u8 magic, type, local_player, count;
  u32 session;
  u16 tick, acknowledged_sequence;
  NetEntity entities[NET_ENTITY_LIMIT];
  u8 names[MAX_PLAYERS][PLAYER_NAME_LENGTH];
} NetSnapshot;

// Dense snapshot wire format: 12-byte header + count * 26-byte entities +
// 40-byte names. Max 884 bytes at 32 entities, typically 286 (8 cubes +
// 1 player). Zero-filling of unused slots never hits the wire.
#define NET_SNAPSHOT_HEADER_SIZE 12
#define NET_SNAPSHOT_NAMES_SIZE (MAX_PLAYERS * PLAYER_NAME_LENGTH)
#define NET_MAX_DATAGRAM sizeof(NetSnapshot)
#define NET_INPUT_WIRE_SIZE sizeof(NetInputPacket)
#define NET_CHAT_WIRE_SIZE sizeof(NetChatPacket)
#define NET_OFF_COUNT 3

_Static_assert(sizeof(NetEntity) == 26, "NetEntity wire size changed");
_Static_assert(sizeof(NetInputPacket) == 24, "NetInputPacket padding changed");
_Static_assert(sizeof(NetChatPacket) == 36, "NetChatPacket padding changed");
_Static_assert(offsetof(NetInputPacket, name) == 16, "input name offset moved");
_Static_assert(offsetof(NetInputPacket, reserved) == 21, "input tail moved");
_Static_assert(offsetof(NetChatPacket, name) == 10, "chat name offset moved");
_Static_assert(offsetof(NetChatPacket, text) == 15, "chat text offset moved");
_Static_assert(offsetof(NetChatPacket, reserved) == 33, "chat tail moved");
_Static_assert(offsetof(NetSnapshot, entities) == NET_SNAPSHOT_HEADER_SIZE,
               "snapshot header has padding");
_Static_assert(sizeof(NetSnapshot) ==
                   NET_SNAPSHOT_HEADER_SIZE +
                       NET_ENTITY_LIMIT * sizeof(NetEntity) +
                       NET_SNAPSHOT_NAMES_SIZE,
               "snapshot has trailing padding");

static i32 net_snapshot_wire_size(u8 count) {
  if (count > NET_ENTITY_LIMIT)
    return -1;
  return (i32)(NET_SNAPSHOT_HEADER_SIZE +
               (i32)count * (i32)sizeof(NetEntity) + NET_SNAPSHOT_NAMES_SIZE);
}

#define ECS_MAX_ENTITIES 32u
#define ECS_MAX_COMPONENTS 9u
#define ECS_INVALID_ENTITY UINT32_MAX

typedef struct ECS {
  uint64_t mask[ECS_MAX_COMPONENTS];
  uint32_t dead;
  uint8_t count;
} ECS;

typedef struct EcsQuery {
  const ECS *ecs;
  uint64_t bits;
  uint64_t wanted;
  uint32_t entity;
} EcsQuery;

static bool ecs_alive(const ECS *e, uint32_t ent) {
  return ent < e->count && !(e->dead & (UINT32_C(1) << ent));
}

static uint32_t ecs_create(ECS *e) {
  uint32_t live_range =
      e->count == 32u ? UINT32_MAX : (UINT32_C(1) << e->count) - 1u;
  uint32_t free_bits = e->dead & live_range;
  if (free_bits) {
    uint32_t ent = (uint32_t)__builtin_ctz(free_bits);
    e->dead &= ~(UINT32_C(1) << ent);
    return ent;
  }
  if (e->count >= ECS_MAX_ENTITIES)
    return ECS_INVALID_ENTITY;
  return e->count++;
}

static void ecs_add(ECS *e, uint32_t ent, uint32_t component) {
  if (ent < ECS_MAX_ENTITIES && component < ECS_MAX_COMPONENTS &&
      ecs_alive(e, ent))
    e->mask[component] |= UINT64_C(1) << ent;
}

static void ecs_remove(ECS *e, uint32_t ent, uint32_t component) {
  if (ent < ECS_MAX_ENTITIES && component < ECS_MAX_COMPONENTS)
    e->mask[component] &= ~(UINT64_C(1) << ent);
}

static bool ecs_has(const ECS *e, uint32_t ent, uint32_t component) {
  return ent < ECS_MAX_ENTITIES && component < ECS_MAX_COMPONENTS &&
         (e->mask[component] & (UINT64_C(1) << ent)) != 0;
}

static void ecs_destroy(ECS *e, uint32_t ent) {
  if (!ecs_alive(e, ent))
    return;
  for (uint32_t c = 0; c < ECS_MAX_COMPONENTS; ++c)
    e->mask[c] &= ~(UINT64_C(1) << ent);
  e->dead |= UINT32_C(1) << ent;
  while (e->count && (e->dead & (UINT32_C(1) << (e->count - 1u)))) {
    e->dead &= ~(UINT32_C(1) << (e->count - 1u));
    --e->count;
  }
}

static EcsQuery ecs_query(const ECS *e, const uint32_t *components,
                          uint32_t n) {
  EcsQuery q = {e, UINT64_MAX, UINT64_MAX, ECS_INVALID_ENTITY};
  if (n > ECS_MAX_COMPONENTS)
    n = ECS_MAX_COMPONENTS;
  for (uint32_t i = 0; i < n; ++i)
    if (components[i] < ECS_MAX_COMPONENTS)
      q.wanted &= e->mask[components[i]];
  q.bits = q.wanted;
  if (e->count < ECS_MAX_ENTITIES)
    q.bits &= (UINT64_C(1) << e->count) - 1u;
  return q;
}

static bool ecs_next(EcsQuery *q) {
  if (!q->bits)
    return false;
  q->entity = (uint32_t)__builtin_ctzll(q->bits);
  q->bits &= q->bits - 1u;
  return true;
}

// Tom Thumb 3x5 font
// source: https://opengameart.org/content/tom-thumb-tiny-ascii-font-3x5

#define TOM_THUMB_3X5_CHARS " !+-./0123456789:?ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define TOM_THUMB_3X5_COUNT 44u

static const uint16_t tom_thumb_3x5[TOM_THUMB_3X5_COUNT] = {
    0x0000, 0x4904, 0x2E80, 0x7000, 0x4000, 0x12A4, 0x3B6E, 0x2C92, 0x62A7,
    0x628E, 0x5BC9, 0x798E, 0x39EF, 0x72A4, 0x7BEF, 0x7BCE, 0x4100, 0x7282,
    0x2BED, 0x6BAE, 0x3923, 0x6B6E, 0x79E7, 0x79E4, 0x39EB, 0x5BED, 0x7497,
    0x126A, 0x5BAD, 0x4927, 0x5FED, 0x5FFD, 0x2B6A, 0x6BA4, 0x2B7B, 0x6BF5,
    0x388E, 0x7492, 0x5B6B, 0x5B52, 0x5BFD, 0x5AAD, 0x5A92, 0x72A7,
};

typedef struct GameState {
  ECS ecs;
  i16 px[ECS_MAX_ENTITIES], py[ECS_MAX_ENTITIES], pz[ECS_MAX_ENTITIES];
  i16 vx[ECS_MAX_ENTITIES], vy[ECS_MAX_ENTITIES], vz[ECS_MAX_ENTITIES];
  i16 yaw[ECS_MAX_ENTITIES], pitch[ECS_MAX_ENTITIES];
  u8 kind[ECS_MAX_ENTITIES], lifetime[ECS_MAX_ENTITIES];
  // Client-side interpolation scratch, never simulated or snapshotted.
  // lag_yaw/lag_pitch hold the encoded yaw/pitch delta (+LAG_NEUTRAL) from
  // the last snapshot so remotes can exaggerate turn bob between snapshots.
  // lifetime stays purely an expiry counter for bullets/casings/marks.
  u8 owner[ECS_MAX_ENTITIES], lag_yaw[ECS_MAX_ENTITIES];
  u8 lag_pitch[ECS_MAX_ENTITIES];
  u8 flash[ECS_MAX_ENTITIES], held_by[ECS_MAX_ENTITIES];
  // Per-entity weapon state (player entities only). Single source of truth
  // for ammo / reload / fire-rate; no parallel per-player arrays.
  u8 weapon_id[ECS_MAX_ENTITIES];
  u8 weapon_ammo[ECS_MAX_ENTITIES];
  u8 weapon_cooldown[ECS_MAX_ENTITIES];
  u8 weapon_prev[ECS_MAX_ENTITIES];
  u16 weapon_reload[ECS_MAX_ENTITIES];
  // Client-side prediction metadata for stale-snapshot guards (never
  // simulated, never snapshotted). weapon_dirty_seq[e] is the input sequence
  // that caused the last local weapon change on e; compared against the
  // snapshot's acknowledged_sequence to prove the server had seen our change
  // before we let it regress predicted state. Zero means synced (see
  // predicted_is_stale). last_snapshot_ack is the ack from the last applied
  // snapshot, for debugging/future predicted fields.
  u16 weapon_dirty_seq[ECS_MAX_ENTITIES];
  u16 last_snapshot_ack;
  InputCommand input[MAX_PLAYERS];
  Peer peers[MAX_PLAYERS];
  u8 player_entity[MAX_PLAYERS];
  u8 network_entity[ECS_MAX_ENTITIES];
  u32 snapshot_seen, session;
  u16 tick, next_sequence, next_chat_sequence;
  u16 last_snapshot_tick, last_chat_sequence, notice_ticks;
  u16 snapshot_silence_ticks;
  u8 local_player, connected, mouse_captured, help, chatting, chat_length;
  u8 interpolation_ticks, command_ticks, gun_kick;
  u8 visible_players, got_snapshot, got_chat_sequence, notice_length;
  u8 requested_name[PLAYER_NAME_LENGTH];
  u8 player_name[MAX_PLAYERS][PLAYER_NAME_LENGTH];
  u8 chat_input[CHAT_LENGTH];
  u8 notice[NOTICE_LENGTH];
  i16 view_yaw, view_pitch;
  f32 gun_recoil, gun_recoil_velocity, gun_slide;
  f32 gun_rise, gun_rise_velocity;
  f32 gun_sway_x, gun_sway_y, look_delta_x, look_delta_y;
  f32 aim_blend, screen_shake;
  f32 walk_phase, walk_blend;
  f32 move_sway_x, move_sway_z;
  f32 move_vel_x, move_vel_z;
  f32 sprint_blend;
  // Per-entity world scale backing COMP_SCALE (cubes only for now,
  // 1.0 == normal). Authoritative on the server, replicated via
  // NetEntity.scale, applied by renderer/collision through scale_value().
  f32 scale[ECS_MAX_ENTITIES];
  // Server-side per-player presentation smoothing (replicated to remotes).
  f32 sv_aim[MAX_PLAYERS], sv_phase[MAX_PLAYERS], sv_blend[MAX_PLAYERS];
  f32 sv_sprint[MAX_PLAYERS];
  f32 sv_leanx[MAX_PLAYERS], sv_leanz[MAX_PLAYERS];
  f32 sv_velx[MAX_PLAYERS], sv_velz[MAX_PLAYERS];
  // Client-side per-entity replicated presentation (decoded snapshot).
  u8 r_aim[ECS_MAX_ENTITIES], r_phase[ECS_MAX_ENTITIES];
  u8 r_blend[ECS_MAX_ENTITIES], r_reload[ECS_MAX_ENTITIES];
  u8 r_sprint[ECS_MAX_ENTITIES];
  i8 r_leanx[ECS_MAX_ENTITIES], r_leanz[ECS_MAX_ENTITIES];
} GameState;

_Static_assert(sizeof(GameState) <= 4096, "game state exceeds 4 KiB");
_Static_assert(ECS_MAX_ENTITIES <= 32u, "network bitsets assume 32 entities");
_Static_assert(MAX_PLAYERS <= 8, "visible_players is an 8-bit mask");
_Static_assert(sizeof tom_thumb_3x5 == 88, "unexpected Tom Thumb subset size");

// string helpers

static i32 cstr_len(const char *s) {
  i32 n = 0;
  while (s[n] != '\0')
    ++n;
  return n;
}

static String string_from_cstr(char *s) {
  i32 n = cstr_len(s);
  return (String){(u8 *)s, n, n};
}

static bool string_eq(String a, String b) {
  if (a.length != b.length)
    return false;
  if (a.length == 0)
    return true;
  return memcmp(a.items, b.items, (size_t)a.length) == 0;
}

// math / time / session helpers

static i16 clamp_i16(int value, i16 low, i16 high) {
  if (value < low)
    return low;
  if (value > high)
    return high;
  return (i16)value;
}

static i16 wrap_degrees(int value) {
  value %= 360;
  if (value > 180)
    value -= 360;
  if (value < -179)
    value += 360;
  return (i16)value;
}

static u32 mix32(u32 x) {
  x ^= x >> 16;
  x *= UINT32_C(0x7feb352d);
  x ^= x >> 15;
  x *= UINT32_C(0x846ca68b);
  x ^= x >> 16;
  return x ? x : 1u;
}

static double hires_time_in_seconds(void) {
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static u32 make_session_id(u32 previous) {
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return mix32((u32)now.tv_nsec ^ (u32)now.tv_sec * UINT32_C(0x9e3779b9) ^
               (u32)getpid() * UINT32_C(0x85ebca6b) ^ previous);
}

static bool sequence_newer(u16 a, u16 b) { return (i16)(a - b) > 0; }

static int tick_age(u16 now, u16 then) { return (int)(u16)(now - then); }

static u8 pack_yaw(int deg) {
  return (u8)((wrap_degrees(deg) + 180) * 256 / 360);
}

static int unpack_yaw(u8 packed) { return (int)packed * 360 / 256 - 180; }

// Client interp lag codec: encoded yaw/pitch delta centred on LAG_NEUTRAL
// so 128 means "no turn since last snapshot". Only ever stored in
// lag_yaw/lag_pitch, never in lifetime or weapon state.
#define LAG_NEUTRAL 128

static u8 lag_encode(int delta) {
  return (u8)clamp_i16(delta + LAG_NEUTRAL, 0, 255);
}

static int lag_decode(u8 v) { return (int)v - LAG_NEUTRAL; }

// Weapon state helpers: single source of truth for server simulation and
// client prediction. All weapon state lives on the player entity itself
// (weapon_id/ammo/weapon_cooldown/weapon_reload/weapon_prev arrays indexed
// by entity), never in a parallel per-player slot. All firing, reload and
// snapshot reconciliation goes through here so adding a WeaponDef entry is
// enough for a new weapon.
static bool weapon_entity_valid(u32 e) { return e < ECS_MAX_ENTITIES; }

static void weapon_init_entity(GameState *g, u32 e) {
  if (!weapon_entity_valid(e))
    return;
  g->weapon_id[e] = WEAPON_P99;
  g->weapon_ammo[e] = weapon_def(WEAPON_P99)->mag_size;
  g->weapon_cooldown[e] = 0;
  g->weapon_reload[e] = 0;
  g->weapon_prev[e] = 0;
}

// Authoritative overwrite from a snapshot (fresh entities, remotes).
static void weapon_set_from_snapshot(GameState *g, u32 e, u8 srv_weapon,
                                     u8 srv_ammo, bool srv_reloading) {
  if (!weapon_entity_valid(e))
    return;
  if (srv_weapon >= WEAPON_COUNT)
    srv_weapon = WEAPON_P99;
  const WeaponDef *srv_def = weapon_def(srv_weapon);
  if (srv_ammo > srv_def->mag_size)
    srv_ammo = srv_def->mag_size;
  g->weapon_id[e] = srv_weapon;
  g->weapon_ammo[e] = srv_ammo;
  g->weapon_reload[e] = srv_reloading ? srv_def->reload_ticks : 0;
  g->weapon_cooldown[e] = 0;
  g->weapon_prev[e] = 0;
}

static bool weapon_is_reloading(const GameState *g, u32 e) {
  return weapon_entity_valid(e) && g->weapon_reload[e] != 0;
}

static u8 weapon_ammo(const GameState *g, u32 e) {
  return weapon_entity_valid(e) ? g->weapon_ammo[e] : 0;
}

static u8 weapon_mag_size(const GameState *g, u32 e) {
  u8 id = weapon_entity_valid(e) ? g->weapon_id[e] : WEAPON_P99;
  return weapon_def(id)->mag_size;
}

static bool weapon_holding(const GameState *g, u32 e, bool want) {
  return want && weapon_entity_valid(e) && g->weapon_prev[e] != 0;
}

// Tick reload and fire cooldown. Auto-starts a reload when the mag is empty
// so callers never need their own ammo==0 branches.
static void weapon_tick(GameState *g, u32 e) {
  if (!weapon_entity_valid(e))
    return;
  const WeaponDef *def = weapon_def(g->weapon_id[e]);
  if (g->weapon_reload[e] > 0) {
    if (--g->weapon_reload[e] == 0)
      g->weapon_ammo[e] = def->mag_size;
  } else if (g->weapon_ammo[e] == 0) {
    g->weapon_reload[e] = def->reload_ticks;
  }
  if (g->weapon_cooldown[e] > 0)
    --g->weapon_cooldown[e];
}

static bool weapon_can_fire(const GameState *g, u32 e, bool want) {
  if (!want || !weapon_entity_valid(e))
    return false;
  return g->weapon_cooldown[e] == 0 && g->weapon_reload[e] == 0 &&
         g->weapon_ammo[e] > 0;
}

static void weapon_on_fired(GameState *g, u32 e, bool holding) {
  if (!weapon_entity_valid(e))
    return;
  const WeaponDef *def = weapon_def(g->weapon_id[e]);
  if (g->weapon_ammo[e] > 0)
    --g->weapon_ammo[e];
  g->weapon_cooldown[e] =
      holding ? def->cooldown_hold_ticks : def->cooldown_tap_ticks;
  if (g->weapon_ammo[e] == 0 && g->weapon_reload[e] == 0)
    g->weapon_reload[e] = def->reload_ticks;
}

// Server ran out of bullet slots: rate-limit without consuming ammo.
static void weapon_on_blocked(GameState *g, u32 e) {
  if (!weapon_entity_valid(e))
    return;
  g->weapon_cooldown[e] =
      weapon_def(g->weapon_id[e])->cooldown_tap_ticks;
}

static void weapon_set_prev(GameState *g, u32 e, bool want) {
  if (weapon_entity_valid(e))
    g->weapon_prev[e] = (u8)(want ? 1 : 0);
}

// Shared recoil step so client prediction (view_pitch) and server
// confirmation (authoritative entity/input pitch) stay in sync.
static i16 weapon_recoil_pitch(i16 pitch, u8 weapon_id, bool holding) {
  const WeaponDef *def = weapon_def(weapon_id);
  int kick = holding ? (int)def->recoil_pitch_hold_deg
                     : (int)def->recoil_pitch_tap_deg;
  return clamp_i16((int)pitch + kick, -75, 75);
}

// Per-weapon imprecision in degrees. Hip is deliberately loose, ADS is
// tight but never perfectly exact.
static f32 weapon_spread_deg(u8 weapon_id, bool aiming) {
  const WeaponDef *def = weapon_def(weapon_id);
  return aiming ? def->spread_aim_deg : def->spread_hip_deg;
}

static f32 weapon_hip_fov(u8 weapon_id) {
  return weapon_def(weapon_id)->hip_fov;
}

static f32 weapon_aim_fov(u8 weapon_id) {
  return weapon_def(weapon_id)->aim_fov;
}

static f32 weapon_aim_sens_scale(u8 weapon_id) {
  return weapon_def(weapon_id)->aim_sens_scale;
}

static f32 weapon_aim_move_scale(u8 weapon_id) {
  return weapon_def(weapon_id)->aim_move_scale;
}

// Reload progress 0..1 (0 when not reloading) so the viewmodel can play
// one smooth whip across the reload.
static f32 weapon_reload_progress(const GameState *g, u32 e) {
  if (!weapon_entity_valid(e) || g->weapon_reload[e] == 0)
    return 0.0f;
  const WeaponDef *def = weapon_def(g->weapon_id[e]);
  if (def->reload_ticks == 0)
    return 0.0f;
  f32 t = 1.0f - (f32)g->weapon_reload[e] / (f32)def->reload_ticks;
  if (t < 0.0f)
    t = 0.0f;
  if (t > 1.0f)
    t = 1.0f;
  return t;
}

// ---- shared presentation tick/pose helpers (single source of truth) ----
// The client (local prediction) and the server (per-player smoothing for
// replication) both step through these, and both render paths pose through
// the ones below. No private copies anywhere.
static void present_blend_tick(f32 *b, bool want, f32 rate, f32 dt) {
  f32 k = 1.0f - expf(-rate * dt);
  *b += ((want ? 1.0f : 0.0f) - *b) * k;
  if (*b < 0.001f && !want)
    *b = 0.0f;
  if (*b > 0.999f && want)
    *b = 1.0f;
}

static void present_walk_tick(f32 *phase, f32 *blend, i8 mx, i8 mz, f32 dt) {
  bool moving = mx != 0 || mz != 0;
  present_blend_tick(blend, moving, 8.0f, dt);
  if (moving) {
    *phase += dt * 11.0f;
    if (*phase > 2.0f * PI)
      *phase -= 2.0f * PI;
  }
}

static void present_lean_tick(f32 *pos, f32 *vel, f32 target, f32 dt) {
  *vel += ((target - *pos) * 90.0f - *vel * 14.0f) * dt;
  *pos += *vel * dt;
}

// Sprint wants forward motion without ADS held (ADS wins if both held).
static bool present_sprint_want(u8 buttons, i8 mx, i8 mz) {
  (void)mx;
  return (buttons & BTN_SPRINT) != 0 && mz > 0 &&
         (buttons & BTN_AIM) == 0;
}

static bool present_aim_want(u8 buttons, bool reloading) {
  return (buttons & BTN_AIM) != 0 && !reloading;
}

// Walk bob offsets; ads_damp is (1 - 0.5*aim) on both render paths.
static void present_bob_offsets(f32 phase, f32 blend, f32 ads_damp, f32 *x,
                                f32 *y, f32 *z, f32 *roll) {
  f32 s = blend * ads_damp;
  *x = sinf(phase) * 0.014f * s;
  *y = -fabsf(cosf(phase)) * 0.012f * s;
  *z = sinf(phase * 2.0f) * 0.008f * s;
  *roll = sinf(phase) * 2.5f * s;
}

// Steady lean from smoothed strafe/push (screen-right is viewmodel -X).
static void present_lean_offsets(f32 sx, f32 sz, f32 *ox, f32 *oz) {
  *ox = -sx * 0.045f;
  *oz = sz * 0.05f;
}

static u8 present_q01(f32 v) {
  if (v <= 0.0f)
    return 0;
  if (v >= 1.0f)
    return 255;
  return (u8)(v * 255.0f + 0.5f);
}

static i8 present_q11(f32 v) {
  if (v <= -1.0f)
    return -127;
  if (v >= 1.0f)
    return 127;
  return (i8)(v * 127.0f);
}

// Reload progress quantized: 0 means not reloading, otherwise 1..255.
static u8 present_reload_q(bool reloading, f32 t) {
  if (!reloading)
    return 0;
  if (t < 0.0f)
    t = 0.0f;
  if (t > 1.0f)
    t = 1.0f;
  return (u8)(1 + t * 254.0f);
}

static f32 present_unq01(u8 q) { return (f32)q / 255.0f; }

static f32 present_unq11(i8 q) { return (f32)q / 127.0f; }

// ---- generic stale-snapshot guards (source fix) ----
// Reconciliation policy, single source of truth for ALL predicted state.
// - Authoritative state (remotes, cubes, casings, presentation, names):
//   overwrite from the snapshot. Global tick ordering in
//   net_validate_snapshot_fields guarantees snapshots apply in order.
// - Predicted state (local weapon ammo/reload/id today; local position or
//   any future client-predicted field): NEVER overwrite directly from a
//   snapshot. A snapshot is stale by RTT: the server hadn't seen our latest
//   inputs when it was taken (see acknowledged_sequence, which the old code
//   discarded). Blind overwrite regresses fresh prediction (the mag-dump
//   15->14->0 wipe). Route every predicted field through the helpers below:
//   (a) monotonic-down counters (ammo, health, ...): adopt only if server is
//       lower (server fired extra shots we haven't, e.g. delayed release).
//       Ignore higher (server hasn't seen our shots yet).
//   (b) ack proof: predicted_is_stale(dirty_seq, snap_ack) is true while the
//       server hasn't acked our last change -> must not regress to it.
//   (c) forward-only progress (reload remaining): adopt the minimum
//       remaining, never extend.
// Adding a new predicted field means adding a dirty_seq for it and using
// these helpers, not a new ad-hoc guard.
static bool predicted_is_stale(u16 dirty_seq, u16 snap_ack) {
  return sequence_newer(dirty_seq, snap_ack);
}

static bool reconcile_should_adopt_lower_u8(u8 local_value, u8 server_value) {
  return server_value < local_value;
}

// Decode the wire reload_q (0 = not reloading, else 1..255) to a remaining
// tick count so both-reloading sides can take the minimum (forward only).
// Unknown/zero progress while reloading means "just started": return the
// full duration so we never fast-forward on bad data.
static u16 reload_remaining_from_q(u8 reload_q, u16 reload_ticks) {
  if (reload_q == 0)
    return 0;
  if (reload_ticks == 0)
    return 0;
  return (u16)(((u32)reload_ticks * (u32)(255u - reload_q) + 127u) / 254u);
}

// Reconcile a snapshot weapon state onto a client entity. Remote entities
// are fully authoritative. The local entity keeps its prediction and only
// moves towards the server via the generic guards above: monotonic ammo,
// ack-proof reload start, forward-only reload progress, ack-gated weapon id.
static void weapon_apply_snapshot(GameState *g, u32 e, u8 srv_weapon,
                                  u8 srv_ammo, bool srv_reloading,
                                  u8 srv_reload_q, u16 snap_ack) {
  if (!weapon_entity_valid(e))
    return;
  if (srv_weapon >= WEAPON_COUNT)
    srv_weapon = WEAPON_P99;
  const WeaponDef *srv_def = weapon_def(srv_weapon);
  if (srv_ammo > srv_def->mag_size)
    srv_ammo = srv_def->mag_size;
  bool is_local = g->local_player < MAX_PLAYERS && g->owner[e] == g->local_player;
  if (!is_local) {
    weapon_set_from_snapshot(g, e, srv_weapon, srv_ammo, srv_reloading);
    return;
  }
  // Predicted path: gate the weapon id itself. While our switch hasn't been
  // acked, keep predicting with the local id; once acked, adopt the server
  // id (today always identical; future pickups need this to avoid snap-back).
  u16 dirty = g->weapon_dirty_seq[e];
  bool stale = predicted_is_stale(dirty, snap_ack);
  if (!stale && g->weapon_id[e] != srv_weapon) {
    g->weapon_id[e] = srv_weapon;
    g->weapon_dirty_seq[e] = snap_ack;
    dirty = snap_ack;
    stale = false;
  }
  const WeaponDef *def = weapon_def(g->weapon_id[e]);
  if (srv_ammo > def->mag_size)
    srv_ammo = def->mag_size;
  if (g->weapon_ammo[e] > def->mag_size) {
    g->weapon_ammo[e] = srv_ammo;
    g->weapon_dirty_seq[e] = snap_ack;
    dirty = snap_ack;
    stale = predicted_is_stale(dirty, snap_ack);
  }
  if (srv_reloading) {
    if (g->weapon_reload[e] != 0) {
      // Both reloading: forward-only via remaining minimum so a stale
      // snapshot can never extend the reload. Ammo stays empty.
      g->weapon_ammo[e] = 0;
      u16 local_remaining = g->weapon_reload[e];
      u16 srv_remaining =
          reload_remaining_from_q(srv_reload_q, def->reload_ticks);
      if (srv_reload_q != 0 && srv_remaining < local_remaining)
        g->weapon_reload[e] = srv_remaining;
    } else if (g->weapon_ammo[e] == 0) {
      // Local emptied but hasn't started its reload yet (the one-tick gap
      // before weapon_tick auto-starts it): adopt the server reload.
      g->weapon_ammo[e] = 0;
      g->weapon_reload[e] = def->reload_ticks;
      g->weapon_dirty_seq[e] = snap_ack;
    } else if (stale) {
      // Local has ammo and isn't reloading, and the server hasn't acked our
      // last weapon change (refill + predicted shots): this reloading
      // snapshot predates our refill and is stale. Ignore it instead of
      // wiping the fresh mag back to 0. This subsumes the old
      // `ammo == mag_size` guard, which only covered the exact refill tick.
    } else {
      // Fresh (server saw our refill yet still says reloading): genuine new
      // cycle the local missed. Adopt it to converge.
      g->weapon_ammo[e] = 0;
      g->weapon_reload[e] = def->reload_ticks;
      g->weapon_dirty_seq[e] = snap_ack;
    }
  } else if (g->weapon_reload[e] != 0) {
    // Server done while local still reloading: fast-forward ONLY if fresh
    // (server has acked our reload start, so it truly finished ahead and we
    // started late). If stale, the server simply hasn't emptied yet (it is
    // behind: e.g. local 0/118 reloading while server still shows 1/0 live):
    // keep reloading, else we snap back out of reload (0->1) and wipe again
    // on the next snapshot. Ammo stays empty while reloading either way.
    if (!stale) {
      // Fresh: client started late, jump to the server state (done + its
      // ammo, which may already include post-refill shots). Forward only.
      g->weapon_ammo[e] = srv_ammo;
      g->weapon_reload[e] = 0;
      g->weapon_dirty_seq[e] = snap_ack;
    } else {
      g->weapon_ammo[e] = 0;
    }
  } else if (reconcile_should_adopt_lower_u8(g->weapon_ammo[e], srv_ammo)) {
    // Both live: server fired extra shots we haven't (delayed release).
    // Adopt the lower count; ignore higher (server hasn't seen our shots).
    g->weapon_ammo[e] = srv_ammo;
    g->weapon_dirty_seq[e] = snap_ack;
  }
}

// string validation

static bool valid_port(String text, u16 *port) {
  unsigned long value = 0;
  if (text.length <= 0)
    return false;
  for (i32 i = 0; i < text.length; ++i) {
    u8 c = text.items[i];
    if (!CHAR_IS_DIGIT(c))
      return false;
    value = value * 10u + (unsigned long)(c - '0');
    if (value > 65535u)
      return false;
  }
  if (value == 0)
    return false;
  *port = (u16)value;
  return true;
}

static bool valid_player_name(String text, String *out) {
  if (text.length != PLAYER_NAME_LENGTH)
    return false;
  if (!out || !out->items || out->capacity < PLAYER_NAME_LENGTH)
    return false;
  for (i32 i = 0; i < PLAYER_NAME_LENGTH; ++i) {
    u8 c = (u8)CHAR_TO_UPPER(text.items[i]);
    if (!CHAR_IS_UPPER(c))
      return false;
    out->items[i] = c;
  }
  out->length = PLAYER_NAME_LENGTH;
  return true;
}

static bool valid_wire_name(String name) {
  if (name.length != PLAYER_NAME_LENGTH)
    return false;
  for (i32 i = 0; i < PLAYER_NAME_LENGTH; ++i)
    if (!CHAR_IS_UPPER(name.items[i]))
      return false;
  return true;
}

static bool parse_ipv4(String text, struct in_addr *address) {
  char tmp[64];
  String use = text;
  if (string_eq(text, STR("localhost")))
    use = STR("127.0.0.1");
  if (use.length <= 0 || use.length >= (i32)sizeof(tmp))
    return false;
  for (i32 i = 0; i < use.length; ++i)
    tmp[i] = (char)use.items[i];
  tmp[use.length] = '\0';
  return inet_pton(AF_INET, tmp, address) == 1;
}

static void socket_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Packet framing (exact size + magic + type). Semantic checks live in
// net_validate_*_fields. All decoders zero-fill *out and copy field-wise,
// so wire padding never propagates and struct padding never leaks.
static bool net_packet_ok(const u8 *buf, i32 n, u8 type, i32 size) {
  return n == size && n >= 2 && buf[0] == NET_MAGIC && buf[1] == type;
}

static bool net_seq_fresh(u16 seq, bool got, u16 last) {
  return !got || sequence_newer(seq, last);
}

static bool net_validate_chat_fields(String name, u8 length, u16 seq, bool got,
                                     u16 last) {
  if (length > CHAT_LENGTH)
    return false;
  if (!valid_wire_name(name))
    return false;
  if (!net_seq_fresh(seq, got, last))
    return false;
  return true;
}

static bool net_validate_input_fields(u32 session, String name) {
  if (session == 0)
    return false;
  if (!valid_wire_name(name))
    return false;
  return true;
}

static bool net_validate_snapshot_fields(const NetSnapshot *snap,
                                         u32 expected_session, bool got,
                                         u16 last_tick) {
  if (snap->count > NET_ENTITY_LIMIT)
    return false;
  if (snap->local_player != NET_PACKET_NONE &&
      snap->local_player >= MAX_PLAYERS)
    return false;
  if (snap->session != expected_session)
    return false;
  if (!net_seq_fresh(snap->tick, got, last_tick))
    return false;
  return true;
}

// Framing check for a dense snapshot: exact size for the claimed count.
static bool net_snapshot_wire_ok(const u8 *buf, i32 n) {
  if (!buf || n < 4 || buf[0] != NET_MAGIC ||
      buf[1] != NET_PACKET_SNAPSHOT)
    return false;
  u8 count = buf[NET_OFF_COUNT];
  if (count > NET_ENTITY_LIMIT)
    return false;
  i32 want = net_snapshot_wire_size(count);
  return want >= 0 && n == want && (size_t)n <= sizeof(NetSnapshot);
}

static void net_write_snapshot_header(u8 *out, const NetSnapshot *snap) {
  out[0] = snap->magic;
  out[1] = snap->type;
  out[2] = snap->local_player;
  out[3] = snap->count;
  memcpy(out + 4, &snap->session, sizeof(snap->session));
  memcpy(out + 8, &snap->tick, sizeof(snap->tick));
  memcpy(out + 10, &snap->acknowledged_sequence,
         sizeof(snap->acknowledged_sequence));
}

static void net_read_snapshot_header(const u8 *buf, NetSnapshot *out) {
  out->magic = buf[0];
  out->type = buf[1];
  out->local_player = buf[2];
  out->count = buf[NET_OFF_COUNT];
  memcpy(&out->session, buf + 4, sizeof(out->session));
  memcpy(&out->tick, buf + 8, sizeof(out->tick));
  memcpy(&out->acknowledged_sequence, buf + 10,
         sizeof(out->acknowledged_sequence));
}

// Returns wire length, or -1 when count invalid / buffer too small.
static i32 net_snapshot_encode(const NetSnapshot *snap, u8 *out, i32 out_cap) {
  if (!snap || !out || snap->count > NET_ENTITY_LIMIT)
    return -1;
  i32 want = net_snapshot_wire_size(snap->count);
  if (want < 0 || out_cap < want)
    return -1;
  net_write_snapshot_header(out, snap);
  if (snap->count > 0)
    memcpy(out + NET_SNAPSHOT_HEADER_SIZE, snap->entities,
           (size_t)snap->count * sizeof(NetEntity));
  memcpy(out + NET_SNAPSHOT_HEADER_SIZE +
             (size_t)snap->count * sizeof(NetEntity),
         snap->names, NET_SNAPSHOT_NAMES_SIZE);
  return want;
}

// Validates framing, then expands into a zero-filled struct.
// Never partially fills *out. Semantic checks still need
// net_validate_snapshot_fields.
static bool net_snapshot_decode(const u8 *buf, i32 n, NetSnapshot *out) {
  if (!buf || !out || !net_snapshot_wire_ok(buf, n))
    return false;
  u8 count = buf[NET_OFF_COUNT];
  *out = (NetSnapshot){0};
  net_read_snapshot_header(buf, out);
  out->count = count;
  if (count > 0)
    memcpy(out->entities, buf + NET_SNAPSHOT_HEADER_SIZE,
           (size_t)count * sizeof(NetEntity));
  memcpy(out->names, buf + NET_SNAPSHOT_HEADER_SIZE +
                         (size_t)count * sizeof(NetEntity),
         NET_SNAPSHOT_NAMES_SIZE);
  return true;
}

// Fixed-size decoders: exact-size framing + zero-fill + field copy.
// Wire pad bytes are ignored, struct pad bytes stay zero.
static bool net_input_decode(const u8 *buf, i32 n, NetInputPacket *out) {
  if (!buf || !out || n != (i32)sizeof(NetInputPacket))
    return false;
  if (buf[0] != NET_MAGIC ||
      (buf[1] != NET_PACKET_INPUT && buf[1] != NET_PACKET_DISCONNECT))
    return false;
  *out = (NetInputPacket){0};
  out->magic = buf[0];
  out->type = buf[1];
  out->claimed_player = buf[2];
  out->buttons = buf[3];
  memcpy(&out->session, buf + 4, sizeof(out->session));
  memcpy(&out->sequence, buf + 8, sizeof(out->sequence));
  out->move_x = (i8)buf[10];
  out->move_z = (i8)buf[11];
  memcpy(&out->aim_yaw, buf + 12, sizeof(out->aim_yaw));
  memcpy(&out->aim_pitch, buf + 14, sizeof(out->aim_pitch));
  memcpy(out->name, buf + 16, PLAYER_NAME_LENGTH);
  return true;
}

static bool net_chat_decode(const u8 *buf, i32 n, NetChatPacket *out) {
  if (!buf || !out)
    return false;
  if (!net_packet_ok(buf, n, NET_PACKET_CHAT, (i32)sizeof(NetChatPacket)))
    return false;
  *out = (NetChatPacket){0};
  out->magic = buf[0];
  out->type = buf[1];
  out->player = buf[2];
  out->length = buf[3];
  if (out->length > CHAT_LENGTH)
    return false;
  memcpy(&out->session, buf + 4, sizeof(out->session));
  memcpy(&out->sequence, buf + 8, sizeof(out->sequence));
  memcpy(out->name, buf + 10, PLAYER_NAME_LENGTH);
  memcpy(out->text, buf + 10 + PLAYER_NAME_LENGTH, out->length);
  return true;
}

static void net_sanitize_chat_text(String text) {
  for (i32 i = 0; i < text.length; ++i) {
    u8 c = text.items[i];
    if (!CHAR_IS_CHAT(c))
      text.items[i] = ' ';
  }
}

// notice helpers

static void notice_set(GameState *g, String s) {
  i32 n = s.length < NOTICE_LENGTH ? s.length : NOTICE_LENGTH;
  if (n > 0)
    memcpy(g->notice, s.items, (size_t)n);
  g->notice_length = (u8)n;
  g->notice_ticks = SIM_HZ * 5;
}

static void notice_name_status(GameState *g, String name, String status) {
  u8 tmp[NOTICE_LENGTH];
  i32 pos = 0;
  for (i32 i = 0; i < name.length && pos < NOTICE_LENGTH; ++i)
    tmp[pos++] = name.items[i];
  if (pos < NOTICE_LENGTH)
    tmp[pos++] = ' ';
  for (i32 i = 0; i < status.length && pos < NOTICE_LENGTH; ++i)
    tmp[pos++] = status.items[i];
  if (pos > 0)
    memcpy(g->notice, tmp, (size_t)pos);
  g->notice_length = (u8)pos;
  g->notice_ticks = SIM_HZ * 5;
}

static void notice_chat_msg(GameState *g, String name, String text) {
  u8 tmp[NOTICE_LENGTH];
  i32 pos = 0;
  for (i32 i = 0; i < name.length && pos < NOTICE_LENGTH; ++i)
    tmp[pos++] = name.items[i];
  if (pos < NOTICE_LENGTH)
    tmp[pos++] = ':';
  if (pos < NOTICE_LENGTH)
    tmp[pos++] = ' ';
  for (i32 i = 0; i < text.length && pos < NOTICE_LENGTH; ++i)
    tmp[pos++] = text.items[i];
  if (pos > 0)
    memcpy(g->notice, tmp, (size_t)pos);
  g->notice_length = (u8)pos;
  g->notice_ticks = SIM_HZ * 5;
}

// GameState string views

static String game_requested_name(GameState *g) {
  return (String){g->requested_name, PLAYER_NAME_LENGTH, PLAYER_NAME_LENGTH};
}

static String game_player_name(GameState *g, u32 p) {
  return (String){g->player_name[p], PLAYER_NAME_LENGTH, PLAYER_NAME_LENGTH};
}

static String game_chat_input(GameState *g) {
  return (String){g->chat_input, (i32)g->chat_length, CHAT_LENGTH};
}

static String game_notice(GameState *g) {
  return (String){g->notice, (i32)g->notice_length, NOTICE_LENGTH};
}

// entity lifecycle

static void game_zero(GameState *g) {
  *g = (GameState){.local_player = NET_PACKET_NONE};
  memset(g->network_entity, NET_PACKET_NONE, sizeof(g->network_entity));
  memset(g->player_entity, NET_PACKET_NONE, sizeof(g->player_entity));
}

static void clear_entity_data(GameState *g, u32 e) {
  g->px[e] = g->py[e] = g->pz[e] = 0;
  g->vx[e] = g->vy[e] = g->vz[e] = 0;
  g->yaw[e] = g->pitch[e] = 0;
  g->kind[e] = g->lifetime[e] = g->owner[e] = 0;
  g->lag_yaw[e] = g->lag_pitch[e] = LAG_NEUTRAL;
  g->flash[e] = g->held_by[e] = 0;
  g->scale[e] = 1.0f;
  g->weapon_id[e] = WEAPON_P99;
  g->weapon_ammo[e] = 0;
  g->weapon_cooldown[e] = 0;
  g->weapon_prev[e] = 0;
  g->weapon_reload[e] = 0;
  g->weapon_dirty_seq[e] = 0;
  g->r_aim[e] = g->r_phase[e] = g->r_blend[e] = 0;
  g->r_reload[e] = g->r_sprint[e] = 0;
  g->r_leanx[e] = g->r_leanz[e] = 0;
}

static void add_kind_components(GameState *g, u32 e, u8 kind) {
  ecs_add(&g->ecs, e, COMP_POSITION);
  ecs_add(&g->ecs, e, COMP_RENDERABLE);
  ecs_add(&g->ecs, e, COMP_NETWORKED);
  if (kind == KIND_CUBE) {
    ecs_add(&g->ecs, e, COMP_SCALE);
  } else if (kind == KIND_PLAYER) {
    ecs_add(&g->ecs, e, COMP_VELOCITY);
    ecs_add(&g->ecs, e, COMP_AIM);
    ecs_add(&g->ecs, e, COMP_PLAYER);
  } else if (kind == KIND_BULLET) {
    ecs_add(&g->ecs, e, COMP_VELOCITY);
    ecs_add(&g->ecs, e, COMP_PROJECTILE);
    ecs_add(&g->ecs, e, COMP_LIFETIME);
  } else if (kind == KIND_CASING) {
    ecs_add(&g->ecs, e, COMP_VELOCITY);
    ecs_add(&g->ecs, e, COMP_LIFETIME);
  } else if (kind == KIND_IMPACT_MARK) {
    ecs_add(&g->ecs, e, COMP_LIFETIME);
  }
}

static u32 count_kind(const GameState *g, u8 kind) {
  u32 comps[] = {COMP_RENDERABLE};
  EcsQuery q = ecs_query(&g->ecs, comps, 1);
  u32 n = 0;
  while (ecs_next(&q))
    if (g->kind[q.entity] == kind)
      ++n;
  return n;
}

static void destroy_oldest(GameState *g, u8 kind) {
  u32 comps[] = {COMP_LIFETIME};
  EcsQuery q = ecs_query(&g->ecs, comps, 1);
  u32 oldest = ECS_INVALID_ENTITY;
  u8 best = UINT8_MAX;
  while (ecs_next(&q)) {
    u32 e = q.entity;
    if (g->kind[e] == kind && g->lifetime[e] < best) {
      best = g->lifetime[e];
      oldest = e;
    }
  }
  if (oldest != ECS_INVALID_ENTITY)
    ecs_destroy(&g->ecs, oldest);
}

static u32 take_entity(GameState *g) {
  u32 e = ecs_create(&g->ecs);
  if (e != ECS_INVALID_ENTITY)
    return e;
  destroy_oldest(g, KIND_IMPACT_MARK);
  e = ecs_create(&g->ecs);
  if (e != ECS_INVALID_ENTITY)
    return e;
  destroy_oldest(g, KIND_CASING);
  return ecs_create(&g->ecs);
}

static u32 create_cube(GameState *g, u32 index) {
  u32 e = ecs_create(&g->ecs);
  if (e == ECS_INVALID_ENTITY)
    return e;
  clear_entity_data(g, e);
  g->px[e] = (i16)(((int)((index * 7u + 3u) % 17u) - 8) * POS_SCALE);
  g->py[e] = POS_SCALE / 2;
  g->pz[e] = (i16)(((int)((index * 11u + 5u) % 17u) - 8) * POS_SCALE);
  g->kind[e] = KIND_CUBE;
  add_kind_components(g, e, KIND_CUBE);
  return e;
}

static void game_init(GameState *g) {
  // Server-only world init: authoritative cubes live here.
  game_zero(g);
  for (u32 i = 0; i < MAX_CUBES; ++i)
    (void)create_cube(g, i);
}

// Client world lifecycle (proper place for world resets, not a patch in
// apply_snapshot). The client world is purely snapshot-driven: it starts
// empty and is populated only by the server. game_init() is server-only.
static void client_clear_world(GameState *g) {
  for (u32 e = 0; e < ECS_MAX_ENTITIES; ++e) {
    if (ecs_alive(&g->ecs, e))
      ecs_destroy(&g->ecs, e);
    g->scale[e] = 1.0f;
  }
  memset(g->network_entity, NET_PACKET_NONE, sizeof(g->network_entity));
  g->snapshot_seen = 0;
}

static void client_init(GameState *g) {
  game_zero(g);
  client_clear_world(g);
}

// spatial / gun helpers

static f32 scale_value(const GameState *g, u32 e) {
  if (e >= ECS_MAX_ENTITIES || !ecs_has(&g->ecs, e, COMP_SCALE))
    return 1.0f;
  f32 s = g->scale[e];
  if (!(s > 0.01f))
    return 1.0f;
  if (s < CUBE_SCALE_MIN)
    return CUBE_SCALE_MIN;
  if (s > CUBE_SCALE_MAX)
    return CUBE_SCALE_MAX;
  return s;
}

static bool cube_overlap(const GameState *g, i16 x, i16 z, i16 radius) {
  u32 comps[] = {COMP_POSITION, COMP_RENDERABLE};
  EcsQuery q = ecs_query(&g->ecs, comps, 2);
  while (ecs_next(&q)) {
    u32 e = q.entity;
    if (g->kind[e] == KIND_CUBE && g->held_by[e] == 0) {
      int half = (int)((f32)(POS_SCALE / 2) * scale_value(g, e));
      if (abs((int)x - g->px[e]) < half + radius &&
          abs((int)z - g->pz[e]) < half + radius)
        return true;
    }
  }
  return false;
}

static void aim_dir(i16 yaw_deg, i16 pitch_deg, Vector3 *out) {
  f32 yaw = (f32)yaw_deg * DEG2RAD;
  f32 pitch = (f32)pitch_deg * DEG2RAD;
  f32 h = cosf(pitch);
  out->x = sinf(yaw) * h;
  out->y = sinf(pitch);
  out->z = cosf(yaw) * h;
}

static void gun_basis(const GameState *g, u32 e, Vector3 *f, Vector3 *r,
                      Vector3 *u) {
  aim_dir(g->yaw[e], g->pitch[e], f);
  f32 yaw = (f32)g->yaw[e] * DEG2RAD;
  *r = (Vector3){-cosf(yaw), 0.0f, sinf(yaw)};
  *u = (Vector3){r->y * f->z - r->z * f->y, r->z * f->x - r->x * f->z,
                 r->x * f->y - r->y * f->x};
}

static Vector3 gun_point(const GameState *g, u32 e, Vector3 f, Vector3 r,
                         Vector3 u, Vector3 off) {
  return (Vector3){
      (f32)g->px[e] / POS_SCALE + f.x * off.x + r.x * off.y + u.x * off.z,
      (f32)g->py[e] / POS_SCALE + 1.5f + f.y * off.x + r.y * off.y +
          u.y * off.z,
      (f32)g->pz[e] / POS_SCALE + f.z * off.x + r.z * off.y + u.z * off.z};
}

#endif
