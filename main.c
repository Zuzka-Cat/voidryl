#define _POSIX_C_SOURCE 200809L

#include "ecs.h"
#include "raylib.h"
#include "rlgl.h"
#include "tom_thumb_3x5.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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
#define GUN_MUZZLE_FORWARD 1.46f
#define GUN_MUZZLE_RIGHT 0.42f
#define GUN_MUZZLE_UP -0.18f
#define ARENA_POS (15 * POS_SCALE)

#define u16 uint16_t
#define i16 int16_t
#define i32 int32_t
#define u32 uint32_t
#define i8 int8_t
#define u8 uint8_t
#define f32 float

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
  u8 visible_players, got_snapshot, got_chat_sequence;
  u8 requested_name[PLAYER_NAME_LENGTH + 1];
  u8 player_name[MAX_PLAYERS][PLAYER_NAME_LENGTH + 1];
  u8 chat_input[CHAT_LENGTH + 1];
  u8 notice[NOTICE_LENGTH + 1];
  i16 view_yaw, view_pitch;
  f32 gun_recoil, gun_recoil_velocity, gun_slide;
  f32 gun_sway_x, gun_sway_y, look_delta_x, look_delta_y;
} GameState;

_Static_assert(sizeof(GameState) <= 4096, "game state exceeds 4 KiB");
_Static_assert(ECS_MAX_ENTITIES <= 32u, "network bitsets assume 32 entities");
_Static_assert(MAX_PLAYERS <= 8, "visible_players is an 8-bit mask");

static const Color PAL_DARK = {16, 8, 24, 255};
static const Color PAL_PURPLE = {60, 30, 102, 255};
static const Color PAL_MID = {155, 123, 208, 255};
static const Color PAL_LIGHT = {246, 213, 232, 255};

static volatile sig_atomic_t server_running = 1;

static void on_signal(int sig) {
  (void)sig;
  server_running = 0;
}

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

static u16 sequence_host(u16 s) { return ntohs(s); }
static u16 sequence_wire(u16 s) { return htons(s); }
static u32 session_host(u32 s) { return ntohl(s); }
static u32 session_wire(u32 s) { return htonl(s); }
static i16 wire_i16(i16 v) { return (i16)ntohs((u16)v); }
static i16 host_i16(i16 v) { return (i16)htons((u16)v); }

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

static bool valid_port(const u8 *text, u16 *port) {
  unsigned long value = 0;
  size_t i = 0;
  if (!text || !text[0])
    return false;
  while (text[i] >= '0' && text[i] <= '9') {
    value = value * 10u + (unsigned long)(text[i] - '0');
    if (value > 65535u)
      return false;
    ++i;
  }
  if (text[i] != '\0' || value == 0)
    return false;
  *port = (u16)value;
  return true;
}

static bool valid_player_name(const u8 *text, u8 out[PLAYER_NAME_LENGTH + 1]) {
  if (!text || strlen(text) != PLAYER_NAME_LENGTH)
    return false;
  for (int i = 0; i < PLAYER_NAME_LENGTH; ++i) {
    u8 c = text[i];
    if (c >= 'a' && c <= 'z')
      c = (u8)(c - ('a' - 'A'));
    if (c < 'A' || c > 'Z')
      return false;
    out[i] = c;
  }
  out[PLAYER_NAME_LENGTH] = '\0';
  return true;
}

static bool valid_wire_name(const u8 name[PLAYER_NAME_LENGTH]) {
  for (int i = 0; i < PLAYER_NAME_LENGTH; ++i)
    if (name[i] < 'A' || name[i] > 'Z')
      return false;
  return true;
}

static bool parse_ipv4(const u8 *text, struct in_addr *address) {
  if (strcmp(text, "localhost") == 0)
    text = "127.0.0.1";
  return inet_pton(AF_INET, text, address) == 1;
}

static void socket_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int open_udp_server(u16 port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in address;
  int reuse = 1;
  if (fd < 0)
    return -1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
    close(fd);
    return -1;
  }
  socket_nonblocking(fd);
  return fd;
}

static int open_udp_client(const u8 *host, u16 port,
                           struct sockaddr_in *server) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct in_addr address;
  if (fd < 0 || !parse_ipv4(host, &address)) {
    if (fd >= 0)
      close(fd);
    return -1;
  }
  memset(server, 0, sizeof(*server));
  server->sin_family = AF_INET;
  server->sin_addr = address;
  server->sin_port = htons(port);
  if (connect(fd, (const struct sockaddr *)server, sizeof(*server)) < 0) {
    close(fd);
    return -1;
  }
  socket_nonblocking(fd);
  return fd;
}

static void game_zero(GameState *g) {
  memset(g, 0, sizeof(*g));
  g->local_player = NET_PACKET_NONE;
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

static u32 create_player(GameState *g, u8 player) {
  u32 e = ecs_create(&g->ecs);
  if (e == ECS_INVALID_ENTITY)
    return e;
  clear_entity_data(g, e);
  g->px[e] = (i16)(((int)player - (int)MAX_PLAYERS / 2) * 2 * POS_SCALE);
  g->pz[e] = 5 * POS_SCALE;
  g->yaw[e] = 180;
  g->kind[e] = KIND_PLAYER;
  g->owner[e] = player;
  g->player_entity[player] = (u8)e;
  add_kind_components(g, e, KIND_PLAYER);
  return e;
}

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
                         Vector3 u, f32 along, f32 side, f32 lift) {
  return (Vector3){
      (f32)g->px[e] / POS_SCALE + f.x * along + r.x * side + u.x * lift,
      (f32)g->py[e] / POS_SCALE + 1.5f + f.y * along + r.y * side + u.y * lift,
      (f32)g->pz[e] / POS_SCALE + f.z * along + r.z * side + u.z * lift};
}

static void movement_system(GameState *g) {
  u32 comps[] = {COMP_POSITION, COMP_VELOCITY, COMP_PLAYER};
  EcsQuery q = ecs_query(&g->ecs, comps, 3);
  while (ecs_next(&q)) {
    u32 e = q.entity;
    InputCommand *in = &g->input[g->owner[e]];
    f32 yaw = (f32)g->yaw[e] * DEG2RAD;
    f32 dx = (f32)in->move_z * sinf(yaw) - (f32)in->move_x * cosf(yaw);
    f32 dz = (f32)in->move_z * cosf(yaw) + (f32)in->move_x * sinf(yaw);
    f32 len = sqrtf(dx * dx + dz * dz);
    if (len > 1.0f) {
      dx /= len;
      dz /= len;
    }
    i16 step_x = (i16)lroundf(dx * 3.0f);
    i16 step_z = (i16)lroundf(dz * 3.0f);
    g->vx[e] = step_x;
    g->vz[e] = step_z;
    if ((in->buttons & BTN_JUMP) != 0 && g->py[e] == 0)
      g->vy[e] = 6;
    if (g->py[e] > 0 || g->vy[e] > 0) {
      g->py[e] = (i16)(g->py[e] + g->vy[e]);
      if (g->tick % 6u == 0 && g->vy[e] > -6)
        --g->vy[e];
      if (g->py[e] <= 0) {
        g->py[e] = 0;
        g->vy[e] = 0;
      }
    }
    i16 nx = clamp_i16((int)g->px[e] + step_x, -ARENA_POS, ARENA_POS);
    i16 nz = clamp_i16((int)g->pz[e] + step_z, -ARENA_POS, ARENA_POS);
    if (!cube_overlap(g, nx, g->pz[e], POS_SCALE / 4))
      g->px[e] = nx;
    if (!cube_overlap(g, g->px[e], nz, POS_SCALE / 4))
      g->pz[e] = nz;
  }
}

static void aim_system(GameState *g) {
  u32 comps[] = {COMP_AIM, COMP_PLAYER};
  EcsQuery q = ecs_query(&g->ecs, comps, 2);
  while (ecs_next(&q)) {
    u8 p = g->owner[q.entity];
    g->yaw[q.entity] = wrap_degrees(g->input[p].aim_yaw);
    g->pitch[q.entity] = clamp_i16(g->input[p].aim_pitch, -75, 75);
  }
}

