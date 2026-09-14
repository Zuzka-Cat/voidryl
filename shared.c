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

#define FIRE_COOLDOWN_TICKS 16
#define PLAYER_FIRE_FLASH_TICKS 6
#define PLAYER_HIT_FLASH_TICKS 24
#define BULLET_LIFETIME_TICKS 128
#define CASING_LIFETIME_TICKS 128
#define IMPACT_MARK_LIFETIME_TICKS 128

#define ARENA 32
#define BULLET_SPEED 32.0f
#define BULLET_ZERO_DISTANCE 12.0f
#define ARENA_POS (15 * POS_SCALE)

static const Vector3 GUN_MUZZLE_OFFSET = {1.46f, 0.42f, -0.18f};

enum EntityKind {
  KIND_CUBE = 1,
  KIND_PLAYER,
  KIND_BULLET,
  KIND_IMPACT_MARK,
  KIND_CASING
};

typedef enum {
  COMP_POSITION = 0,
  COMP_VELOCITY,
  COMP_AIM,
  COMP_PLAYER,
  COMP_PROJECTILE,
  COMP_LIFETIME,
  COMP_RENDERABLE,
  COMP_NETWORKED
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
} NetInputPacket;

typedef struct NetEntity {
  u8 entity, kind, owner, flags, extra, _pad;
  i16 x, y, z, yaw, pitch;
} NetEntity;

typedef struct NetChatPacket {
  u8 magic, type, player, length;
  u32 session;
  u16 sequence;
  u8 name[PLAYER_NAME_LENGTH];
  u8 text[CHAT_LENGTH];
} NetChatPacket;

typedef struct NetSnapshot {
  u8 magic, type, local_player, count;
  u32 session;
  u16 tick, acknowledged_sequence;
  NetEntity entities[NET_ENTITY_LIMIT];
  u8 names[MAX_PLAYERS][PLAYER_NAME_LENGTH];
} NetSnapshot;

#define ECS_MAX_ENTITIES 32u
#define ECS_MAX_COMPONENTS 8u
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
  u8 owner[ECS_MAX_ENTITIES], cooldown[ECS_MAX_ENTITIES];
  u8 flash[ECS_MAX_ENTITIES], held_by[ECS_MAX_ENTITIES];
  InputCommand input[MAX_PLAYERS];
  Peer peers[MAX_PLAYERS];
  u8 player_entity[MAX_PLAYERS];
  u8 network_entity[ECS_MAX_ENTITIES];
  u32 snapshot_seen, session;
  u16 tick, next_sequence, next_chat_sequence;
  u16 last_snapshot_tick, last_chat_sequence, notice_ticks;
  u16 snapshot_silence_ticks;
  u8 local_player, connected, mouse_captured, help, chatting, chat_length;
  u8 interpolation_ticks, command_ticks, gun_kick, fire_cooldown;
  u8 visible_players, got_snapshot, got_chat_sequence, notice_length;
  u8 requested_name[PLAYER_NAME_LENGTH];
  u8 player_name[MAX_PLAYERS][PLAYER_NAME_LENGTH];
  u8 chat_input[CHAT_LENGTH];
  u8 notice[NOTICE_LENGTH];
  i16 view_yaw, view_pitch;
  f32 gun_recoil, gun_recoil_velocity, gun_slide;
  f32 gun_sway_x, gun_sway_y, look_delta_x, look_delta_y;
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

// packet validation

static bool net_magic_ok(const u8 *buf, i32 n) {
  return n >= 2 && buf[0] == NET_MAGIC;
}

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
  if (snap->session != expected_session)
    return false;
  if (!net_seq_fresh(snap->tick, got, last_tick))
    return false;
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
  g->cooldown[e] = g->flash[e] = g->held_by[e] = 0;
}

static void add_kind_components(GameState *g, u32 e, u8 kind) {
  ecs_add(&g->ecs, e, COMP_POSITION);
  ecs_add(&g->ecs, e, COMP_RENDERABLE);
  ecs_add(&g->ecs, e, COMP_NETWORKED);
  if (kind == KIND_PLAYER) {
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
  game_zero(g);
  for (u32 i = 0; i < MAX_CUBES; ++i)
    (void)create_cube(g, i);
}

// spatial / gun helpers

static bool cube_overlap(const GameState *g, i16 x, i16 z, i16 radius) {
  u32 comps[] = {COMP_POSITION, COMP_RENDERABLE};
  EcsQuery q = ecs_query(&g->ecs, comps, 2);
  while (ecs_next(&q)) {
    u32 e = q.entity;
    if (g->kind[e] == KIND_CUBE && g->held_by[e] == 0 &&
        abs((int)x - g->px[e]) < POS_SCALE / 2 + radius &&
        abs((int)z - g->pz[e]) < POS_SCALE / 2 + radius)
      return true;
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
