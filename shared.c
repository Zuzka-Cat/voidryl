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

typedef struct {
  i16 x, y, z;
} IVec3;

typedef struct {
  i16 yaw, pitch;
} Aim;

typedef struct {
  u8 yaw, pitch;
} LagAim;

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
  Vector2 hip_off;
  f32 ads_cant_deg;
  f32 ads_pitch_deg;
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
            .hip_off = {0.06f, -0.18f},
            .ads_cant_deg = -12.0f,
            .ads_pitch_deg = -3.0f,
            .sprint_tilt_deg = -32.0f,
        },
};

static const WeaponDef *weapon_def(u8 id) {
  return &WEAPON_DEFS[id < WEAPON_COUNT ? id : WEAPON_P99];
}

typedef struct ShellDef {
  f32 len;
  f32 radius;
  f32 rim_radius;
  Vector3 eject;
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
            .eject = {2.4f, 1.9f, 0.55f},
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
#define BULLET_SPEED 64.0f
#define ARENA_POS (15 * POS_SCALE)

#define HOLD_BASE_DIST 2.0f
#define HOLD_PITCH_STEP 0.02f
#define HOLD_MIN_DIST 1.0f
#define HOLD_MAX_DIST 4.0f
#define CUBE_SCALE_MIN 0.12f
#define CUBE_SCALE_MAX 2.0f

#define CUBE_VIS_X 0.9f
#define CUBE_VIS_Y 1.0f
#define CUBE_VIS_Z 0.9f
#define PLAYER_BODY_X 0.50f
#define PLAYER_BODY_Y 1.36f
#define PLAYER_BODY_Z 0.30f
#define PLAYER_HEAD_Y 1.44f
#define PLAYER_HEAD_S 0.26f
#define PLAYER_HIT_RADIUS_M 0.30f
#define PLAYER_HIT_HEIGHT_M 1.58f

#define EYE_HEIGHT_M 1.5f
#define GUN_HIP_SIDE_M 0.36f
#define GUN_HIP_UP_M 0.98f
#define GUN_HIP_FWD_M 0.24f
#define GUN_CENTER_X_M 0.32f
#define GUN_MUZZLE_Z_M 0.72f
#define SHOT_SPAWN_M 0.20f
#define MARK_NUDGE 1.0f
#define SIGHT_SKY_DIST_M 12.0f
#define SIGHT_MAX_DIST_M 60.0f

static int clamp_int(int v, int lo, int hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

static f32 clamp_float(f32 v, f32 lo, f32 hi) {
  return fminf(fmaxf(v, lo), hi);
}

static f32 hold_dist_for_pitch(i16 pitch_deg) {
  return clamp_float(HOLD_BASE_DIST + (f32)pitch_deg * HOLD_PITCH_STEP,
                     HOLD_MIN_DIST, HOLD_MAX_DIST);
}

static u8 encode_unorm8(f32 t) {
  return (u8)clamp_int((int)lroundf(clamp_float(t, 0.0f, 1.0f) * 255.0f), 0,
                       255);
}

static f32 decode_unorm8(u8 q) { return (f32)q / 255.0f; }

static u8 encode_centered8(int v) { return (u8)clamp_int(v + 128, 0, 255); }

static int decode_centered8(u8 q) { return (int)q - 128; }

static u8 encode_range8(f32 t, u8 lo, u8 hi) {
  return (u8)(lo +
              (u8)lroundf(decode_unorm8(encode_unorm8(t)) * (f32)(hi - lo)));
}

static u8 scale_encode(f32 s) {
  s = clamp_float(s, CUBE_SCALE_MIN, CUBE_SCALE_MAX);
  return (u8)clamp_int((int)lroundf(s * 64.0f), 1, 255);
}

static f32 scale_decode(u8 q) {
  if (q == 0)
    return 1.0f;
  return (f32)q / 64.0f;
}

enum MarkNormal {
  MARK_NORM_NONE = 0,
  MARK_NORM_PX = 1,
  MARK_NORM_NX = 2,
  MARK_NORM_PY = 3,
  MARK_NORM_NY = 4,
  MARK_NORM_PZ = 5,
  MARK_NORM_NZ = 6
};

static u8 mark_code_for_vec(Vector3 n) {
  float ax = fabsf(n.x), ay = fabsf(n.y), az = fabsf(n.z);
  if (ax >= ay && ax >= az)
    return n.x > 0.0f ? MARK_NORM_PX : MARK_NORM_NX;
  if (az >= ax && az >= ay)
    return n.z > 0.0f ? MARK_NORM_PZ : MARK_NORM_NZ;
  return n.y > 0.0f ? MARK_NORM_PY : MARK_NORM_NY;
}

static Vector3 mark_normal_vec(u8 code) {
  switch (code) {
  case MARK_NORM_PX:
    return (Vector3){1.0f, 0.0f, 0.0f};
  case MARK_NORM_NX:
    return (Vector3){-1.0f, 0.0f, 0.0f};
  case MARK_NORM_NY:
    return (Vector3){0.0f, -1.0f, 0.0f};
  case MARK_NORM_PZ:
    return (Vector3){0.0f, 0.0f, 1.0f};
  case MARK_NORM_NZ:
    return (Vector3){0.0f, 0.0f, -1.0f};
  case MARK_NORM_PY:
  default:
    return (Vector3){0.0f, 1.0f, 0.0f};
  }
}

#define MARK_STR_LEVELS 4u
#define MARK_EXTRA_STR_SHIFT 3
#define MARK_EXTRA_MAT_SHIFT 5
enum MarkMaterial { MARK_MAT_PURPLE = 0, MARK_MAT_MID = 1 };

static u8 mark_extra_pack(u8 norm, u8 strength, u8 mat) {
  return (u8)((norm & 0x07u) | ((strength & 0x03u) << MARK_EXTRA_STR_SHIFT) |
              ((mat & 0x03u) << MARK_EXTRA_MAT_SHIFT));
}

static u8 mark_ex_norm(u8 ex) { return (u8)(ex & 0x07u); }

static u8 mark_ex_str(u8 ex) {
  return (u8)((ex >> MARK_EXTRA_STR_SHIFT) & 0x03u);
}

static u8 mark_ex_mat(u8 ex) {
  return (u8)((ex >> MARK_EXTRA_MAT_SHIFT) & 0x03u);
}

enum EntityKind {
  KIND_CUBE = 1,
  KIND_PLAYER,
  KIND_BULLET,
  KIND_IMPACT_MARK,
  KIND_CASING,
  KIND_FLOOR
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
  COMP_SCALE,
  COMP_SOLID
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
  u8 reserved[3];
} NetInputPacket;

typedef struct NetEntity {
  u8 entity, kind, owner, flags, extra, weapon, ammo, reloading;
  u8 reload_q;
  u8 scale;
  i16 x, y, z, yaw, pitch;
} NetEntity;

typedef struct NetChatPacket {
  u8 magic, type, player, length;
  u32 session;
  u16 sequence;
  u8 name[PLAYER_NAME_LENGTH];
  u8 text[CHAT_LENGTH];
  u8 reserved[3];
} NetChatPacket;

typedef struct NetSnapshot {
  u8 magic, type, local_player, count;
  u32 session;
  u16 tick, acknowledged_sequence;
  NetEntity entities[NET_ENTITY_LIMIT];
  u8 names[MAX_PLAYERS][PLAYER_NAME_LENGTH];
} NetSnapshot;

#define NET_SNAPSHOT_HEADER_SIZE (offsetof(NetSnapshot, entities))
#define NET_SNAPSHOT_NAMES_SIZE (MAX_PLAYERS * PLAYER_NAME_LENGTH)
#define NET_MAX_DATAGRAM sizeof(NetSnapshot)
#define NET_INPUT_WIRE_SIZE sizeof(NetInputPacket)
#define NET_CHAT_WIRE_SIZE sizeof(NetChatPacket)

static i32 net_snapshot_wire_size(u8 count) {
  if (count > NET_ENTITY_LIMIT)
    return -1;
  return (i32)(NET_SNAPSHOT_HEADER_SIZE + (i32)count * (i32)sizeof(NetEntity) +
               NET_SNAPSHOT_NAMES_SIZE);
}

#define ECS_MAX_ENTITIES 32u
#define ECS_MAX_COMPONENTS 10u
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

#define TOM_THUMB_3X5_CHARS " !+-./0123456789:?ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define TOM_THUMB_3X5_COUNT 44u

static const uint16_t tom_thumb_3x5[TOM_THUMB_3X5_COUNT] = {
    0x0000, 0x4904, 0x2E80, 0x7000, 0x4000, 0x12A4, 0x3B6E, 0x2C92, 0x62A7,
    0x628E, 0x5BC9, 0x798E, 0x39EF, 0x72A4, 0x7BEF, 0x7BCE, 0x4100, 0x7282,
    0x2BED, 0x6BAE, 0x3923, 0x6B6E, 0x79E7, 0x79E4, 0x39EB, 0x5BED, 0x7497,
    0x126A, 0x5BAD, 0x4927, 0x5FED, 0x5FFD, 0x2B6A, 0x6BA4, 0x2B7B, 0x6BF5,
    0x388E, 0x7492, 0x5B6B, 0x5B52, 0x5BFD, 0x5AAD, 0x5A92, 0x72A7,
};

#define PING_WINDOW 64u
#define PING_WINDOW_MASK (PING_WINDOW - 1u)
#define PING_MS_PER_SEC 1000.0
#define PING_MAX_MS 9999.0
#define PING_SMOOTH_NEW 0.2
typedef struct PingState {
  double send_time[PING_WINDOW];
  u16 send_seq[PING_WINDOW];
  u8 slot_valid[PING_WINDOW];
  double rtt_ms;
  u8 have;
} PingState;

typedef struct GameState {
  ECS ecs;
  IVec3 pos[ECS_MAX_ENTITIES];
  IVec3 vel[ECS_MAX_ENTITIES];
  Aim aim[ECS_MAX_ENTITIES];
  u8 kind[ECS_MAX_ENTITIES], lifetime[ECS_MAX_ENTITIES];
  u8 owner[ECS_MAX_ENTITIES];
  LagAim lag[ECS_MAX_ENTITIES];
  u8 flash[ECS_MAX_ENTITIES], held_by[ECS_MAX_ENTITIES];
  u8 weapon_id[ECS_MAX_ENTITIES];
  u8 weapon_ammo[ECS_MAX_ENTITIES];
  u8 weapon_cooldown[ECS_MAX_ENTITIES];
  u8 weapon_prev[ECS_MAX_ENTITIES];
  u16 weapon_reload[ECS_MAX_ENTITIES];
  u16 weapon_dirty_seq[ECS_MAX_ENTITIES];
  u16 last_snapshot_ack;
  PingState ping;
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
  Aim view;
  f32 gun_recoil, gun_recoil_velocity, gun_slide;
  f32 gun_rise, gun_rise_velocity;
  Vector2 gun_sway, look_delta;
  f32 aim_blend, screen_shake;
  f32 walk_phase, walk_blend;
  Vector2 move_sway, move_vel;
  f32 sprint_blend;
  f32 scale[ECS_MAX_ENTITIES];
  u8 r_reload[ECS_MAX_ENTITIES];
} GameState;

_Static_assert(sizeof(GameState) <= 4096, "game state exceeds 4 KiB");
_Static_assert(ECS_MAX_ENTITIES <= 32u,
               "current ecs can only handle 32 entities");
_Static_assert(MAX_PLAYERS <= 8, "visible_players is an 8-bit mask");

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

#define LAG_NEUTRAL 128
#define lag_encode(d) encode_centered8(d)
#define lag_decode(v) decode_centered8(v)

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

static u8 weapon_mag_size(u8 weapon_id) {
  return weapon_def(weapon_id)->mag_size;
}

static bool weapon_holding(const GameState *g, u32 e, bool want) {
  return want && weapon_entity_valid(e) && g->weapon_prev[e] != 0;
}

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

static void weapon_on_blocked(GameState *g, u32 e) {
  if (!weapon_entity_valid(e))
    return;
  g->weapon_cooldown[e] = weapon_def(g->weapon_id[e])->cooldown_tap_ticks;
}

static void weapon_set_prev(GameState *g, u32 e, bool want) {
  if (weapon_entity_valid(e))
    g->weapon_prev[e] = (u8)(want ? 1 : 0);
}

static i16 weapon_recoil_pitch(i16 pitch, u8 weapon_id, bool holding) {
  const WeaponDef *def = weapon_def(weapon_id);
  int kick = holding ? (int)def->recoil_pitch_hold_deg
                     : (int)def->recoil_pitch_tap_deg;
  return (i16)clamp_int((int)pitch + kick, -75, 75);
}

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

static f32 weapon_reload_progress(const GameState *g, u32 e) {
  if (!weapon_entity_valid(e) || g->weapon_reload[e] == 0)
    return 0.0f;
  const WeaponDef *def = weapon_def(g->weapon_id[e]);
  if (def->reload_ticks == 0)
    return 0.0f;
  return clamp_float(1.0f - (f32)g->weapon_reload[e] / (f32)def->reload_ticks,
                     0.0f, 1.0f);
}

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

static bool present_sprint_want(u8 buttons, i8 mx, i8 mz) {
  (void)mx;
  return (buttons & BTN_SPRINT) != 0 && mz > 0 && (buttons & BTN_AIM) == 0;
}

static bool present_aim_want(u8 buttons, bool reloading) {
  return (buttons & BTN_AIM) != 0 && !reloading;
}

typedef struct {
  Vector3 pos;
  f32 roll;
} PresentBob;

static PresentBob present_bob_offsets(f32 phase, f32 blend, f32 ads_damp) {
  f32 s = blend * ads_damp;
  return (PresentBob){{sinf(phase) * 0.014f * s,
                       -fabsf(cosf(phase)) * 0.012f * s,
                       sinf(phase * 2.0f) * 0.008f * s},
                      sinf(phase) * 2.5f * s};
}

static Vector2 present_lean_offsets(Vector2 sway) {
  return (Vector2){-sway.x * 0.045f, sway.y * 0.05f};
}

static u8 encode_reload_q(bool reloading, f32 t) {
  if (!reloading)
    return 0;
  return encode_range8(t, 1, 255);
}

static u16 decode_reload_q(u8 reload_q, u16 reload_ticks) {
  if (reload_q == 0 || reload_ticks == 0)
    return 0;
  return (u16)(((u32)reload_ticks * (u32)(255u - reload_q) + 127u) / 254u);
}

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
  bool is_local =
      g->local_player < MAX_PLAYERS && g->owner[e] == g->local_player;
  if (!is_local) {
    weapon_set_from_snapshot(g, e, srv_weapon, srv_ammo, srv_reloading);
    return;
  }
  u16 dirty = g->weapon_dirty_seq[e];
  bool stale = sequence_newer(dirty, snap_ack);
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
    stale = sequence_newer(dirty, snap_ack);
  }
  if (srv_reloading) {
    if (g->weapon_reload[e] != 0) {
      g->weapon_ammo[e] = 0;
      u16 local_remaining = g->weapon_reload[e];
      u16 srv_remaining = decode_reload_q(srv_reload_q, def->reload_ticks);
      if (srv_reload_q != 0 && srv_remaining < local_remaining)
        g->weapon_reload[e] = srv_remaining;
    } else if (g->weapon_ammo[e] == 0) {
      g->weapon_ammo[e] = 0;
      g->weapon_reload[e] = def->reload_ticks;
      g->weapon_dirty_seq[e] = snap_ack;
    } else if (!stale) {
      g->weapon_ammo[e] = 0;
      g->weapon_reload[e] = def->reload_ticks;
      g->weapon_dirty_seq[e] = snap_ack;
    }
  } else if (g->weapon_reload[e] != 0) {
    if (!stale) {
      g->weapon_ammo[e] = srv_ammo;
      g->weapon_reload[e] = 0;
      g->weapon_dirty_seq[e] = snap_ack;
    } else {
      g->weapon_ammo[e] = 0;
    }
  } else if (srv_ammo < g->weapon_ammo[e]) {
    g->weapon_ammo[e] = srv_ammo;
    g->weapon_dirty_seq[e] = snap_ack;
  }
}

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