static u32 held_cube_for(const GameState *g, u8 player) {
  u32 comps[] = {COMP_POSITION, COMP_RENDERABLE, COMP_NETWORKED};
  EcsQuery q = ecs_query(&g->ecs, comps, 3);
  while (ecs_next(&q))
    if (g->kind[q.entity] == KIND_CUBE && g->held_by[q.entity] == player + 1u)
      return q.entity;
  return ECS_INVALID_ENTITY;
}

static void holding_system(GameState *g) {
  u32 players[] = {COMP_POSITION, COMP_AIM, COMP_PLAYER};
  EcsQuery pq = ecs_query(&g->ecs, players, 3);
  while (ecs_next(&pq)) {
    u32 pe = pq.entity;
    u8 player = g->owner[pe];
    u32 held = held_cube_for(g, player);
    bool want = (g->input[player].buttons & BTN_GRAB) != 0;
    if (!want && held != ECS_INVALID_ENTITY) {
      g->held_by[held] = 0;
      held = ECS_INVALID_ENTITY;
    }
    if (want && held == ECS_INVALID_ENTITY) {
      Vector3 dir;
      u32 cubes[] = {COMP_POSITION, COMP_RENDERABLE, COMP_NETWORKED};
      EcsQuery cq = ecs_query(&g->ecs, cubes, 3);
      f32 best_d = 1000.0f;
      u32 best = ECS_INVALID_ENTITY;
      aim_dir(g->yaw[pe], g->pitch[pe], &dir);
      while (ecs_next(&cq)) {
        u32 c = cq.entity;
        if (g->kind[c] != KIND_CUBE || g->held_by[c] != 0)
          continue;
        Vector3 off = {(f32)(g->px[c] - g->px[pe]) / POS_SCALE,
                       (f32)(g->py[c] - 96) / POS_SCALE,
                       (f32)(g->pz[c] - g->pz[pe]) / POS_SCALE};
        f32 d = sqrtf(off.x * off.x + off.y * off.y + off.z * off.z);
        f32 dot = d > 0.01f
                      ? (off.x * dir.x + off.y * dir.y + off.z * dir.z) / d
                      : -1.0f;
        if (d < 5.0f && dot > 0.55f && d < best_d) {
          best_d = d;
          best = c;
        }
      }
      if (best != ECS_INVALID_ENTITY) {
        g->held_by[best] = player + 1u;
        held = best;
      }
    }
    if (held != ECS_INVALID_ENTITY) {
      Vector3 dir;
      aim_dir(g->yaw[pe], g->pitch[pe], &dir);
      g->px[held] =
          clamp_i16((int)g->px[pe] + (int)lroundf(dir.x * 2.0f * POS_SCALE),
                    -ARENA_POS, ARENA_POS);
      g->py[held] = POS_SCALE + 32;
      g->pz[held] =
          clamp_i16((int)g->pz[pe] + (int)lroundf(dir.z * 2.0f * POS_SCALE),
                    -ARENA_POS, ARENA_POS);
    }
  }
}

static u32 spawn_bullet(GameState *g, u32 pe) {
  Vector3 f, r, u, muzzle, target, dir;
  if (count_kind(g, KIND_BULLET) >= MAX_BULLETS)
    return ECS_INVALID_ENTITY;
  u32 e = take_entity(g);
  if (e == ECS_INVALID_ENTITY)
    return e;
  clear_entity_data(g, e);
  gun_basis(g, pe, &f, &r, &u);
  muzzle = gun_point(g, pe, f, r, u, GUN_MUZZLE_FORWARD, GUN_MUZZLE_RIGHT,
                     GUN_MUZZLE_UP);
  target = gun_point(g, pe, f, r, u, BULLET_ZERO_DISTANCE, 0.0f, 0.0f);
  dir =
      (Vector3){target.x - muzzle.x, target.y - muzzle.y, target.z - muzzle.z};
  f32 len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
  if (len > 0.001f) {
    dir.x /= len;
    dir.y /= len;
    dir.z /= len;
  } else {
    dir = f;
  }
  g->px[e] = (i16)lroundf(muzzle.x * POS_SCALE);
  g->py[e] = (i16)lroundf(muzzle.y * POS_SCALE);
  g->pz[e] = (i16)lroundf(muzzle.z * POS_SCALE);
  g->vx[e] = (i16)lroundf(dir.x * BULLET_SPEED);
  g->vy[e] = (i16)lroundf(dir.y * BULLET_SPEED);
  g->vz[e] = (i16)lroundf(dir.z * BULLET_SPEED);
  g->yaw[e] = wrap_degrees((int)lroundf(atan2f(dir.x, dir.z) * RAD2DEG));
  g->pitch[e] = clamp_i16((int)lroundf(asinf(dir.y) * RAD2DEG), -89, 89);
  g->owner[e] = g->owner[pe];
  g->kind[e] = KIND_BULLET;
  g->lifetime[e] = BULLET_LIFETIME_TICKS;
  add_kind_components(g, e, KIND_BULLET);
  return e;
}

static void spawn_casing(GameState *g, u32 pe) {
  Vector3 f, r, u, port;
  u32 h, e;
  if (count_kind(g, KIND_CASING) >= MAX_CASINGS)
    destroy_oldest(g, KIND_CASING);
  e = take_entity(g);
  if (e == ECS_INVALID_ENTITY)
    return;
  clear_entity_data(g, e);
  gun_basis(g, pe, &f, &r, &u);
  port = gun_point(g, pe, f, r, u, 0.72f, GUN_MUZZLE_RIGHT, 0.03f);
  h = mix32(g->tick * 33u + e * 17u + (u32)g->owner[pe] * 91u + (u32)g->px[pe]);
  f32 jx = ((int)(h & 255u) - 128) / 64.0f;
  f32 jy = ((int)((h >> 8) & 255u) - 128) / 96.0f;
  f32 jz = ((int)((h >> 16) & 255u) - 128) / 64.0f;
  g->px[e] = (i16)lroundf(port.x * POS_SCALE);
  g->py[e] = (i16)lroundf(port.y * POS_SCALE);
  g->pz[e] = (i16)lroundf(port.z * POS_SCALE);
  g->vx[e] = (i16)lroundf(r.x * (4.2f + jx) + u.x * (4.5f + jy) - f.x * 0.6f);
  g->vy[e] = (i16)lroundf(5.4f + jy * 2.8f + ((h >> 24) & 7u) * 0.15f);
  g->vz[e] = (i16)lroundf(r.z * (4.2f + jz) + u.z * (4.5f + jy) - f.z * 0.6f);
  g->yaw[e] = wrap_degrees(g->yaw[pe] + 50 + (int)(h % 41u));
  g->pitch[e] = (i16)(18 + (int)((h >> 5) % 29u));
  g->held_by[e] = (u8)h;
  g->owner[e] = g->owner[pe];
  g->kind[e] = KIND_CASING;
  g->lifetime[e] = CASING_LIFETIME_TICKS;
  add_kind_components(g, e, KIND_CASING);
}

static void weapon_system(GameState *g) {
  u32 comps[] = {COMP_POSITION, COMP_AIM, COMP_PLAYER};
  EcsQuery q = ecs_query(&g->ecs, comps, 3);
  while (ecs_next(&q)) {
    u32 e = q.entity;
    u8 p = g->owner[e];
    if (g->cooldown[e] > 0)
      --g->cooldown[e];
    if ((g->input[p].buttons & BTN_FIRE) != 0 && g->cooldown[e] == 0 &&
        spawn_bullet(g, e) != ECS_INVALID_ENTITY) {
      g->cooldown[e] = FIRE_COOLDOWN_TICKS;
      if (g->flash[e] <= PLAYER_FIRE_FLASH_TICKS)
        g->flash[e] = PLAYER_FIRE_FLASH_TICKS;
      spawn_casing(g, e);
    }
    if (g->flash[e] > 0)
      --g->flash[e];
  }
}

static void make_impact_mark(GameState *g, u32 e, bool floor_impact) {
  if (count_kind(g, KIND_IMPACT_MARK) >= MAX_IMPACT_MARKS)
    destroy_oldest(g, KIND_IMPACT_MARK);
  if (floor_impact)
    g->py[e] = 1;
  else {
    g->px[e] = (i16)(g->px[e] - g->vx[e]);
    g->py[e] = (i16)(g->py[e] - g->vy[e]);
    g->pz[e] = (i16)(g->pz[e] - g->vz[e]);
  }
  g->kind[e] = KIND_IMPACT_MARK;
  g->lifetime[e] = IMPACT_MARK_LIFETIME_TICKS;
  g->flash[e] = 3;
  g->vx[e] = g->vy[e] = g->vz[e] = 0;
  ecs_remove(&g->ecs, e, COMP_PROJECTILE);
  ecs_remove(&g->ecs, e, COMP_VELOCITY);
}

static void casing_system(GameState *g) {
  u32 comps[] = {COMP_POSITION, COMP_VELOCITY, COMP_LIFETIME};
  EcsQuery q = ecs_query(&g->ecs, comps, 3);
  while (ecs_next(&q)) {
    u32 e = q.entity;
    if (g->kind[e] != KIND_CASING)
      continue;
    g->px[e] = (i16)(g->px[e] + g->vx[e]);
    g->py[e] = (i16)(g->py[e] + g->vy[e]);
    g->pz[e] = (i16)(g->pz[e] + g->vz[e]);
    if (g->tick % 3u == 0 && g->vy[e] > -8)
      --g->vy[e];
    if (g->py[e] <= 4) {
      g->py[e] = 4;
      g->vy[e] = 0;
      g->vx[e] = (i16)(g->vx[e] * 2 / 3);
      g->vz[e] = (i16)(g->vz[e] * 2 / 3);
      if (abs(g->vx[e]) < 1)
        g->vx[e] = 0;
      if (abs(g->vz[e]) < 1)
        g->vz[e] = 0;
    }
    if (g->vx[e] || g->vy[e] || g->vz[e]) {
      g->yaw[e] = wrap_degrees(g->yaw[e] + 9 + (g->held_by[e] & 7));
      g->pitch[e] = wrap_degrees(g->pitch[e] + 11 + ((g->held_by[e] >> 3) & 7));
      g->held_by[e] = (u8)(g->held_by[e] + 13u);
    }
  }
}

static void mark_hit_dir(GameState *g, u32 victim, u32 from) {
  int deg = (int)lroundf(atan2f((f32)(g->px[from] - g->px[victim]),
                                (f32)(g->pz[from] - g->pz[victim])) *
                         RAD2DEG);
  g->held_by[victim] = pack_yaw(deg);
  g->flash[victim] = PLAYER_HIT_FLASH_TICKS;
}

static void projectile_system(GameState *g) {
  u32 comps[] = {COMP_POSITION, COMP_VELOCITY, COMP_PROJECTILE, COMP_LIFETIME};
  EcsQuery q = ecs_query(&g->ecs, comps, 4);
  int bound = ARENA * POS_SCALE;
  while (ecs_next(&q)) {
    u32 e = q.entity;
    g->px[e] = (i16)(g->px[e] + g->vx[e]);
    g->py[e] = (i16)(g->py[e] + g->vy[e]);
    g->pz[e] = (i16)(g->pz[e] + g->vz[e]);
    bool floor_hit = g->py[e] < 0;
    bool leave = floor_hit;
    bool hit = floor_hit || g->py[e] > bound || g->px[e] < -bound ||
               g->px[e] > bound || g->pz[e] < -bound || g->pz[e] > bound;
    u32 solids[] = {COMP_POSITION, COMP_RENDERABLE};
    EcsQuery sq = ecs_query(&g->ecs, solids, 2);
    while (!hit && ecs_next(&sq)) {
      u32 c = sq.entity;
      if (g->kind[c] == KIND_CUBE && g->held_by[c] == 0 &&
          abs((int)g->px[e] - g->px[c]) < 42 &&
          abs((int)g->py[e] - g->py[c]) < 48 &&
          abs((int)g->pz[e] - g->pz[c]) < 42) {
        hit = true;
        leave = true;
      }
    }
    u32 players[] = {COMP_POSITION, COMP_PLAYER};
    EcsQuery pq = ecs_query(&g->ecs, players, 2);
    while (!hit && ecs_next(&pq)) {
      u32 p = pq.entity;
      if (g->owner[p] != g->owner[e] && abs((int)g->px[e] - g->px[p]) < 32 &&
          g->py[e] < 128 && g->py[e] > 0 &&
          abs((int)g->pz[e] - g->pz[p]) < 32) {
        u32 src = e;
        u8 ow = g->owner[e];
        if (ow < MAX_PLAYERS && g->player_entity[ow] != NET_PACKET_NONE &&
            ecs_alive(&g->ecs, g->player_entity[ow]))
          src = g->player_entity[ow];
        mark_hit_dir(g, p, src);
        hit = true;
      }
    }
    if (hit) {
      if (leave)
        make_impact_mark(g, e, floor_hit);
      else
        ecs_destroy(&g->ecs, e);
    }
  }
}

static void lifetime_system(GameState *g) {
  u32 comps[] = {COMP_POSITION, COMP_LIFETIME};
  EcsQuery q = ecs_query(&g->ecs, comps, 2);
  while (ecs_next(&q)) {
    u32 e = q.entity;
    bool age = g->kind[e] != KIND_IMPACT_MARK || g->tick % 2u == 0;
    if (age && g->lifetime[e] > 0)
      --g->lifetime[e];
    if (g->kind[e] == KIND_IMPACT_MARK)
      g->flash[e] = (u8)(g->lifetime[e] > 84 ? 3 : g->lifetime[e] > 42 ? 2 : 1);
    if (g->lifetime[e] == 0)
      ecs_destroy(&g->ecs, e);
  }
}

static void server_tick(GameState *g) {
  aim_system(g);
  movement_system(g);
  holding_system(g);
  weapon_system(g);
  casing_system(g);
  projectile_system(g);
  lifetime_system(g);
  ++g->tick;
}

static void write_snapshot(GameState *g, NetSnapshot *snap, u8 recipient,
                           u16 ack, u32 session) {
  u32 comps[] = {COMP_POSITION, COMP_NETWORKED, COMP_RENDERABLE};
  EcsQuery q;
  memset(snap, 0, sizeof(*snap));
  snap->magic = NET_MAGIC;
  snap->type = NET_PACKET_SNAPSHOT;
  snap->local_player = recipient;
  snap->session = session_wire(session);
  snap->tick = sequence_wire(g->tick);
  snap->acknowledged_sequence = sequence_wire(ack);
  for (u32 p = 0; p < MAX_PLAYERS; ++p)
    memcpy(snap->names[p], g->player_name[p], PLAYER_NAME_LENGTH);
  q = ecs_query(&g->ecs, comps, 3);
  while (snap->count < NET_ENTITY_LIMIT && ecs_next(&q)) {
    u32 e = q.entity;
    NetEntity *w = &snap->entities[snap->count++];
    w->entity = (u8)e;
    w->kind = g->kind[e];
    w->owner = g->owner[e];
    w->flags = g->flash[e];
    w->extra = g->held_by[e];
    w->x = host_i16(g->px[e]);
    w->y = host_i16(g->py[e]);
    w->z = host_i16(g->pz[e]);
    w->yaw = host_i16(g->yaw[e]);
    w->pitch = host_i16(g->pitch[e]);
  }
}

static void send_peer_snapshot(int fd, const Peer *peer,
                               const NetSnapshot *snap) {
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = peer->address;
  address.sin_port = peer->port;
  (void)sendto(fd, snap, sizeof(*snap), 0, (const struct sockaddr *)&address,
               sizeof(address));
}

static int find_peer_session(const GameState *g, u32 session) {
  for (int i = 0; i < MAX_PLAYERS; ++i)
    if (g->peers[i].active && g->peers[i].session == session)
      return i;
  return -1;
}