static bool net_packet_ok(const u8 *buf, i32 n, u8 type, i32 size) {
  return n == size && n >= 2 &&
         buf[offsetof(NetSnapshot, magic)] == NET_MAGIC &&
         buf[offsetof(NetSnapshot, type)] == type;
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

static bool net_snapshot_wire_ok(const u8 *buf, i32 n) {
  if (!buf || n < (i32)NET_SNAPSHOT_HEADER_SIZE ||
      buf[offsetof(NetSnapshot, magic)] != NET_MAGIC ||
      buf[offsetof(NetSnapshot, type)] != NET_PACKET_SNAPSHOT)
    return false;
  u8 count = buf[offsetof(NetSnapshot, count)];
  if (count > NET_ENTITY_LIMIT)
    return false;
  i32 want = net_snapshot_wire_size(count);
  return want >= 0 && n == want && (size_t)n <= sizeof(NetSnapshot);
}

static i32 net_snapshot_encode(const NetSnapshot *snap, u8 *out, i32 out_cap) {
  if (!snap || !out || snap->count > NET_ENTITY_LIMIT)
    return -1;
  i32 want = net_snapshot_wire_size(snap->count);
  if (want < 0 || out_cap < want)
    return -1;
  memcpy(out, snap, NET_SNAPSHOT_HEADER_SIZE);
  if (snap->count > 0)
    memcpy(out + NET_SNAPSHOT_HEADER_SIZE, snap->entities,
           (size_t)snap->count * sizeof(NetEntity));
  memcpy(out + NET_SNAPSHOT_HEADER_SIZE +
             (size_t)snap->count * sizeof(NetEntity),
         snap->names, NET_SNAPSHOT_NAMES_SIZE);
  return want;
}

static bool net_snapshot_decode(const u8 *buf, i32 n, NetSnapshot *out) {
  if (!buf || !out || !net_snapshot_wire_ok(buf, n))
    return false;
  u8 count = buf[offsetof(NetSnapshot, count)];
  *out = (NetSnapshot){0};
  memcpy(out, buf, NET_SNAPSHOT_HEADER_SIZE);
  out->count = count;
  if (count > 0)
    memcpy(out->entities, buf + NET_SNAPSHOT_HEADER_SIZE,
           (size_t)count * sizeof(NetEntity));
  memcpy(out->names,
         buf + NET_SNAPSHOT_HEADER_SIZE + (size_t)count * sizeof(NetEntity),
         NET_SNAPSHOT_NAMES_SIZE);
  return true;
}

static bool net_input_decode(const u8 *buf, i32 n, NetInputPacket *out) {
  if (!buf || !out || n != (i32)sizeof(NetInputPacket))
    return false;
  if (buf[offsetof(NetInputPacket, magic)] != NET_MAGIC ||
      (buf[offsetof(NetInputPacket, type)] != NET_PACKET_INPUT &&
       buf[offsetof(NetInputPacket, type)] != NET_PACKET_DISCONNECT))
    return false;
  *out = (NetInputPacket){0};
  out->magic = buf[offsetof(NetInputPacket, magic)];
  out->type = buf[offsetof(NetInputPacket, type)];
  out->claimed_player = buf[offsetof(NetInputPacket, claimed_player)];
  out->buttons = buf[offsetof(NetInputPacket, buttons)];
  memcpy(&out->session, buf + offsetof(NetInputPacket, session),
         sizeof(out->session));
  memcpy(&out->sequence, buf + offsetof(NetInputPacket, sequence),
         sizeof(out->sequence));
  out->move_x = (i8)buf[offsetof(NetInputPacket, move_x)];
  out->move_z = (i8)buf[offsetof(NetInputPacket, move_z)];
  memcpy(&out->aim_yaw, buf + offsetof(NetInputPacket, aim_yaw),
         sizeof(out->aim_yaw));
  memcpy(&out->aim_pitch, buf + offsetof(NetInputPacket, aim_pitch),
         sizeof(out->aim_pitch));
  memcpy(out->name, buf + offsetof(NetInputPacket, name), PLAYER_NAME_LENGTH);
  return true;
}

static bool net_chat_decode(const u8 *buf, i32 n, NetChatPacket *out) {
  if (!buf || !out)
    return false;
  if (!net_packet_ok(buf, n, NET_PACKET_CHAT, (i32)sizeof(NetChatPacket)))
    return false;
  *out = (NetChatPacket){0};
  out->magic = buf[offsetof(NetChatPacket, magic)];
  out->type = buf[offsetof(NetChatPacket, type)];
  out->player = buf[offsetof(NetChatPacket, player)];
  out->length = buf[offsetof(NetChatPacket, length)];
  if (out->length > CHAT_LENGTH)
    return false;
  memcpy(&out->session, buf + offsetof(NetChatPacket, session),
         sizeof(out->session));
  memcpy(&out->sequence, buf + offsetof(NetChatPacket, sequence),
         sizeof(out->sequence));
  memcpy(out->name, buf + offsetof(NetChatPacket, name), PLAYER_NAME_LENGTH);
  memcpy(out->text, buf + offsetof(NetChatPacket, text), out->length);
  return true;
}

static void net_sanitize_chat_text(String text) {
  for (i32 i = 0; i < text.length; ++i) {
    u8 c = text.items[i];
    if (!CHAR_IS_CHAT(c))
      text.items[i] = ' ';
  }
}

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

static void game_zero(GameState *g) {
  *g = (GameState){.local_player = NET_PACKET_NONE};
  memset(g->network_entity, NET_PACKET_NONE, sizeof(g->network_entity));
  memset(g->player_entity, NET_PACKET_NONE, sizeof(g->player_entity));
}

static void clear_entity_data(GameState *g, u32 e) {
  g->pos[e] = (IVec3){0, 0, 0};
  g->vel[e] = (IVec3){0, 0, 0};
  g->aim[e] = (Aim){0, 0};
  g->kind[e] = g->lifetime[e] = g->owner[e] = 0;
  g->flash[e] = g->held_by[e] = 0;
  g->weapon_ammo[e] = g->weapon_cooldown[e] = g->weapon_prev[e] = 0;
  g->weapon_reload[e] = g->weapon_dirty_seq[e] = 0;
  g->r_reload[e] = 0;
  g->lag[e] = (LagAim){LAG_NEUTRAL, LAG_NEUTRAL};
  g->scale[e] = 1.0f;
  g->weapon_id[e] = WEAPON_P99;
}

static void add_kind_components(GameState *g, u32 e, u8 kind) {
  ecs_add(&g->ecs, e, COMP_POSITION);
  ecs_add(&g->ecs, e, COMP_RENDERABLE);
  ecs_add(&g->ecs, e, COMP_NETWORKED);
  if (kind == KIND_CUBE) {
    ecs_add(&g->ecs, e, COMP_SCALE);
    ecs_add(&g->ecs, e, COMP_SOLID);
  } else if (kind == KIND_PLAYER) {
    ecs_add(&g->ecs, e, COMP_VELOCITY);
    ecs_add(&g->ecs, e, COMP_AIM);
    ecs_add(&g->ecs, e, COMP_PLAYER);
    ecs_add(&g->ecs, e, COMP_SOLID);
  } else if (kind == KIND_BULLET) {
    ecs_add(&g->ecs, e, COMP_VELOCITY);
    ecs_add(&g->ecs, e, COMP_PROJECTILE);
    ecs_add(&g->ecs, e, COMP_LIFETIME);
  } else if (kind == KIND_CASING) {
    ecs_add(&g->ecs, e, COMP_VELOCITY);
    ecs_add(&g->ecs, e, COMP_LIFETIME);
  } else if (kind == KIND_IMPACT_MARK) {
    ecs_add(&g->ecs, e, COMP_LIFETIME);
  } else if (kind == KIND_FLOOR) {
    ecs_add(&g->ecs, e, COMP_SOLID);
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
  g->pos[e].x = (i16)(((int)((index * 7u + 3u) % 17u) - 8) * POS_SCALE);
  g->pos[e].y = POS_SCALE / 2;
  g->pos[e].z = (i16)(((int)((index * 11u + 5u) % 17u) - 8) * POS_SCALE);
  g->kind[e] = KIND_CUBE;
  add_kind_components(g, e, KIND_CUBE);
  return e;
}

static u32 create_floor(GameState *g) {
  u32 e = ecs_create(&g->ecs);
  if (e == ECS_INVALID_ENTITY)
    return e;
  clear_entity_data(g, e);
  g->kind[e] = KIND_FLOOR;
  add_kind_components(g, e, KIND_FLOOR);
  return e;
}

static void game_init(GameState *g) {
  game_zero(g);
  for (u32 i = 0; i < MAX_CUBES; ++i)
    (void)create_cube(g, i);
  (void)create_floor(g);
}

static void client_clear_world(GameState *g) {
  u32 comps[] = {COMP_POSITION};
  EcsQuery q = ecs_query(&g->ecs, comps, 1);
  while (ecs_next(&q))
    ecs_destroy(&g->ecs, q.entity);
  for (u32 e = 0; e < ECS_MAX_ENTITIES; ++e)
    g->scale[e] = 1.0f;
  memset(g->network_entity, NET_PACKET_NONE, sizeof(g->network_entity));
  g->snapshot_seen = 0;
}

static void client_init(GameState *g) {
  game_zero(g);
  client_clear_world(g);
}

static Vector3 vec3(f32 x, f32 y, f32 z) { return (Vector3){x, y, z}; }

static Vector3 vec3_add(Vector3 a, Vector3 b) {
  return vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static Vector3 vec3_sub(Vector3 a, Vector3 b) {
  return vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static Vector3 vec3_mul(Vector3 a, f32 s) {
  return vec3(a.x * s, a.y * s, a.z * s);
}

static f32 vec3_dot(Vector3 a, Vector3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static f32 vec3_len(Vector3 a) { return sqrtf(vec3_dot(a, a)); }

static Vector3 vec3_cross(Vector3 a, Vector3 b) {
  return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x);
}

static Vector3 vec3_norm(Vector3 a) {
  f32 l = vec3_len(a);
  return l < 1e-6f ? vec3(1.0f, 0.0f, 0.0f) : vec3_mul(a, 1.0f / l);
}

static f32 scale_value(const GameState *g, u32 e) {
  if (e >= ECS_MAX_ENTITIES || !ecs_has(&g->ecs, e, COMP_SCALE))
    return 1.0f;
  f32 s = g->scale[e];
  if (!(s > 0.01f))
    return 1.0f;
  return clamp_float(s, CUBE_SCALE_MIN, CUBE_SCALE_MAX);
}

static Vector3 interp_center_fp(const GameState *g, u32 e, f32 a) {
  f32 k = 1.0f - a;
  return vec3((f32)g->pos[e].x - (f32)g->vel[e].x * k,
              (f32)g->pos[e].y - (f32)g->vel[e].y * k,
              (f32)g->pos[e].z - (f32)g->vel[e].z * k);
}

static Vector3 entity_pos_m(const GameState *g, u32 e, f32 a) {
  return vec3_mul(interp_center_fp(g, e, a), 1.0f / (f32)POS_SCALE);
}

static bool cube_overlap(const GameState *g, i16 x, i16 z, i16 radius) {
  u32 comps[] = {COMP_POSITION, COMP_SOLID};
  EcsQuery q = ecs_query(&g->ecs, comps, 2);
  while (ecs_next(&q)) {
    u32 e = q.entity;
    if (g->kind[e] != KIND_CUBE || g->held_by[e] != 0)
      continue;
    f32 s = scale_value(g, e);
    int hx = (int)lroundf(0.5f * CUBE_VIS_X * s * (f32)POS_SCALE);
    int hz = (int)lroundf(0.5f * CUBE_VIS_Z * s * (f32)POS_SCALE);
    if (abs((int)x - g->pos[e].x) < hx + radius &&
        abs((int)z - g->pos[e].z) < hz + radius)
      return true;
  }
  return false;
}

static Vector3 sight_dir(f32 yaw_deg, f32 pitch_deg) {
  f32 h = cosf(pitch_deg * DEG2RAD);
  return vec3(sinf(yaw_deg * DEG2RAD) * h, sinf(pitch_deg * DEG2RAD),
              cosf(yaw_deg * DEG2RAD) * h);
}

static void aim_dir(i16 yaw_deg, i16 pitch_deg, Vector3 *out) {
  *out = sight_dir((f32)yaw_deg, (f32)pitch_deg);
}

static void look_basis(f32 yaw_deg, f32 pitch_deg, Vector3 *fwd, Vector3 *right,
                       Vector3 *up) {
  *fwd = sight_dir(yaw_deg, pitch_deg);
  f32 cy = cosf(yaw_deg * DEG2RAD), sy = sinf(yaw_deg * DEG2RAD);
  *right = vec3(-cy, 0.0f, sy);
  *up = vec3_cross(*right, *fwd);
}

static Vector3 player_eye_m(Vector3 feet) {
  return vec3(feet.x, feet.y + EYE_HEIGHT_M, feet.z);
}

static Vector3 hip_origin_m(Vector3 feet, f32 yaw_deg) {
  f32 cy = cosf(yaw_deg * DEG2RAD), sy = sinf(yaw_deg * DEG2RAD);
  return vec3(feet.x - GUN_HIP_SIDE_M * cy + GUN_HIP_FWD_M * sy,
              feet.y + GUN_HIP_UP_M,
              feet.z + GUN_HIP_SIDE_M * sy + GUN_HIP_FWD_M * cy);
}

typedef struct {
  Vector3 origin, fwd, right, up;
  f32 yaw, pitch;
} GunPose;

static GunPose gun_pose_at(Vector3 feet, f32 look_yaw, f32 look_pitch,
                           f32 zero_m) {
  GunPose p;
  p.origin = hip_origin_m(feet, look_yaw);
  Vector3 target =
      vec3_add(player_eye_m(feet),
               vec3_mul(sight_dir(look_yaw, look_pitch), fmaxf(zero_m, 0.35f)));
  Vector3 d = vec3_sub(target, p.origin);
  f32 len = vec3_len(d);
  if (len < 1e-5f) {
    p.yaw = look_yaw;
    p.pitch = look_pitch;
  } else {
    d = vec3_mul(d, 1.0f / len);
    p.yaw = atan2f(d.x, d.z) * RAD2DEG;
    p.pitch = asinf(clamp_float(d.y, -1.0f, 1.0f)) * RAD2DEG;
  }
  look_basis(p.yaw, p.pitch, &p.fwd, &p.right, &p.up);
  return p;
}

static Vector3 gun_port_m(const GunPose *p) {
  f32 x = GUN_CENTER_X_M - 0.385f, y = 0.045f, z = 0.18f;
  f32 pt = -p->pitch * DEG2RAD;
  f32 cp = cosf(pt), sp = sinf(pt);
  f32 y1 = y * cp - z * sp, z1 = y * sp + z * cp;
  f32 cy = cosf(p->yaw * DEG2RAD), sy = sinf(p->yaw * DEG2RAD);
  return vec3(p->origin.x + x * cy + z1 * sy, p->origin.y + y1,
              p->origin.z - x * sy + z1 * cy);
}

static f32 present_lerp_angle(i16 base, int delta, f32 a) {
  return (f32)base - (f32)delta * (1.0f - a);
}

static const u8 SOLID_MAT[] = {
    MARK_MAT_PURPLE, MARK_MAT_MID,    MARK_MAT_MID,    MARK_MAT_PURPLE,
    MARK_MAT_PURPLE, MARK_MAT_PURPLE, MARK_MAT_PURPLE,
};

static void solid_bounds_fp(const GameState *g, u32 e, f32 a, Vector3 *bmin,
                            Vector3 *bmax) {
  Vector3 c = interp_center_fp(g, e, a);
  if (g->kind[e] == KIND_FLOOR) {
    float r = (float)(ARENA * POS_SCALE);
    *bmin = vec3(-r, -r, -r);
    *bmax = vec3(r, 0.0f, r);
    return;
  }
  if (g->kind[e] == KIND_PLAYER) {
    float r = PLAYER_HIT_RADIUS_M * (float)POS_SCALE;
    float h = PLAYER_HIT_HEIGHT_M * (float)POS_SCALE;
    *bmin = vec3(c.x - r, c.y, c.z - r);
    *bmax = vec3(c.x + r, c.y + h, c.z + r);
    return;
  }
  f32 s = scale_value(g, e);
  f32 hx = 0.5f * CUBE_VIS_X * s * (f32)POS_SCALE;
  f32 hy = 0.5f * CUBE_VIS_Y * s * (f32)POS_SCALE;
  f32 hz = 0.5f * CUBE_VIS_Z * s * (f32)POS_SCALE;
  *bmin = vec3(c.x - hx, c.y - hy, c.z - hz);
  *bmax = vec3(c.x + hx, c.y + hy, c.z + hz);
}

static bool seg_box_hit(Vector3 p0, Vector3 d, Vector3 bmin, Vector3 bmax,
                        float *t_out, Vector3 *n_out) {
  float tmin = 0.0f, tmax = 1.0f;
  Vector3 n = {0.0f, 0.0f, 0.0f};
  if (fabsf(d.x) < 1e-9f) {
    if (p0.x < bmin.x || p0.x > bmax.x)
      return false;
  } else {
    float inv = 1.0f / d.x;
    float t1 = (bmin.x - p0.x) * inv;
    float t2 = (bmax.x - p0.x) * inv;
    Vector3 f1 = {-1.0f, 0.0f, 0.0f}, f2 = {1.0f, 0.0f, 0.0f};
    if (t1 > t2) {
      float tt = t1;
      t1 = t2;
      t2 = tt;
      Vector3 tf = f1;
      f1 = f2;
      f2 = tf;
    }
    if (t1 > tmin) {
      tmin = t1;
      n = f1;
    }
    if (t2 < tmax)
      tmax = t2;
    if (tmin > tmax)
      return false;
  }
  if (fabsf(d.y) < 1e-9f) {
    if (p0.y < bmin.y || p0.y > bmax.y)
      return false;
  } else {
    float inv = 1.0f / d.y;
    float t1 = (bmin.y - p0.y) * inv;
    float t2 = (bmax.y - p0.y) * inv;
    Vector3 f1 = {0.0f, -1.0f, 0.0f}, f2 = {0.0f, 1.0f, 0.0f};
    if (t1 > t2) {
      float tt = t1;
      t1 = t2;
      t2 = tt;
      Vector3 tf = f1;
      f1 = f2;
      f2 = tf;
    }
    if (t1 > tmin) {
      tmin = t1;
      n = f1;
    }
    if (t2 < tmax)
      tmax = t2;
    if (tmin > tmax)
      return false;
  }
  if (fabsf(d.z) < 1e-9f) {
    if (p0.z < bmin.z || p0.z > bmax.z)
      return false;
  } else {
    float inv = 1.0f / d.z;
    float t1 = (bmin.z - p0.z) * inv;
    float t2 = (bmax.z - p0.z) * inv;
    Vector3 f1 = {0.0f, 0.0f, -1.0f}, f2 = {0.0f, 0.0f, 1.0f};
    if (t1 > t2) {
      float tt = t1;
      t1 = t2;
      t2 = tt;
      Vector3 tf = f1;
      f1 = f2;
      f2 = tf;
    }
    if (t1 > tmin) {
      tmin = t1;
      n = f1;
    }
    if (t2 < tmax)
      tmax = t2;
    if (tmin > tmax)
      return false;
  }
  if (n.x == 0.0f && n.y == 0.0f && n.z == 0.0f)
    n = mark_normal_vec(mark_code_for_vec(vec3(-d.x, -d.y, -d.z)));
  *t_out = tmin;
  *n_out = n;
  return true;
}

typedef struct {
  bool hit;
  f32 t;
  Vector3 n;
  u32 entity;
} TraceHit;

static bool player_seg_hit(const GameState *g, u32 e, f32 a, Vector3 p0,
                           Vector3 d, float *t_out, Vector3 *n_out) {
  Vector3 feet = interp_center_fp(g, e, a);
  f32 yaw = present_lerp_angle(g->aim[e].yaw, lag_decode(g->lag[e].yaw), a);
  f32 cy = cosf(yaw * DEG2RAD), sy = sinf(yaw * DEG2RAD);
  Vector3 w0 = vec3_sub(p0, feet);
  Vector3 lp0 = vec3(w0.x * cy - w0.z * sy, w0.y, w0.x * sy + w0.z * cy);
  Vector3 ld = vec3(d.x * cy - d.z * sy, d.y, d.x * sy + d.z * cy);
  f32 hx = 0.5f * PLAYER_BODY_X * (f32)POS_SCALE;
  f32 zn = 0.5f * PLAYER_BODY_Z * (f32)POS_SCALE;
  f32 zp = (0.06f + 0.5f * PLAYER_HEAD_S) * (f32)POS_SCALE;
  Vector3 bmin = vec3(-hx, 0.0f, -zn);
  Vector3 bmax = vec3(hx, PLAYER_HIT_HEIGHT_M * (f32)POS_SCALE, zp);
  Vector3 ln;
  float t;
  if (!seg_box_hit(lp0, ld, bmin, bmax, &t, &ln))
    return false;
  *n_out = vec3(ln.x * cy + ln.z * sy, ln.y, -ln.x * sy + ln.z * cy);
  *t_out = t;
  return true;
}

static TraceHit trace_solids(const GameState *g, Vector3 p0, Vector3 d,
                             u32 skip_e, u8 skip_owner, f32 a) {
  TraceHit r = {false, 1.0f, {0.0f, 1.0f, 0.0f}, ECS_INVALID_ENTITY};
  u32 comps[] = {COMP_SOLID};
  EcsQuery q = ecs_query(&g->ecs, comps, 1);
  while (ecs_next(&q)) {
    u32 s = q.entity;
    if (s == skip_e)
      continue;
    if (g->kind[s] == KIND_CUBE && g->held_by[s] != 0)
      continue;
    if (g->kind[s] == KIND_PLAYER && skip_owner < MAX_PLAYERS &&
        g->owner[s] == skip_owner)
      continue;
    Vector3 n;
    f32 t;
    bool hit;
    if (g->kind[s] == KIND_PLAYER) {
      hit = player_seg_hit(g, s, a, p0, d, &t, &n);
    } else {
      Vector3 bmin, bmax;
      solid_bounds_fp(g, s, a, &bmin, &bmax);
      hit = seg_box_hit(p0, d, bmin, bmax, &t, &n);
    }
    if (hit && t < r.t) {
      r.hit = true;
      r.t = t;
      r.n = n;
      r.entity = s;
    }
  }
  return r;
}

static f32 sight_zero_m(const GameState *g, u32 skip, Vector3 eye, Vector3 f,
                        f32 a) {
  Vector3 p0 = vec3_mul(eye, (f32)POS_SCALE);
  Vector3 d = vec3_mul(f, SIGHT_MAX_DIST_M * (f32)POS_SCALE);
  TraceHit h = trace_solids(g, p0, d, skip, NET_PACKET_NONE, a);
  if (!h.hit)
    return SIGHT_SKY_DIST_M;
  return clamp_float(h.t * SIGHT_MAX_DIST_M, 0.35f, SIGHT_MAX_DIST_M);
}

static GunPose player_gun_pose(const GameState *g, u32 e, Vector3 feet, f32 yaw,
                               f32 pitch, f32 a) {
  Vector3 eye = player_eye_m(feet);
  Vector3 look = sight_dir(yaw, pitch);
  return gun_pose_at(feet, yaw, pitch, sight_zero_m(g, e, eye, look, a));
}

#endif