static bool session_was_retired(const GameState *g, u32 session) {
  for (int i = 0; i < MAX_PLAYERS; ++i)
    if ((!g->peers[i].active && g->peers[i].session == session) ||
        g->peers[i].retired_session == session)
      return true;
  return false;
}

static int peer_with_name(const GameState *g,
                          const u8 name[PLAYER_NAME_LENGTH]) {
  for (int i = 0; i < MAX_PLAYERS; ++i)
    if (memcmp(g->player_name[i], name, PLAYER_NAME_LENGTH) == 0)
      return i;
  return -1;
}

static void remove_peer(GameState *g, int peer_index);

static int add_peer(GameState *g, const struct sockaddr_in *from, u32 session,
                    const u8 name[PLAYER_NAME_LENGTH]) {
  int slot = peer_with_name(g, name);
  if (slot < 0) {
    for (int i = 0; i < MAX_PLAYERS; ++i)
      if (!g->peers[i].active && g->player_entity[i] == NET_PACKET_NONE) {
        slot = i;
        break;
      }
  }
  if (slot < 0)
    return -1;
  if (g->peers[slot].active)
    remove_peer(g, slot);
  u32 e = create_player(g, (u8)slot);
  if (e == ECS_INVALID_ENTITY)
    return -1;
  Peer *peer = &g->peers[slot];
  if (peer->session != session)
    peer->retired_session = peer->session;
  peer->session = session;
  peer->address = from->sin_addr.s_addr;
  peer->port = from->sin_port;
  peer->last_sequence = peer->last_chat_sequence = 0;
  peer->last_seen = g->tick;
  peer->active = 1;
  peer->got_sequence = peer->got_chat_sequence = 0;
  peer->player = (u8)slot;
  memset(&g->input[slot], 0, sizeof(g->input[slot]));
  g->input[slot].aim_yaw = g->yaw[e];
  memcpy(g->player_name[slot], name, PLAYER_NAME_LENGTH);
  g->player_name[slot][PLAYER_NAME_LENGTH] = '\0';
  return slot;
}

static void remove_peer(GameState *g, int peer_index) {
  Peer *peer = &g->peers[peer_index];
  u8 player = peer->player;
  u32 e = g->player_entity[player];
  if (e != NET_PACKET_NONE && ecs_alive(&g->ecs, e))
    ecs_destroy(&g->ecs, e);
  for (u32 i = 0; i < ECS_MAX_ENTITIES; ++i)
    if (g->held_by[i] == player + 1u)
      g->held_by[i] = 0;
  g->player_entity[player] = NET_PACKET_NONE;
  memset(&g->input[player], 0, sizeof(g->input[player]));
  peer->active = peer->got_sequence = peer->got_chat_sequence = 0;
}

static void server_relay_chat(GameState *g, int fd, int sender,
                              NetChatPacket *packet) {
  packet->player = g->peers[sender].player;
  packet->sequence = sequence_wire(g->next_chat_sequence++);
  memcpy(packet->name, g->player_name[packet->player], PLAYER_NAME_LENGTH);
  for (int i = 0; i < MAX_PLAYERS; ++i) {
    if (!g->peers[i].active)
      continue;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = g->peers[i].address;
    address.sin_port = g->peers[i].port;
    packet->session = session_wire(g->peers[i].session);
    (void)sendto(fd, packet, sizeof(*packet), 0,
                 (const struct sockaddr *)&address, sizeof(address));
  }
}

static void server_receive(GameState *g, int fd) {
  u8 buf[sizeof(NetSnapshot)];
  for (;;) {
    struct sockaddr_in from;
    socklen_t from_size = sizeof(from);
    ssize_t n =
        recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_size);
    if (n < 0)
      return;
    if (n < 2 || buf[0] != NET_MAGIC)
      continue;
    if (buf[1] == NET_PACKET_CHAT && (size_t)n == sizeof(NetChatPacket)) {
      NetChatPacket packet;
      memcpy(&packet, buf, sizeof(packet));
      int pi = find_peer_session(g, session_host(packet.session));
      if (pi < 0 || packet.length > CHAT_LENGTH)
        continue;
      Peer *peer = &g->peers[pi];
      u16 seq = sequence_host(packet.sequence);
      if (peer->got_chat_sequence &&
          !sequence_newer(seq, peer->last_chat_sequence))
        continue;
      peer->got_chat_sequence = 1;
      peer->last_chat_sequence = seq;
      peer->last_seen = g->tick;
      peer->address = from.sin_addr.s_addr;
      peer->port = from.sin_port;
      for (u32 i = 0; i < packet.length; ++i)
        if ((packet.text[i] < 'A' || packet.text[i] > 'Z') &&
            (packet.text[i] < '0' || packet.text[i] > '9') &&
            packet.text[i] != ' ')
          packet.text[i] = ' ';
      server_relay_chat(g, fd, pi, &packet);
      continue;
    }
    if ((buf[1] != NET_PACKET_INPUT && buf[1] != NET_PACKET_DISCONNECT) ||
        (size_t)n != sizeof(NetInputPacket))
      continue;
    NetInputPacket packet;
    memcpy(&packet, buf, sizeof(packet));
    u32 session = session_host(packet.session);
    if (session == 0 || !valid_wire_name(packet.name))
      continue;
    int pi = find_peer_session(g, session);
    if (buf[1] == NET_PACKET_DISCONNECT) {
      if (pi >= 0 &&
          memcmp(g->player_name[pi], packet.name, PLAYER_NAME_LENGTH) == 0)
        remove_peer(g, pi);
      continue;
    }
    if (pi < 0) {
      if (packet.claimed_player != NET_PACKET_NONE ||
          session_was_retired(g, session))
        continue;
      pi = add_peer(g, &from, session, packet.name);
      if (pi < 0)
        continue;
    }
    Peer *peer = &g->peers[pi];
    if (memcmp(g->player_name[peer->player], packet.name, PLAYER_NAME_LENGTH) !=
            0 ||
        (packet.claimed_player != NET_PACKET_NONE &&
         packet.claimed_player != peer->player))
      continue;
    u16 seq = sequence_host(packet.sequence);
    if (peer->got_sequence && !sequence_newer(seq, peer->last_sequence))
      continue;
    peer->got_sequence = 1;
    peer->last_sequence = seq;
    peer->last_seen = g->tick;
    peer->address = from.sin_addr.s_addr;
    peer->port = from.sin_port;
    g->input[peer->player].move_x = clamp_i16(packet.move_x, -1, 1);
    g->input[peer->player].move_z = clamp_i16(packet.move_z, -1, 1);
    g->input[peer->player].aim_yaw = wrap_degrees(wire_i16(packet.aim_yaw));
    g->input[peer->player].aim_pitch =
        clamp_i16(wire_i16(packet.aim_pitch), -75, 75);
    g->input[peer->player].buttons =
        packet.buttons & (BTN_FIRE | BTN_GRAB | BTN_JUMP);
  }
}

static void server_expire_peers(GameState *g) {
  for (int i = 0; i < MAX_PLAYERS; ++i)
    if (g->peers[i].active &&
        tick_age(g->tick, g->peers[i].last_seen) > SIM_HZ * 10)
      remove_peer(g, i);
}

static void server_send_all(GameState *g, int fd) {
  NetSnapshot snap;
  for (int i = 0; i < MAX_PLAYERS; ++i) {
    if (!g->peers[i].active)
      continue;
    u16 ack = g->peers[i].got_sequence ? g->peers[i].last_sequence : 0;
    write_snapshot(g, &snap, g->peers[i].player, ack, g->peers[i].session);
    send_peer_snapshot(fd, &g->peers[i], &snap);
  }
}

static void server_run(u16 port) {
  GameState game;
  int fd = open_udp_server(port);
  double current = hires_time_in_seconds(), acc = 0.0;
  struct timespec idle = {0, 1000000L};
  if (fd < 0) {
    fprintf(stderr, "could not bind UDP port %u\n", port);
    return;
  }
  game_init(&game);
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  printf("server listening on UDP %u (Ctrl-C to stop)\n", port);
  while (server_running) {
    double now = hires_time_in_seconds();
    double dt = now - current;
    if (dt > 0.25)
      dt = 0.25;
    current = now;
    acc += dt;
    server_receive(&game, fd);
    while (acc >= SIM_DT) {
      server_tick(&game);
      server_expire_peers(&game);
      if (game.tick % SNAPSHOT_INTERVAL == 0)
        server_send_all(&game, fd);
      acc -= SIM_DT;
    }
    (void)nanosleep(&idle, NULL);
  }
  close(fd);
}

static void show_notice(GameState *g, const u8 *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g->notice, sizeof(g->notice), fmt, ap);
  va_end(ap);
  g->notice_ticks = SIM_HZ * 5;
}

static void renew_client_session(GameState *g) {
  g->session = make_session_id(g->session);
  g->connected = 0;
  g->local_player = NET_PACKET_NONE;
  g->visible_players = 0;
  g->got_snapshot = g->got_chat_sequence = 0;
  g->next_sequence = g->next_chat_sequence = 0;
  g->last_snapshot_tick = g->last_chat_sequence = 0;
  g->snapshot_silence_ticks = 0;
}

static u32 client_entity_for(GameState *g, u8 server_entity, u8 kind) {
  u8 local = g->network_entity[server_entity];
  if (local != NET_PACKET_NONE && ecs_alive(&g->ecs, local) &&
      ecs_has(&g->ecs, local, COMP_NETWORKED) && g->kind[local] == kind)
    return local;
  if (local != NET_PACKET_NONE && ecs_alive(&g->ecs, local))
    ecs_destroy(&g->ecs, local);
  u32 e = ecs_create(&g->ecs);
  if (e == ECS_INVALID_ENTITY)
    return e;
  g->network_entity[server_entity] = (u8)e;
  for (u32 c = 0; c < ECS_MAX_COMPONENTS; ++c)
    ecs_remove(&g->ecs, e, c);
  clear_entity_data(g, e);
  return e;
}

static void apply_snapshot(GameState *g, const NetSnapshot *snap) {
  u8 mask = 0;
  bool was = g->connected != 0;
  for (u8 i = 0; i < snap->count; ++i)
    if (snap->entities[i].kind == KIND_PLAYER &&
        snap->entities[i].owner < MAX_PLAYERS)
      mask |= (u8)(1u << snap->entities[i].owner);
  if (!was)
    show_notice(g, "LINKED");
  else {
    u8 left = (u8)(g->visible_players & ~mask);
    u8 joined = (u8)(mask & ~g->visible_players);
    for (u32 p = 0; p < MAX_PLAYERS; ++p) {
      u8 bit = (u8)(1u << p);
      if (left & bit)
        show_notice(g, "%.*s DROPPED", PLAYER_NAME_LENGTH, g->player_name[p]);
      if (joined & bit)
        show_notice(g, "%.*s LINKED", PLAYER_NAME_LENGTH, snap->names[p]);
    }
  }
  for (u32 p = 0; p < MAX_PLAYERS; ++p) {
    memcpy(g->player_name[p], snap->names[p], PLAYER_NAME_LENGTH);
    g->player_name[p][PLAYER_NAME_LENGTH] = '\0';
  }
  g->visible_players = mask;
  g->snapshot_seen = 0;
  g->local_player = snap->local_player;
  g->last_snapshot_tick = sequence_host(snap->tick);
  g->connected = 1;
  g->snapshot_silence_ticks = 0;
  for (u8 i = 0; i < snap->count; ++i) {
    const NetEntity *w = &snap->entities[i];
    if (w->entity >= ECS_MAX_ENTITIES || w->kind == 0 || w->kind > KIND_CASING)
      continue;
    g->snapshot_seen |= UINT32_C(1) << w->entity;
    u8 mapped = g->network_entity[w->entity];
    bool had = mapped != NET_PACKET_NONE && ecs_alive(&g->ecs, mapped) &&
               g->kind[mapped] == w->kind;
    u32 e = client_entity_for(g, w->entity, w->kind);
    if (e == ECS_INVALID_ENTITY)
      continue;
    i16 nx = wire_i16(w->x), ny = wire_i16(w->y), nz = wire_i16(w->z);
    i16 dx = (i16)(nx - g->px[e]);
    i16 dy = (i16)(ny - g->py[e]);
    i16 dz = (i16)(nz - g->pz[e]);
    bool snap_pos =
        !had || abs(dx) > 240 || abs(dz) > 240 || dy > 48 || dy < -240;
    g->vx[e] = snap_pos ? 0 : dx;
    g->vy[e] = snap_pos ? 0 : dy;
    g->vz[e] = snap_pos ? 0 : dz;
    if (had && w->kind == KIND_PLAYER) {
      g->cooldown[e] = (u8)clamp_i16(
          wrap_degrees(wire_i16(w->yaw) - g->yaw[e]) + 128, 0, 255);
      g->lifetime[e] = (u8)clamp_i16(
          (int)wire_i16(w->pitch) - (int)g->pitch[e] + 128, 0, 255);
    } else if (w->kind == KIND_PLAYER) {
      g->cooldown[e] = 128;
      g->lifetime[e] = 128;
    }
    g->kind[e] = w->kind;
    g->owner[e] = w->owner;
    g->px[e] = nx;
    g->py[e] = ny;
    g->pz[e] = nz;
    g->yaw[e] = wire_i16(w->yaw);
    g->pitch[e] = wire_i16(w->pitch);
    g->flash[e] = w->flags;
    g->held_by[e] = w->extra;
    add_kind_components(g, e, w->kind);
  }
  g->interpolation_ticks = 0;
  for (u32 s = 0; s < ECS_MAX_ENTITIES; ++s) {
    u8 local = g->network_entity[s];
    if (local != NET_PACKET_NONE &&
        (g->snapshot_seen & (UINT32_C(1) << s)) == 0) {
      if (ecs_alive(&g->ecs, local))
        ecs_destroy(&g->ecs, local);
      g->network_entity[s] = NET_PACKET_NONE;
    }
  }
}

static void client_receive(GameState *g, int fd) {
  u8 buf[sizeof(NetSnapshot)];
  for (;;) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        if (g->connected)
          show_notice(g, "DROPPED");
        renew_client_session(g);
      }
      return;
    }
    if (n < 2 || (size_t)n > sizeof(NetSnapshot) || buf[0] != NET_MAGIC)
      continue;
    if (buf[1] == NET_PACKET_CHAT && (size_t)n == sizeof(NetChatPacket)) {
      NetChatPacket packet;
      memcpy(&packet, buf, sizeof(packet));
      u16 seq = sequence_host(packet.sequence);
      if (session_host(packet.session) != g->session ||
          packet.length > CHAT_LENGTH || !valid_wire_name(packet.name) ||
          (g->got_chat_sequence && !sequence_newer(seq, g->last_chat_sequence)))
        continue;
      g->got_chat_sequence = 1;
      g->last_chat_sequence = seq;
      show_notice(g, "%.*s: %.*s", PLAYER_NAME_LENGTH, packet.name,
                  packet.length, packet.text);
      continue;
    }
    if (buf[1] != NET_PACKET_SNAPSHOT || (size_t)n != sizeof(NetSnapshot))
      continue;
    NetSnapshot snap;
    memcpy(&snap, buf, sizeof(snap));
    u16 tick = sequence_host(snap.tick);
    if (snap.count > NET_ENTITY_LIMIT ||
        session_host(snap.session) != g->session ||
        (g->got_snapshot && !sequence_newer(tick, g->last_snapshot_tick)))
      continue;
    g->got_snapshot = 1;
    apply_snapshot(g, &snap);
  }
}

static void client_send_input(GameState *g, int fd, u8 type) {
  NetInputPacket packet;
  memset(&packet, 0, sizeof(packet));
  packet.magic = NET_MAGIC;
  packet.type = type;
  packet.claimed_player = g->local_player;
  packet.session = session_wire(g->session);
  memcpy(packet.name, g->requested_name, PLAYER_NAME_LENGTH);
  if (type == NET_PACKET_INPUT) {
    packet.buttons = g->input[0].buttons & (BTN_FIRE | BTN_GRAB | BTN_JUMP);
    packet.sequence = sequence_wire(g->next_sequence++);
    packet.move_x = g->input[0].move_x;
    packet.move_z = g->input[0].move_z;
    packet.aim_yaw = host_i16(g->view_yaw);
    packet.aim_pitch = host_i16(g->view_pitch);
  }
  (void)send(fd, &packet, sizeof(packet), 0);
}

static void client_send_chat(GameState *g, int fd) {
  NetChatPacket packet;
  memset(&packet, 0, sizeof(packet));
  packet.magic = NET_MAGIC;
  packet.type = NET_PACKET_CHAT;
  packet.player = g->local_player;
  packet.length = g->chat_length;
  packet.session = session_wire(g->session);
  packet.sequence = sequence_wire(g->next_chat_sequence++);
  memcpy(packet.name, g->requested_name, PLAYER_NAME_LENGTH);
  memcpy(packet.text, g->chat_input, g->chat_length);
  (void)send(fd, &packet, sizeof(packet), 0);
}

static void update_input(GameState *g, int fd) {
  g->look_delta_x = g->look_delta_y = 0.0f;
  if (!g->chatting && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_T))) {
    g->chatting = 1;
    g->chat_length = 0;
    g->chat_input[0] = '\0';
    g->input[0].move_x = g->input[0].move_z = 0;
    g->input[0].buttons = 0;
    while (GetKeyPressed() != 0) {
    }
    (void)GetMouseDelta();
    return;
  }
  if (g->chatting) {
    int c;
    g->input[0].move_x = g->input[0].move_z = 0;
    g->input[0].buttons = 0;
    if (IsKeyPressed(KEY_ESCAPE)) {
      g->chatting = g->chat_length = 0;
      g->chat_input[0] = '\0';
    } else if (IsKeyPressed(KEY_ENTER)) {
      if (g->chat_length > 0)
        client_send_chat(g, fd);
      g->chatting = g->chat_length = 0;
      g->chat_input[0] = '\0';
    } else {
      if (IsKeyPressed(KEY_BACKSPACE) && g->chat_length > 0)
        --g->chat_length;
      while ((c = GetKeyPressed()) != 0) {
        if (c >= 'a' && c <= 'z')
          c -= 'a' - 'A';
        if (((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ') &&
            g->chat_length < CHAT_LENGTH)
          g->chat_input[g->chat_length++] = (u8)c;
      }
      g->chat_input[g->chat_length] = '\0';
    }
    (void)GetMouseDelta();
    return;
  }
  int mx = (IsKeyDown(KEY_D) ? 1 : 0) - (IsKeyDown(KEY_A) ? 1 : 0);
  int mz = (IsKeyDown(KEY_W) ? 1 : 0) - (IsKeyDown(KEY_S) ? 1 : 0);
  g->input[0].move_x = (i8)mx;
  g->input[0].move_z = (i8)mz;
  g->input[0].buttons = 0;
  if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    g->input[0].buttons |= BTN_FIRE;
  if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsKeyDown(KEY_E))
    g->input[0].buttons |= BTN_GRAB;
  if (IsKeyDown(KEY_SPACE))
    g->input[0].buttons |= BTN_JUMP;
  if (IsKeyPressed(KEY_F1))
    g->help ^= 1u;
  if (IsKeyPressed(KEY_ESCAPE)) {
    g->mouse_captured ^= 1u;
    if (g->mouse_captured)
      DisableCursor();
    else
      EnableCursor();
  }
  if (!g->mouse_captured)
    return;
  Vector2 mouse = GetMouseDelta();
  g->look_delta_x = mouse.x;
  g->look_delta_y = mouse.y;
  g->view_yaw = wrap_degrees((int)g->view_yaw - (int)lroundf(mouse.x));
  g->view_pitch =
      clamp_i16((int)g->view_pitch - (int)lroundf(mouse.y), -75, 75);
}

static u32 local_player_entity(const GameState *g) {
  u32 comps[] = {COMP_PLAYER, COMP_POSITION};
  EcsQuery q = ecs_query(&g->ecs, comps, 2);
  while (ecs_next(&q))
    if (g->owner[q.entity] == g->local_player)
      return q.entity;
  return ECS_INVALID_ENTITY;
}

static f32 xf(u32 e, f32 a, const i16 *p, const i16 *d) {
  return ((f32)p[e] - (f32)d[e] * (1.0f - a)) / (f32)POS_SCALE;
}

static Vector3 entity_pos(const GameState *g, u32 e, f32 a) {
  return (Vector3){xf(e, a, g->px, g->vx), xf(e, a, g->py, g->vy),
                   xf(e, a, g->pz, g->vz)};
}

static Camera3D make_camera(const GameState *g, f32 a) {
  Camera3D cam = {0};
  u32 e = local_player_entity(g);
  Vector3 dir = {0.0f, 0.0f, 1.0f};
  cam.position = (Vector3){0.0f, 1.5f, 0.0f};
  if (e != ECS_INVALID_ENTITY) {
    Vector3 p = entity_pos(g, e, a);
    cam.position = (Vector3){p.x, 1.5f + p.y, p.z};
    aim_dir(g->view_yaw, g->view_pitch, &dir);
  }
  cam.target = (Vector3){cam.position.x + dir.x, cam.position.y + dir.y,
                         cam.position.z + dir.z};
  cam.up = (Vector3){0.0f, 1.0f, 0.0f};
  cam.fovy = 72.0f;
  cam.projection = CAMERA_PERSPECTIVE;
  return cam;
}

static void draw_gun_local(f32 recoil, f32 slide, f32 scale, bool firing) {
  rlPushMatrix();
  rlScalef(scale, scale, scale);
  Vector3 sl = {-0.32f, 0.0f, 0.30f - recoil - slide};
  Vector3 sls = {0.16f, 0.13f, 0.46f};
  Vector3 gr = {-0.32f, -0.15f, 0.18f - recoil};
  Vector3 grs = {0.13f, 0.26f, 0.15f};
  DrawCubeV(sl, sls, PAL_MID);
  DrawCubeWiresV(sl, sls, PAL_DARK);
  DrawCubeV(gr, grs, PAL_PURPLE);
  DrawCubeWiresV(gr, grs, PAL_DARK);
  if (firing)
    DrawCylinderEx((Vector3){-0.32f, 0.0f, 0.56f - recoil},
                   (Vector3){-0.32f, 0.0f, 0.72f - recoil}, 0.065f, 0.0f, 5,
                   PAL_LIGHT);
  rlPopMatrix();
}

static void draw_cube_lit(Vector3 p, Vector3 s, Color fill) {
  DrawCubeV(p, s, fill);
  DrawCubeWiresV(p, s, PAL_DARK);
}

static void draw_remote_player(const GameState *g, u32 e, Vector3 pos, f32 a) {
  bool hurt = g->flash[e] > PLAYER_FIRE_FLASH_TICKS;
  bool firing = g->flash[e] > 0 && g->flash[e] <= PLAYER_FIRE_FLASH_TICKS;
  f32 fire = firing ? (f32)g->flash[e] / (f32)PLAYER_FIRE_FLASH_TICKS : 0.0f;
  f32 lag = 1.0f - a;
  f32 yaw_lag = ((int)g->cooldown[e] - 128) * lag;
  f32 pitch_lag = ((int)g->lifetime[e] - 128) * lag;
  Color body = hurt ? PAL_LIGHT : PAL_MID;
  rlPushMatrix();
  rlTranslatef(pos.x, pos.y, pos.z);
  rlRotatef((f32)g->yaw[e], 0.0f, 1.0f, 0.0f);
  draw_cube_lit((Vector3){0.0f, 0.72f, 0.0f}, (Vector3){0.50f, 1.36f, 0.30f},
                body);
  draw_cube_lit((Vector3){0.0f, 1.44f, 0.06f}, (Vector3){0.26f, 0.26f, 0.26f},
                hurt ? PAL_LIGHT : PAL_PURPLE);
  rlPushMatrix();
  rlTranslatef(-0.22f - yaw_lag * 0.012f, 1.16f + pitch_lag * 0.006f, 0.30f);
  rlRotatef((f32)-g->pitch[e] - pitch_lag * 0.55f, 1.0f, 0.0f, 0.0f);
  rlRotatef(-yaw_lag * 0.55f, 0.0f, 1.0f, 0.0f);
  rlRotatef(-yaw_lag * 0.18f, 0.0f, 0.0f, 1.0f);
  draw_gun_local(fire * 0.03f, fire * 0.08f, 1.0f, firing);
  rlPopMatrix();
  rlPopMatrix();
}

static void draw_casing(const GameState *g, u32 e, Vector3 p) {
  f32 roll = (f32)g->held_by[e] * (360.0f / 256.0f);
  rlPushMatrix();
  rlTranslatef(p.x, p.y, p.z);
  rlRotatef((f32)g->yaw[e], 0.0f, 1.0f, 0.0f);
  rlRotatef((f32)g->pitch[e], 1.0f, 0.0f, 0.0f);
  rlRotatef(roll, 0.0f, 0.0f, 1.0f);
  DrawCylinderEx((Vector3){0.0f, -0.068f, 0.0f}, (Vector3){0.0f, 0.068f, 0.0f},
                 0.030f, 0.026f, 6, PAL_DARK);
  DrawCylinderEx((Vector3){0.0f, -0.065f, 0.0f}, (Vector3){0.0f, 0.065f, 0.0f},
                 0.028f, 0.024f, 6, PAL_LIGHT);
  DrawCylinderEx((Vector3){0.0f, -0.071f, 0.0f}, (Vector3){0.0f, -0.061f, 0.0f},
                 0.034f, 0.034f, 6, PAL_MID);
  rlPopMatrix();
}

static void draw_world(const GameState *g, f32 a) {
  u32 comps[] = {COMP_POSITION, COMP_RENDERABLE};
  EcsQuery q = ecs_query(&g->ecs, comps, 2);
  while (ecs_next(&q)) {
    u32 e = q.entity;
    Vector3 p = entity_pos(g, e, a);
    switch (g->kind[e]) {
    case KIND_CUBE:
      draw_cube_lit(p, (Vector3){0.9f, 1.0f, 0.9f},
                    g->held_by[e] ? PAL_LIGHT : PAL_MID);
      break;
    case KIND_PLAYER:
      if (g->owner[e] != g->local_player)
        draw_remote_player(g, e, p, a);
      break;
    case KIND_BULLET: {
      Vector3 dir;
      aim_dir(g->yaw[e], g->pitch[e], &dir);
      Vector3 shoulder = {p.x - dir.x * 0.20f, p.y - dir.y * 0.20f,
                          p.z - dir.z * 0.20f};
      Vector3 base = {shoulder.x - dir.x * 0.34f, shoulder.y - dir.y * 0.34f,
                      shoulder.z - dir.z * 0.34f};
      DrawCylinderEx(base, shoulder, 0.040f, 0.040f, 7, PAL_DARK);
      DrawCylinderEx(base, shoulder, 0.032f, 0.032f, 7, PAL_MID);
      DrawCylinderEx(shoulder, p, 0.032f, 0.0f, 7, PAL_LIGHT);
      break;
    }
    case KIND_CASING:
      draw_casing(g, e, p);
      break;
    case KIND_IMPACT_MARK: {
      f32 s = g->flash[e] >= 3 ? 0.18f : g->flash[e] == 2 ? 0.13f : 0.08f;
      Color c = g->flash[e] >= 3   ? PAL_LIGHT
                : g->flash[e] == 2 ? PAL_MID
                                   : PAL_DARK;
      DrawCubeV(p, (Vector3){s, s, s}, c);
      break;
    }
    }
  }
}

static int glyph_index(u8 c) {
  for (u32 i = 0; i < TOM_THUMB_3X5_COUNT; ++i)
    if (TOM_THUMB_3X5_CHARS[i] == c)
      return (int)i;
  return -1;
}

static void draw_small_text(int x, int y, const u8 *text, Color color) {
  for (int i = 0; text[i]; ++i) {
    int idx = glyph_index(text[i]);
    if (idx < 0)
      continue;
    for (int row = 0; row < 5; ++row)
      for (int col = 0; col < 3; ++col)
        if (tom_thumb_3x5[idx] & (1u << ((4 - row) * 3 + 2 - col)))
          DrawRectangle(x + i * 4 + col, y + row, 1, 1, color);
  }
}

static void draw_number(int x, int y, unsigned value, Color color) {
  u8 digits[8];
  int n = 0;
  do {
    digits[n++] = (u8)('0' + value % 10u);
    value /= 10u;
  } while (value && n < (int)sizeof(digits));
  for (int i = 0; i < n; ++i) {
    u8 t[2] = {digits[n - 1 - i], 0};
    draw_small_text(x + i * 4, y, t, color);
  }
}

static int small_text_width(const u8 *text) {
  size_t n = strlen(text);
  return n ? (int)n * 4 - 1 : 0;
}

static void draw_hit_indicator(const GameState *g, u32 e) {
  if (g->flash[e] <= PLAYER_FIRE_FLASH_TICKS)
    return;
  int rel = wrap_degrees(unpack_yaw(g->held_by[e]) - g->view_yaw);
  f32 r = (f32)rel * DEG2RAD;
  f32 s = sinf(r);
  f32 f = cosf(r);
  Color c = g->flash[e] > PLAYER_HIT_FLASH_TICKS / 2 ? PAL_LIGHT : PAL_MID;
  int cx = LOGICAL_W / 2, cy = LOGICAL_H / 2;
  int len = g->flash[e] > 12 ? 14 : 10;
  if (s < -0.20f) {
    int y = clamp_i16(cy - (int)(f * 18.0f) - len / 2, 2, LOGICAL_H - len - 2);
    DrawRectangle(0, y, 3, len, c);
    DrawRectangle(3, y + len / 2 - 2, 2, 4, c);
  }
  if (s > 0.20f) {
    int y = clamp_i16(cy - (int)(f * 18.0f) - len / 2, 2, LOGICAL_H - len - 2);
    DrawRectangle(LOGICAL_W - 3, y, 3, len, c);
    DrawRectangle(LOGICAL_W - 5, y + len / 2 - 2, 2, 4, c);
  }
  if (f > 0.50f) {
    int x = clamp_i16(cx + (int)(s * 22.0f) - len / 2, 2, LOGICAL_W - len - 2);
    DrawRectangle(x, 0, len, 3, c);
  }
  if (f < -0.50f) {
    int x = clamp_i16(cx + (int)(s * 22.0f) - len / 2, 2, LOGICAL_W - len - 2);
    DrawRectangle(x, LOGICAL_H - 3, len, 3, c);
  }
}

static void draw_hud(const GameState *g) {
  u32 e = local_player_entity(g);
  unsigned fps = (unsigned)GetFPS();
  unsigned copy = fps;
  int digits = 1;
  while (copy >= 10u) {
    copy /= 10u;
    ++digits;
  }
  int fps_w = 15 + digits * 4;
  int fps_x = LOGICAL_W - fps_w - 2;
  if (!g->chatting && g->notice_ticks && g->notice[0]) {
    int w = small_text_width(g->notice);
    int y = w + 4 < fps_x ? 1 : 9;
    DrawRectangle(1, y, w + 2, 7, PAL_DARK);
    draw_small_text(2, y + 1, g->notice, PAL_LIGHT);
  }
  if (!g->chatting) {
    DrawRectangle(fps_x - 1, 1, fps_w + 2, 7, PAL_DARK);
    draw_small_text(fps_x, 2, "FPS", PAL_MID);
    draw_number(fps_x + 16, 2, fps, PAL_LIGHT);
  }
  if (e != ECS_INVALID_ENTITY)
    draw_hit_indicator(g, e);
  DrawRectangle(LOGICAL_W / 2, LOGICAL_H / 2, 1, 1, PAL_LIGHT);
  if (g->help) {
    DrawRectangle(1, 17, 42, 16, PAL_DARK);
    DrawRectangle(47, 17, 44, 16, PAL_DARK);
    draw_small_text(2, 19, "WASD MOVE", PAL_LIGHT);
    draw_small_text(2, 26, "MOUSE AIM", PAL_LIGHT);
    draw_small_text(48, 19, "SPACE JUMP", PAL_LIGHT);
    draw_small_text(48, 26, "E HOLD", PAL_LIGHT);
  }
  if (g->chatting) {
    int w = 8 + small_text_width(g->chat_input);
    if (w < 7)
      w = 7;
    if (w > LOGICAL_W - 4)
      w = LOGICAL_W - 4;
    DrawRectangle(1, 1, w + 2, 7, PAL_DARK);
    draw_small_text(2, 2, "T:", PAL_MID);
    draw_small_text(10, 2, g->chat_input, PAL_LIGHT);
  }
}

static void update_viewmodel(GameState *g, f32 dt) {
  if (dt > 0.05f)
    dt = 0.05f;
  f32 tx = g->look_delta_x * 0.038f;
  f32 ty = -g->look_delta_y * 0.030f;
  tx = fmaxf(-0.42f, fminf(0.42f, tx));
  ty = fmaxf(-0.32f, fminf(0.32f, ty));
  f32 k = 1.0f - expf(-6.5f * dt);
  g->gun_sway_x += (tx - g->gun_sway_x) * k;
  g->gun_sway_y += (ty - g->gun_sway_y) * k;
  g->gun_slide *= expf(-22.0f * dt);
  g->gun_recoil_velocity +=
      (-75.0f * g->gun_recoil - 15.0f * g->gun_recoil_velocity) * dt;
  g->gun_recoil += g->gun_recoil_velocity * dt;
  if (g->gun_recoil < 0.0f)
    g->gun_recoil = g->gun_recoil_velocity = 0.0f;
  if (g->gun_recoil > 0.09f)
    g->gun_recoil = 0.09f;
}

static void render_frame(GameState *g, RenderTexture2D target, f32 a) {
  Camera3D cam = make_camera(g, a);
  BeginTextureMode(target);
  ClearBackground(PAL_DARK);
  BeginMode3D(cam);
  DrawPlane((Vector3){0.0f, -0.02f, 0.0f}, (Vector2){32.0f, 32.0f}, PAL_PURPLE);
  draw_world(g, a);
  EndMode3D();
  Camera3D wcam = {0};
  wcam.position = (Vector3){0.0f, 0.0f, 0.0f};
  wcam.target = (Vector3){0.0f, 0.0f, 1.0f};
  wcam.up = (Vector3){0.0f, 1.0f, 0.0f};
  wcam.fovy = 72.0f;
  wcam.projection = CAMERA_PERSPECTIVE;
  BeginMode3D(wcam);
  rlPushMatrix();
  rlTranslatef(g->gun_sway_x * 1.05f, -0.18f + g->gun_sway_y * 1.05f, 0.75f);
  rlRotatef(g->gun_sway_x * -48.0f, 0.0f, 1.0f, 0.0f);
  rlRotatef(g->gun_sway_y * -38.0f, 1.0f, 0.0f, 0.0f);
  rlRotatef(g->gun_sway_x * -30.0f, 0.0f, 0.0f, 1.0f);
  draw_gun_local(g->gun_recoil / 1.35f, g->gun_slide / 1.35f, 1.35f,
                 g->gun_kick != 0);
  rlPopMatrix();
  EndMode3D();
  draw_hud(g);
  EndTextureMode();

  BeginDrawing();
  ClearBackground(PAL_DARK);
  int scale = GetScreenWidth() / LOGICAL_W;
  int hs = GetScreenHeight() / LOGICAL_H;
  if (hs < scale)
    scale = hs;
  if (scale < 1)
    scale = 1;
  int w = LOGICAL_W * scale, h = LOGICAL_H * scale;
  Rectangle src = {0.0f, 0.0f, (f32)LOGICAL_W, -(f32)LOGICAL_H};
  Rectangle dst = {(f32)((GetScreenWidth() - w) / 2),
                   (f32)((GetScreenHeight() - h) / 2), (f32)w, (f32)h};
  DrawTexturePro(target.texture, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
  EndDrawing();
}

static int client_run(const u8 *host, u16 port, const u8 *name) {
  GameState game;
  struct sockaddr_in server;
  int fd = open_udp_client(host, port, &server);
  double current = hires_time_in_seconds(), acc = 0.0;
  if (fd < 0) {
    fprintf(stderr, "host must be an IPv4 address or localhost\n");
    return 1;
  }
  game_init(&game);
  renew_client_session(&game);
  game.view_yaw = 180;
  memcpy(game.requested_name, name, PLAYER_NAME_LENGTH + 1);
  memcpy(game.player_name[0], name, PLAYER_NAME_LENGTH + 1);
  show_notice(&game, "LINKING");
  game.mouse_captured = 1;
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
  InitWindow(920, 640, "voidryl");
  SetWindowMinSize(LOGICAL_W, LOGICAL_H);
  DisableCursor();
  RenderTexture2D target = LoadRenderTexture(LOGICAL_W, LOGICAL_H);
  SetTextureFilter(target.texture, TEXTURE_FILTER_POINT);
  while (!WindowShouldClose()) {
    double now = hires_time_in_seconds();
    double dt = now - current;
    if (dt > 0.25)
      dt = 0.25;
    current = now;
    acc += dt;
    update_input(&game, fd);
    client_receive(&game, fd);
    while (acc >= SIM_DT) {
      if (++game.command_ticks >= COMMAND_INTERVAL) {
        client_send_input(&game, fd, NET_PACKET_INPUT);
        game.command_ticks = 0;
      }
      if (game.interpolation_ticks < SNAPSHOT_INTERVAL)
        ++game.interpolation_ticks;
      if (game.notice_ticks > 0)
        --game.notice_ticks;
      if (game.connected && ++game.snapshot_silence_ticks > SIM_HZ * 2) {
        renew_client_session(&game);
        show_notice(&game, "DROPPED");
      }
      if (game.fire_cooldown > 0)
        --game.fire_cooldown;
      if (game.gun_kick > 0)
        --game.gun_kick;
      if ((game.input[0].buttons & BTN_FIRE) && game.fire_cooldown == 0) {
        game.fire_cooldown = FIRE_COOLDOWN_TICKS;
        game.gun_kick = 4;
        game.gun_slide = 0.13f;
        game.gun_recoil_velocity += 1.4f;
      }
      acc -= SIM_DT;
    }
    client_receive(&game, fd);
    update_viewmodel(&game, (f32)dt);
    f32 alpha = ((f32)game.interpolation_ticks + (f32)(acc / SIM_DT)) /
                (f32)SNAPSHOT_INTERVAL;
    if (alpha > 1.0f)
      alpha = 1.0f;
    render_frame(&game, target, alpha);
  }
  client_send_input(&game, fd, NET_PACKET_DISCONNECT);
  EnableCursor();
  UnloadRenderTexture(target);
  CloseWindow();
  close(fd);
  return 0;
}

static void print_usage(const u8 *program) {
  fprintf(stderr, "usage: %s --server [port]\n", program);
  fprintf(stderr,
          "       %s --client <ipv4|localhost> <port> <four-letter-name>\n",
          program);
}

int main(int argc, u8 **argv) {
  u16 port;
  u8 name[PLAYER_NAME_LENGTH + 1];
  if (argc >= 2 && strcmp(argv[1], "--server") == 0) {
    port = 4242;
    if (argc >= 3 && !valid_port(argv[2], &port)) {
      print_usage(argv[0]);
      return 1;
    }
    server_run(port);
    return 0;
  }
  if (argc == 5 && strcmp(argv[1], "--client") == 0) {
    if (!valid_port(argv[3], &port) || !valid_player_name(argv[4], name)) {
      print_usage(argv[0]);
      return 1;
    }
    return client_run(argv[2], port, name);
  }
  print_usage(argv[0]);
  return 1;
}
