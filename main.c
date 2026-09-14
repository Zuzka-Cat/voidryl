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
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LOGICAL_W 92
#define LOGICAL_H 64
#define POS_SCALE 64
#define MAX_PLAYERS 3
#define MAX_BULLETS 9
#define MAX_IMPACT_MARKS 4
#define MAX_CASINGS 4
#define NET_ENTITY_LIMIT 28
#define NET_MAGIC 0xB7u
#define NET_INPUT 1u
#define NET_SNAPSHOT 2u
#define NET_CHAT 3u
#define NET_DISCONNECT 4u
#define NET_NONE 255u
#define SIM_HZ 100
#define SIM_DT 0.01
#define SNAPSHOT_HZ 20
#define SNAPSHOT_INTERVAL (SIM_HZ / SNAPSHOT_HZ)
#define COMMAND_HZ 50
#define COMMAND_INTERVAL (SIM_HZ / COMMAND_HZ)
#define CHAT_LENGTH 18
#define PLAYER_NAME_LENGTH 4
#define NOTICE_LENGTH 21
#define BTN_FIRE 1u
#define BTN_GRAB 2u
#define BTN_JUMP 4u
#define FIRE_COOLDOWN_TICKS 16
#define PLAYER_FIRE_FLASH_TICKS 6
#define PLAYER_HIT_FLASH_TICKS 24
#define BULLET_LIFETIME_TICKS 160
#define CASING_LIFETIME_TICKS 180
#define IMPACT_MARK_LIFETIME_TICKS 250
#define PROJECTILE_LIMIT 96
#define BULLET_SPEED 32.0f
#define BULLET_ZERO_DISTANCE 12.0f
#define GUN_MUZZLE_FORWARD 1.46f
#define GUN_MUZZLE_RIGHT 0.42f
#define GUN_MUZZLE_UP -0.18f

#define COMP_POSITION 0u
#define COMP_VELOCITY 1u
#define COMP_AIM 2u
#define COMP_PLAYER 3u
#define COMP_PROJECTILE 4u
#define COMP_LIFETIME 5u
#define COMP_RENDERABLE 6u
#define COMP_NETWORKED 7u

enum EntityKind {
  KIND_CUBE = 1,
  KIND_PLAYER = 2,
  KIND_BULLET = 3,
  KIND_IMPACT_MARK = 4,
  KIND_CASING = 5
};

typedef struct InputCommand {
  int8_t move_x;
  int8_t move_z;
  int16_t aim_yaw;
  int16_t aim_pitch;
  uint8_t buttons;
} InputCommand;

typedef struct Peer {
  uint32_t address;
  uint32_t session;
  uint32_t retired_session;
  uint16_t port;
  uint16_t last_sequence;
  uint16_t last_chat_sequence;
  uint16_t last_seen;
  uint8_t active;
  uint8_t got_sequence;
  uint8_t got_chat_sequence;
  uint8_t player;
} Peer;

typedef struct NetInputPacket {
  uint8_t magic;
  uint8_t type;
  uint8_t claimed_player;
  uint8_t buttons;
  uint32_t session;
  uint16_t sequence;
  int8_t move_x;
  int8_t move_z;
  int16_t aim_yaw;
  int16_t aim_pitch;
  char name[PLAYER_NAME_LENGTH];
} NetInputPacket;

typedef struct NetEntity {
  uint8_t entity;
  uint8_t kind;
  uint8_t owner;
  uint8_t flags;
  int16_t x;
  int16_t y;
  int16_t z;
  int16_t yaw;
  int16_t pitch;
} NetEntity;

typedef struct NetChatPacket {
  uint8_t magic;
  uint8_t type;
  uint8_t player;
  uint8_t length;
  uint32_t session;
  uint16_t sequence;
  char name[PLAYER_NAME_LENGTH];
  char text[CHAT_LENGTH];
} NetChatPacket;

typedef struct NetSnapshot {
  uint8_t magic;
  uint8_t type;
  uint8_t local_player;
  uint8_t count;
  uint32_t session;
  uint16_t tick;
  uint16_t acknowledged_sequence;
  NetEntity entities[NET_ENTITY_LIMIT];
  char names[MAX_PLAYERS][PLAYER_NAME_LENGTH];
} NetSnapshot;

/* All persistent mutable gameplay, interpolation, chat, and protocol
   storage is in this aggregate. RenderTexture, socket descriptors, and
   derived Camera3D values are transport/render handles, not gameplay state. */
typedef struct GameState {
  ECS ecs;
  int16_t px[ECS_MAX_ENTITIES];
  int16_t py[ECS_MAX_ENTITIES];
  int16_t pz[ECS_MAX_ENTITIES];
  int16_t vx[ECS_MAX_ENTITIES];
  int16_t vy[ECS_MAX_ENTITIES];
  int16_t vz[ECS_MAX_ENTITIES];
  int16_t yaw[ECS_MAX_ENTITIES];
  int16_t pitch[ECS_MAX_ENTITIES];
  int16_t previous_px[ECS_MAX_ENTITIES];
  int16_t previous_py[ECS_MAX_ENTITIES];
  int16_t previous_pz[ECS_MAX_ENTITIES];
  uint8_t kind[ECS_MAX_ENTITIES];
  uint8_t lifetime[ECS_MAX_ENTITIES];
  uint8_t owner[ECS_MAX_ENTITIES];
  uint8_t cooldown[ECS_MAX_ENTITIES];
  uint8_t flash[ECS_MAX_ENTITIES];
  uint8_t held_by[ECS_MAX_ENTITIES];
  InputCommand input[MAX_PLAYERS];
  Peer peers[MAX_PLAYERS];
  uint8_t player_entity[MAX_PLAYERS];
  uint8_t network_entity[ECS_MAX_ENTITIES];
  uint32_t snapshot_seen;
  uint32_t session;
  uint16_t tick;
  uint16_t next_sequence;
  uint16_t next_chat_sequence;
  uint16_t last_snapshot_tick;
  uint16_t last_chat_sequence;
  uint8_t local_player;
  uint8_t connected;
  uint8_t mouse_captured;
  uint8_t help;
  uint8_t chatting;
  uint8_t chat_length;
  uint8_t interpolation_ticks;
  uint8_t command_ticks;
  uint8_t gun_kick;
  uint8_t fire_cooldown;
  uint8_t visible_players;
  uint8_t got_snapshot;
  uint8_t got_chat_sequence;
  char requested_name[PLAYER_NAME_LENGTH + 1];
  char player_name[MAX_PLAYERS][PLAYER_NAME_LENGTH + 1];
  char chat_input[CHAT_LENGTH + 1];
  char notice[NOTICE_LENGTH + 1];
  uint16_t notice_ticks;
  uint16_t snapshot_silence_ticks;
  int16_t view_yaw;
  int16_t view_pitch;
  float gun_recoil;
  float gun_recoil_velocity;
  float gun_slide;
  float gun_sway_x;
  float gun_sway_y;
  float look_delta_x;
  float look_delta_y;
  uint8_t net_rx[sizeof(NetSnapshot)];
  uint8_t net_tx[sizeof(NetSnapshot)];
} GameState;

_Static_assert(sizeof(NetInputPacket) == 20, "input packet layout changed");
_Static_assert(sizeof(NetEntity) == 14, "snapshot entity layout changed");
_Static_assert(sizeof(NetChatPacket) == 32, "chat packet layout changed");
_Static_assert(sizeof(NetSnapshot) == 416, "snapshot layout changed");
_Static_assert(sizeof(GameState) <= 4096, "game state exceeds 4 KiB");

_Static_assert(ECS_MAX_ENTITIES <= 32u, "network bitsets assume 32 entities");

static const Color PAL_DARK = {16, 8, 24, 255};
static const Color PAL_PURPLE = {60, 30, 102, 255};
static const Color PAL_MID = {155, 123, 208, 255};
static const Color PAL_LIGHT = {246, 213, 232, 255};

static const char *OUTLINE_FRAGMENT_SHADER =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec2 texelSize;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "  vec4 center = texture(texture0, fragTexCoord);\n"
    "  vec3 left = texture(texture0, fragTexCoord - vec2(texelSize.x, 0.0)).rgb;\n"
    "  vec3 right = texture(texture0, fragTexCoord + vec2(texelSize.x, 0.0)).rgb;\n"
    "  vec3 up = texture(texture0, fragTexCoord - vec2(0.0, texelSize.y)).rgb;\n"
    "  vec3 down = texture(texture0, fragTexCoord + vec2(0.0, texelSize.y)).rgb;\n"
    "  float edge = max(max(length(center.rgb - left), length(center.rgb - right)),\n"
    "                   max(length(center.rgb - up), length(center.rgb - down)));\n"
    "  vec3 luma = vec3(0.2126, 0.7152, 0.0722);\n"
    "  float centerLuma = dot(center.rgb, luma);\n"
    "  float darkest = min(min(dot(left, luma), dot(right, luma)),\n"
    "                      min(dot(up, luma), dot(down, luma)));\n"
    "  vec3 ink = vec3(0.062745, 0.031373, 0.094118);\n"
    "  bool darkSide = centerLuma <= darkest + 0.05;\n"
    "  finalColor = vec4(edge > 0.24 && darkSide ? ink : center.rgb, center.a) * fragColor * colDiffuse;\n"
    "}\n";

static volatile sig_atomic_t server_running = 1;

static void on_signal(int signal_number) {
  (void)signal_number;
  server_running = 0;
}

static int16_t clamp_i16(int value, int16_t low, int16_t high) {
  if (value < low)
    return low;
  if (value > high)
    return high;
  return (int16_t)value;
}

static int16_t wrap_degrees(int value) {
  while (value > 180)
    value -= 360;
  while (value < -179)
    value += 360;
  return (int16_t)value;
}

static double hires_time_in_seconds(void) {
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static uint16_t sequence_host(uint16_t sequence) {
  return ntohs(sequence);
}

static uint16_t sequence_wire(uint16_t sequence) {
  return htons(sequence);
}

static uint32_t session_host(uint32_t session) {
  return ntohl(session);
}

static uint32_t session_wire(uint32_t session) {
  return htonl(session);
}

static uint32_t make_session_id(uint32_t previous) {
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  uint32_t value = (uint32_t)now.tv_nsec ^ (uint32_t)now.tv_sec * UINT32_C(0x9e3779b9) ^
                   (uint32_t)getpid() * UINT32_C(0x85ebca6b) ^ previous;
  value ^= value >> 16;
  value *= UINT32_C(0x7feb352d);
  value ^= value >> 15;
  value *= UINT32_C(0x846ca68b);
  value ^= value >> 16;
  return value != 0 ? value : UINT32_C(1);
}

static bool sequence_newer(uint16_t a, uint16_t b) {
  return (int16_t)(a - b) > 0;
}

static int16_t wire_i16(int16_t value) {
  return (int16_t)ntohs((uint16_t)value);
}

static int16_t host_i16(int16_t value) {
  return (int16_t)htons((uint16_t)value);
}

static int tick_age(uint16_t now, uint16_t then) {
  return (int)(uint16_t)(now - then);
}

static bool valid_port(const char *text, uint16_t *port) {
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
  *port = (uint16_t)value;
  return true;
}

static bool valid_player_name(const char *text, char out[PLAYER_NAME_LENGTH + 1]) {
  if (!text || strlen(text) != PLAYER_NAME_LENGTH)
    return false;
  for (int i = 0; i < PLAYER_NAME_LENGTH; ++i) {
    char character = text[i];
    if (character >= 'a' && character <= 'z')
      character = (char)(character - ('a' - 'A'));
    if (character < 'A' || character > 'Z')
      return false;
    out[i] = character;
  }
  out[PLAYER_NAME_LENGTH] = '\0';
  return true;
}

static bool valid_wire_name(const char name[PLAYER_NAME_LENGTH]) {
  for (int i = 0; i < PLAYER_NAME_LENGTH; ++i)
    if (name[i] < 'A' || name[i] > 'Z')
      return false;
  return true;
}

static bool parse_ipv4(const char *text, struct in_addr *address) {
  if (strcmp(text, "localhost") == 0)
    text = "127.0.0.1";
  return inet_pton(AF_INET, text, address) == 1;
}

static void socket_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int open_udp_server(uint16_t port) {
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

static int open_udp_client(const char *host, uint16_t port,
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

static void game_zero(GameState *game) {
  memset(game, 0, sizeof(*game));
  game->local_player = NET_NONE;
  for (uint32_t i = 0; i < ECS_MAX_ENTITIES; ++i)
    game->network_entity[i] = NET_NONE;
  for (uint32_t i = 0; i < MAX_PLAYERS; ++i)
    game->player_entity[i] = NET_NONE;
}

static void clear_entity_data(GameState *game, uint32_t entity) {
  game->px[entity] = 0;
  game->py[entity] = 0;
  game->pz[entity] = 0;
  game->vx[entity] = 0;
  game->vy[entity] = 0;
  game->vz[entity] = 0;
  game->yaw[entity] = 0;
  game->pitch[entity] = 0;
  game->previous_px[entity] = 0;
  game->previous_py[entity] = 0;
  game->previous_pz[entity] = 0;
  game->kind[entity] = 0;
  game->lifetime[entity] = 0;
  game->owner[entity] = 0;
  game->cooldown[entity] = 0;
  game->flash[entity] = 0;
  game->held_by[entity] = 0;
}

static int16_t cube_x(uint32_t index) {
  return (int16_t)(((int)((index * 7u + 3u) % 17u) - 8) * POS_SCALE);
}

static int16_t cube_z(uint32_t index) {
  return (int16_t)(((int)((index * 11u + 5u) % 17u) - 8) * POS_SCALE);
}

static uint32_t create_cube(GameState *game, uint32_t index) {
  uint32_t entity = ecs_create(&game->ecs);
  if (entity == ECS_INVALID_ENTITY)
    return entity;
  clear_entity_data(game, entity);
  game->px[entity] = cube_x(index);
  game->py[entity] = POS_SCALE / 2;
  game->pz[entity] = cube_z(index);
  game->kind[entity] = KIND_CUBE;
  game->previous_px[entity] = game->px[entity];
  game->previous_py[entity] = game->py[entity];
  game->previous_pz[entity] = game->pz[entity];
  ecs_add(&game->ecs, entity, COMP_POSITION);
  ecs_add(&game->ecs, entity, COMP_RENDERABLE);
  ecs_add(&game->ecs, entity, COMP_NETWORKED);
  return entity;
}

static void game_init(GameState *game) {
  game_zero(game);
  for (uint32_t i = 0; i < 8; ++i) {
    uint32_t entity = create_cube(game, i);
    if (entity != ECS_INVALID_ENTITY)
      game->network_entity[entity] = (uint8_t)entity;
  }
}

static uint32_t create_player(GameState *game, uint8_t player) {
  uint32_t entity = ecs_create(&game->ecs);
  if (entity == ECS_INVALID_ENTITY)
    return entity;
  clear_entity_data(game, entity);
  game->px[entity] = (int16_t)((int)player * 3 - 3) * POS_SCALE;
  game->py[entity] = 0;
  game->pz[entity] = 5 * POS_SCALE;
  game->yaw[entity] = 180;
  game->pitch[entity] = 0;
  game->kind[entity] = KIND_PLAYER;
  game->owner[entity] = player;
  game->previous_px[entity] = game->px[entity];
  game->previous_py[entity] = game->py[entity];
  game->previous_pz[entity] = game->pz[entity];
  game->player_entity[player] = (uint8_t)entity;
  ecs_add(&game->ecs, entity, COMP_POSITION);
  ecs_add(&game->ecs, entity, COMP_VELOCITY);
  ecs_add(&game->ecs, entity, COMP_AIM);
  ecs_add(&game->ecs, entity, COMP_PLAYER);
  ecs_add(&game->ecs, entity, COMP_RENDERABLE);
  ecs_add(&game->ecs, entity, COMP_NETWORKED);
  return entity;
}

static bool cube_overlap(const GameState *game, int16_t x, int16_t z,
                         int16_t radius) {
  uint32_t components[] = {COMP_POSITION, COMP_RENDERABLE};
  EcsQuery query = ecs_query(&game->ecs, components, 2);
  while (ecs_next(&query)) {
    uint32_t entity = query.entity;
    if (game->kind[entity] != KIND_CUBE || game->held_by[entity] != 0)
      continue;
    if (abs((int)x - game->px[entity]) < POS_SCALE / 2 + radius &&
        abs((int)z - game->pz[entity]) < POS_SCALE / 2 + radius)
      return true;
  }
  return false;
}

static void aim_vector_angles(int16_t yaw_degrees, int16_t pitch_degrees,
                              Vector3 *out) {
  float yaw = (float)yaw_degrees * DEG2RAD;
  float pitch = (float)pitch_degrees * DEG2RAD;
  float horizontal = cosf(pitch);
  out->x = sinf(yaw) * horizontal;
  out->y = sinf(pitch);
  out->z = cosf(yaw) * horizontal;
}

static void aim_vector(const GameState *game, uint32_t entity, Vector3 *out) {
  float yaw = (float)game->yaw[entity] * DEG2RAD;
  float pitch = (float)game->pitch[entity] * DEG2RAD;
  float horizontal = cosf(pitch);
  out->x = sinf(yaw) * horizontal;
  out->y = sinf(pitch);
  out->z = cosf(yaw) * horizontal;
}

static void movement_system(GameState *game) {
  uint32_t components[] = {COMP_POSITION, COMP_VELOCITY, COMP_PLAYER};
  EcsQuery query = ecs_query(&game->ecs, components, 3);
  while (ecs_next(&query)) {
    uint32_t entity = query.entity;
    uint8_t player = game->owner[entity];
    InputCommand *input = &game->input[player];
    float yaw = (float)game->yaw[entity] * DEG2RAD;
    float dx = (float)input->move_z * sinf(yaw) -
               (float)input->move_x * cosf(yaw);
    float dz = (float)input->move_z * cosf(yaw) +
               (float)input->move_x * sinf(yaw);
    float length = sqrtf(dx * dx + dz * dz);
    if (length > 1.0f) {
      dx /= length;
      dz /= length;
    }
    int16_t step_x = (int16_t)lroundf(dx * 3.0f);
    int16_t step_z = (int16_t)lroundf(dz * 3.0f);
    game->vx[entity] = step_x;
    game->vz[entity] = step_z;
    if ((input->buttons & BTN_JUMP) != 0 && game->py[entity] == 0)
      game->vy[entity] = 6;
    if (game->py[entity] > 0 || game->vy[entity] > 0) {
      game->py[entity] = (int16_t)(game->py[entity] + game->vy[entity]);
      if (game->tick % 6u == 0 && game->vy[entity] > -6)
        --game->vy[entity];
      if (game->py[entity] <= 0) {
        game->py[entity] = 0;
        game->vy[entity] = 0;
      }
    }
    int16_t next_x = clamp_i16((int)game->px[entity] + step_x,
                                -15 * POS_SCALE, 15 * POS_SCALE);
    int16_t next_z = clamp_i16((int)game->pz[entity] + step_z,
                                -15 * POS_SCALE, 15 * POS_SCALE);
    if (!cube_overlap(game, next_x, game->pz[entity], POS_SCALE / 4))
      game->px[entity] = next_x;
    if (!cube_overlap(game, game->px[entity], next_z, POS_SCALE / 4))
      game->pz[entity] = next_z;
  }
}

static void aim_system(GameState *game) {
  uint32_t components[] = {COMP_AIM, COMP_PLAYER};
  EcsQuery query = ecs_query(&game->ecs, components, 2);
  while (ecs_next(&query)) {
    uint32_t entity = query.entity;
    uint8_t player = game->owner[entity];
    game->yaw[entity] = wrap_degrees(game->input[player].aim_yaw);
    game->pitch[entity] = clamp_i16(game->input[player].aim_pitch, -75, 75);
  }
}

static uint32_t held_cube_for(const GameState *game, uint8_t player) {
  uint32_t components[] = {COMP_POSITION, COMP_RENDERABLE, COMP_NETWORKED};
  EcsQuery query = ecs_query(&game->ecs, components, 3);
  while (ecs_next(&query)) {
    uint32_t entity = query.entity;
    if (game->kind[entity] == KIND_CUBE && game->held_by[entity] == player + 1u)
      return entity;
  }
  return ECS_INVALID_ENTITY;
}

static void holding_system(GameState *game) {
  uint32_t players[] = {COMP_POSITION, COMP_AIM, COMP_PLAYER};
  EcsQuery player_query = ecs_query(&game->ecs, players, 3);
  while (ecs_next(&player_query)) {
    uint32_t player_entity = player_query.entity;
    uint8_t player = game->owner[player_entity];
    uint32_t held = held_cube_for(game, player);
    bool wants_hold = (game->input[player].buttons & BTN_GRAB) != 0;
    if (!wants_hold && held != ECS_INVALID_ENTITY) {
      game->held_by[held] = 0;
      held = ECS_INVALID_ENTITY;
    }
    if (wants_hold && held == ECS_INVALID_ENTITY) {
      Vector3 direction;
      uint32_t cubes[] = {COMP_POSITION, COMP_RENDERABLE, COMP_NETWORKED};
      EcsQuery cube_query = ecs_query(&game->ecs, cubes, 3);
      float best_distance = 1000.0f;
      uint32_t best = ECS_INVALID_ENTITY;
      aim_vector(game, player_entity, &direction);
      while (ecs_next(&cube_query)) {
        uint32_t cube = cube_query.entity;
        if (game->kind[cube] != KIND_CUBE || game->held_by[cube] != 0)
          continue;
        Vector3 offset = {
            (float)(game->px[cube] - game->px[player_entity]) / POS_SCALE,
            (float)(game->py[cube] - 96) / POS_SCALE,
            (float)(game->pz[cube] - game->pz[player_entity]) / POS_SCALE};
        float distance = sqrtf(offset.x * offset.x + offset.y * offset.y +
                               offset.z * offset.z);
        float dot = distance > 0.01f
                        ? (offset.x * direction.x + offset.y * direction.y +
                           offset.z * direction.z) /
                              distance
                        : -1.0f;
        if (distance < 5.0f && dot > 0.55f && distance < best_distance) {
          best_distance = distance;
          best = cube;
        }
      }
      if (best != ECS_INVALID_ENTITY) {
        game->held_by[best] = player + 1u;
        held = best;
      }
    }
    if (held != ECS_INVALID_ENTITY) {
      Vector3 direction;
      aim_vector(game, player_entity, &direction);
      game->px[held] = clamp_i16(
          (int)game->px[player_entity] + (int)lroundf(direction.x * 2.0f * POS_SCALE),
          -15 * POS_SCALE, 15 * POS_SCALE);
      game->py[held] = POS_SCALE + 32;
      game->pz[held] = clamp_i16(
          (int)game->pz[player_entity] + (int)lroundf(direction.z * 2.0f * POS_SCALE),
          -15 * POS_SCALE, 15 * POS_SCALE);
    }
  }
}

static uint32_t bullet_count(const GameState *game) {
  uint32_t components[] = {COMP_PROJECTILE};
  return ecs_count(&game->ecs, components, 1);
}

static uint32_t spawn_bullet(GameState *game, uint32_t player_entity) {
  uint32_t entity;
  Vector3 aim;
  if (bullet_count(game) >= MAX_BULLETS)
    return ECS_INVALID_ENTITY;
  entity = ecs_create(&game->ecs);
  if (entity == ECS_INVALID_ENTITY)
    return entity;
  clear_entity_data(game, entity);
  aim_vector(game, player_entity, &aim);
  float yaw = (float)game->yaw[player_entity] * DEG2RAD;
  Vector3 right = {-cosf(yaw), 0.0f, sinf(yaw)};
  Vector3 up = {right.y * aim.z - right.z * aim.y,
                right.z * aim.x - right.x * aim.z,
                right.x * aim.y - right.y * aim.x};
  Vector3 muzzle = {
      (float)game->px[player_entity] / POS_SCALE +
          aim.x * GUN_MUZZLE_FORWARD + right.x * GUN_MUZZLE_RIGHT +
          up.x * GUN_MUZZLE_UP,
      (float)game->py[player_entity] / POS_SCALE + 1.5f +
          aim.y * GUN_MUZZLE_FORWARD + right.y * GUN_MUZZLE_RIGHT +
          up.y * GUN_MUZZLE_UP,
      (float)game->pz[player_entity] / POS_SCALE +
          aim.z * GUN_MUZZLE_FORWARD + right.z * GUN_MUZZLE_RIGHT +
          up.z * GUN_MUZZLE_UP};
  Vector3 target = {
      (float)game->px[player_entity] / POS_SCALE +
          aim.x * BULLET_ZERO_DISTANCE,
      (float)game->py[player_entity] / POS_SCALE + 1.5f +
          aim.y * BULLET_ZERO_DISTANCE,
      (float)game->pz[player_entity] / POS_SCALE +
          aim.z * BULLET_ZERO_DISTANCE};
  Vector3 direction = {target.x - muzzle.x, target.y - muzzle.y,
                       target.z - muzzle.z};
  float length = sqrtf(direction.x * direction.x + direction.y * direction.y +
                       direction.z * direction.z);
  if (length > 0.001f) {
    direction.x /= length;
    direction.y /= length;
    direction.z /= length;
  } else {
    direction = aim;
  }
  game->px[entity] = (int16_t)lroundf(muzzle.x * POS_SCALE);
  game->py[entity] = (int16_t)lroundf(muzzle.y * POS_SCALE);
  game->pz[entity] = (int16_t)lroundf(muzzle.z * POS_SCALE);
  game->vx[entity] = (int16_t)lroundf(direction.x * BULLET_SPEED);
  game->vy[entity] = (int16_t)lroundf(direction.y * BULLET_SPEED);
  game->vz[entity] = (int16_t)lroundf(direction.z * BULLET_SPEED);
  game->yaw[entity] =
      wrap_degrees((int)lroundf(atan2f(direction.x, direction.z) * RAD2DEG));
  game->pitch[entity] =
      clamp_i16((int)lroundf(asinf(direction.y) * RAD2DEG), -89, 89);
  game->owner[entity] = game->owner[player_entity];
  game->kind[entity] = KIND_BULLET;
  game->lifetime[entity] = BULLET_LIFETIME_TICKS;
  game->previous_px[entity] = game->px[entity];
  game->previous_py[entity] = game->py[entity];
  game->previous_pz[entity] = game->pz[entity];
  ecs_add(&game->ecs, entity, COMP_POSITION);
  ecs_add(&game->ecs, entity, COMP_VELOCITY);
  ecs_add(&game->ecs, entity, COMP_PROJECTILE);
  ecs_add(&game->ecs, entity, COMP_LIFETIME);
  ecs_add(&game->ecs, entity, COMP_RENDERABLE);
  ecs_add(&game->ecs, entity, COMP_NETWORKED);
  return entity;
}

static uint32_t casing_count(const GameState *game) {
  uint32_t components[] = {COMP_LIFETIME};
  EcsQuery query = ecs_query(&game->ecs, components, 1);
  uint32_t count = 0;
  while (ecs_next(&query))
    if (game->kind[query.entity] == KIND_CASING)
      ++count;
  return count;
}

static void remove_oldest_casing(GameState *game) {
  uint32_t components[] = {COMP_LIFETIME};
  EcsQuery query = ecs_query(&game->ecs, components, 1);
  uint32_t oldest = ECS_INVALID_ENTITY;
  uint8_t shortest_lifetime = UINT8_MAX;
  while (ecs_next(&query)) {
    uint32_t entity = query.entity;
    if (game->kind[entity] == KIND_CASING &&
        game->lifetime[entity] < shortest_lifetime) {
      oldest = entity;
      shortest_lifetime = game->lifetime[entity];
    }
  }
  if (oldest != ECS_INVALID_ENTITY)
    ecs_destroy(&game->ecs, oldest);
}

static void spawn_casing(GameState *game, uint32_t player_entity) {
  Vector3 aim;
  aim_vector(game, player_entity, &aim);
  float yaw = (float)game->yaw[player_entity] * DEG2RAD;
  Vector3 right = {-cosf(yaw), 0.0f, sinf(yaw)};
  Vector3 up = {right.y * aim.z - right.z * aim.y,
                right.z * aim.x - right.x * aim.z,
                right.x * aim.y - right.y * aim.x};
  if (casing_count(game) >= MAX_CASINGS)
    remove_oldest_casing(game);
  uint32_t entity = ecs_create(&game->ecs);
  if (entity == ECS_INVALID_ENTITY)
    return;
  clear_entity_data(game, entity);
  Vector3 ejection_port = {
      (float)game->px[player_entity] / POS_SCALE + aim.x * 0.72f +
          right.x * GUN_MUZZLE_RIGHT + up.x * 0.03f,
      (float)game->py[player_entity] / POS_SCALE + 1.5f + aim.y * 0.72f +
          right.y * GUN_MUZZLE_RIGHT + up.y * 0.03f,
      (float)game->pz[player_entity] / POS_SCALE + aim.z * 0.72f +
          right.z * GUN_MUZZLE_RIGHT + up.z * 0.03f};
  game->px[entity] = (int16_t)lroundf(ejection_port.x * POS_SCALE);
  game->py[entity] = (int16_t)lroundf(ejection_port.y * POS_SCALE);
  game->pz[entity] = (int16_t)lroundf(ejection_port.z * POS_SCALE);
  game->vx[entity] = (int16_t)lroundf(right.x * 4.0f + up.x * 5.0f - aim.x);
  game->vy[entity] = (int16_t)lroundf(right.y * 4.0f + up.y * 5.0f - aim.y);
  game->vz[entity] = (int16_t)lroundf(right.z * 4.0f + up.z * 5.0f - aim.z);
  game->yaw[entity] = wrap_degrees(game->yaw[player_entity] + 70);
  game->pitch[entity] = 30;
  game->held_by[entity] = (uint8_t)(entity * 37u);
  game->owner[entity] = game->owner[player_entity];
  game->kind[entity] = KIND_CASING;
  game->lifetime[entity] = CASING_LIFETIME_TICKS;
  game->previous_px[entity] = game->px[entity];
  game->previous_py[entity] = game->py[entity];
  game->previous_pz[entity] = game->pz[entity];
  ecs_add(&game->ecs, entity, COMP_POSITION);
  ecs_add(&game->ecs, entity, COMP_VELOCITY);
  ecs_add(&game->ecs, entity, COMP_LIFETIME);
  ecs_add(&game->ecs, entity, COMP_RENDERABLE);
  ecs_add(&game->ecs, entity, COMP_NETWORKED);
}

static void weapon_system(GameState *game) {
  uint32_t components[] = {COMP_POSITION, COMP_AIM, COMP_PLAYER};
  EcsQuery query = ecs_query(&game->ecs, components, 3);
  while (ecs_next(&query)) {
    uint32_t entity = query.entity;
    uint8_t player = game->owner[entity];
    if (game->cooldown[entity] > 0)
      --game->cooldown[entity];
    if ((game->input[player].buttons & BTN_FIRE) != 0 &&
        game->cooldown[entity] == 0) {
      if (spawn_bullet(game, entity) != ECS_INVALID_ENTITY) {
        game->cooldown[entity] = FIRE_COOLDOWN_TICKS;
        if (game->flash[entity] <= PLAYER_FIRE_FLASH_TICKS)
          game->flash[entity] = PLAYER_FIRE_FLASH_TICKS;
        spawn_casing(game, entity);
      }
    }
    if (game->flash[entity] > 0)
      --game->flash[entity];
  }
}

static uint32_t impact_mark_count(const GameState *game) {
  uint32_t components[] = {COMP_LIFETIME};
  EcsQuery query = ecs_query(&game->ecs, components, 1);
  uint32_t count = 0;
  while (ecs_next(&query))
    if (game->kind[query.entity] == KIND_IMPACT_MARK)
      ++count;
  return count;
}

static void make_impact_mark(GameState *game, uint32_t entity,
                             bool floor_impact) {
  if (impact_mark_count(game) >= MAX_IMPACT_MARKS) {
    uint32_t components[] = {COMP_LIFETIME};
    EcsQuery query = ecs_query(&game->ecs, components, 1);
    uint32_t oldest = ECS_INVALID_ENTITY;
    uint8_t shortest_lifetime = UINT8_MAX;
    while (ecs_next(&query)) {
      uint32_t mark = query.entity;
      if (game->kind[mark] == KIND_IMPACT_MARK &&
          game->lifetime[mark] < shortest_lifetime) {
        oldest = mark;
        shortest_lifetime = game->lifetime[mark];
      }
    }
    if (oldest != ECS_INVALID_ENTITY)
      ecs_destroy(&game->ecs, oldest);
  }
  if (floor_impact) {
    game->py[entity] = 1;
  } else {
    game->px[entity] = (int16_t)(game->px[entity] - game->vx[entity]);
    game->py[entity] = (int16_t)(game->py[entity] - game->vy[entity]);
    game->pz[entity] = (int16_t)(game->pz[entity] - game->vz[entity]);
  }
  game->kind[entity] = KIND_IMPACT_MARK;
  game->lifetime[entity] = IMPACT_MARK_LIFETIME_TICKS;
  game->flash[entity] = 3;
  ecs_remove(&game->ecs, entity, COMP_PROJECTILE);
  ecs_remove(&game->ecs, entity, COMP_VELOCITY);
}

static void casing_system(GameState *game) {
  uint32_t components[] = {COMP_POSITION, COMP_VELOCITY, COMP_LIFETIME};
  EcsQuery query = ecs_query(&game->ecs, components, 3);
  while (ecs_next(&query)) {
    uint32_t entity = query.entity;
    if (game->kind[entity] != KIND_CASING)
      continue;
    game->px[entity] = (int16_t)(game->px[entity] + game->vx[entity]);
    game->py[entity] = (int16_t)(game->py[entity] + game->vy[entity]);
    game->pz[entity] = (int16_t)(game->pz[entity] + game->vz[entity]);
    if (game->tick % 3u == 0 && game->vy[entity] > -8)
      --game->vy[entity];
    if (game->py[entity] <= 3) {
      game->py[entity] = 3;
      if (game->vy[entity] < -1)
        game->vy[entity] = (int16_t)(-game->vy[entity] / 3);
      else
        game->vy[entity] = 0;
      game->vx[entity] = (int16_t)(game->vx[entity] * 3 / 4);
      game->vz[entity] = (int16_t)(game->vz[entity] * 3 / 4);
    }
    if (game->vx[entity] != 0 || game->vy[entity] != 0 ||
        game->vz[entity] != 0) {
      game->yaw[entity] = wrap_degrees(game->yaw[entity] + 13);
      game->pitch[entity] = wrap_degrees(game->pitch[entity] + 17);
      game->held_by[entity] = (uint8_t)(game->held_by[entity] + 19u);
    }
  }
}

static void projectile_system(GameState *game) {
  uint32_t components[] = {COMP_POSITION, COMP_VELOCITY, COMP_PROJECTILE,
                           COMP_LIFETIME};
  EcsQuery query = ecs_query(&game->ecs, components, 4);
  while (ecs_next(&query)) {
    uint32_t entity = query.entity;
    game->px[entity] = (int16_t)(game->px[entity] + game->vx[entity]);
    game->py[entity] = (int16_t)(game->py[entity] + game->vy[entity]);
    game->pz[entity] = (int16_t)(game->pz[entity] + game->vz[entity]);
    bool floor_impact = game->py[entity] < 0;
    bool leave_mark = floor_impact;
    bool hit = game->px[entity] < -PROJECTILE_LIMIT * POS_SCALE ||
               game->px[entity] > PROJECTILE_LIMIT * POS_SCALE ||
               game->pz[entity] < -PROJECTILE_LIMIT * POS_SCALE ||
               game->pz[entity] > PROJECTILE_LIMIT * POS_SCALE ||
               floor_impact ||
               game->py[entity] > PROJECTILE_LIMIT * POS_SCALE;
    uint32_t cubes[] = {COMP_POSITION, COMP_RENDERABLE};
    EcsQuery cube_query = ecs_query(&game->ecs, cubes, 2);
    while (!hit && ecs_next(&cube_query)) {
      uint32_t cube = cube_query.entity;
      if (game->kind[cube] == KIND_CUBE && game->held_by[cube] == 0 &&
          abs((int)game->px[entity] - game->px[cube]) < 42 &&
          abs((int)game->py[entity] - game->py[cube]) < 48 &&
          abs((int)game->pz[entity] - game->pz[cube]) < 42) {
        hit = true;
        leave_mark = true;
      }
    }
    uint32_t players[] = {COMP_POSITION, COMP_PLAYER};
    EcsQuery player_query = ecs_query(&game->ecs, players, 2);
    while (!hit && ecs_next(&player_query)) {
      uint32_t player = player_query.entity;
      if (game->owner[player] != game->owner[entity] &&
          abs((int)game->px[entity] - game->px[player]) < 32 &&
          game->py[entity] < 128 && game->py[entity] > 0 &&
          abs((int)game->pz[entity] - game->pz[player]) < 32) {
        game->flash[player] = PLAYER_HIT_FLASH_TICKS;
        hit = true;
      }
    }
    if (hit) {
      if (leave_mark)
        make_impact_mark(game, entity, floor_impact);
      else
        ecs_destroy(&game->ecs, entity);
    }
  }
}

static void lifetime_system(GameState *game) {
  uint32_t components[] = {COMP_POSITION, COMP_LIFETIME};
  EcsQuery query = ecs_query(&game->ecs, components, 2);
  while (ecs_next(&query)) {
    uint32_t entity = query.entity;
    bool age = game->kind[entity] != KIND_IMPACT_MARK || game->tick % 2u == 0;
    if (age && game->lifetime[entity] > 0)
      --game->lifetime[entity];
    if (game->kind[entity] == KIND_IMPACT_MARK) {
      if (game->lifetime[entity] > 166)
        game->flash[entity] = 3;
      else if (game->lifetime[entity] > 83)
        game->flash[entity] = 2;
      else
        game->flash[entity] = 1;
    }
    if (game->lifetime[entity] == 0)
      ecs_destroy(&game->ecs, entity);
  }
}

static void server_tick(GameState *game) {
  aim_system(game);
  movement_system(game);
  holding_system(game);
  weapon_system(game);
  casing_system(game);
  projectile_system(game);
  lifetime_system(game);
  ++game->tick;
}

static void write_snapshot(GameState *game, uint8_t recipient, uint16_t ack,
                           uint32_t session) {
  NetSnapshot snapshot;
  uint32_t components[] = {COMP_POSITION, COMP_NETWORKED, COMP_RENDERABLE};
  EcsQuery query;
  memset(&snapshot, 0, sizeof(snapshot));
  snapshot.magic = NET_MAGIC;
  snapshot.type = NET_SNAPSHOT;
  snapshot.local_player = recipient;
  snapshot.session = session_wire(session);
  snapshot.tick = sequence_wire(game->tick);
  snapshot.acknowledged_sequence = sequence_wire(ack);
  for (uint32_t player = 0; player < MAX_PLAYERS; ++player)
    memcpy(snapshot.names[player], game->player_name[player], PLAYER_NAME_LENGTH);
  query = ecs_query(&game->ecs, components, 3);
  while (snapshot.count < NET_ENTITY_LIMIT && ecs_next(&query)) {
    uint32_t entity = query.entity;
    NetEntity *wire = &snapshot.entities[snapshot.count++];
    wire->entity = (uint8_t)entity;
    wire->kind = game->kind[entity];
    wire->owner = game->owner[entity];
    wire->flags = game->flash[entity];
    if (wire->kind == KIND_CUBE || wire->kind == KIND_CASING)
      wire->flags = game->held_by[entity];
    wire->x = host_i16(game->px[entity]);
    wire->y = host_i16(game->py[entity]);
    wire->z = host_i16(game->pz[entity]);
    wire->yaw = host_i16(game->yaw[entity]);
    wire->pitch = host_i16(game->pitch[entity]);
  }
  memcpy(game->net_tx, &snapshot, sizeof(snapshot));
}

static bool snapshot_size_valid(size_t size, const NetSnapshot *snapshot) {
  return size == sizeof(NetSnapshot) && snapshot->count <= NET_ENTITY_LIMIT;
}

static void send_peer_snapshot(GameState *game, int fd, const Peer *peer) {
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = peer->address;
  address.sin_port = peer->port;
  (void)sendto(fd, game->net_tx, sizeof(NetSnapshot), 0,
               (const struct sockaddr *)&address, sizeof(address));
}

static int find_peer_session(const GameState *game, uint32_t session) {
  for (int i = 0; i < MAX_PLAYERS; ++i)
    if (game->peers[i].active && game->peers[i].session == session)
      return i;
  return -1;
}

static bool session_was_retired(const GameState *game, uint32_t session) {
  for (int i = 0; i < MAX_PLAYERS; ++i)
    if ((!game->peers[i].active && game->peers[i].session == session) ||
        game->peers[i].retired_session == session)
      return true;
  return false;
}

static int peer_with_name(const GameState *game,
                          const char name[PLAYER_NAME_LENGTH]) {
  for (int i = 0; i < MAX_PLAYERS; ++i)
    if (memcmp(game->player_name[i], name, PLAYER_NAME_LENGTH) == 0)
      return i;
  return -1;
}

static void remove_peer(GameState *game, int peer_index);

static int add_peer(GameState *game, const struct sockaddr_in *from,
                    uint32_t session, const char name[PLAYER_NAME_LENGTH]) {
  int slot = peer_with_name(game, name);
  if (slot < 0) {
    for (int i = 0; i < MAX_PLAYERS; ++i)
      if (!game->peers[i].active && game->player_entity[i] == NET_NONE) {
        slot = i;
        break;
      }
  }
  if (slot < 0)
    return -1;
  if (game->peers[slot].active)
    remove_peer(game, slot);
  uint32_t entity = create_player(game, (uint8_t)slot);
  if (entity == ECS_INVALID_ENTITY)
    return -1;
  Peer *peer = &game->peers[slot];
  if (peer->session != session)
    peer->retired_session = peer->session;
  peer->session = session;
  peer->address = from->sin_addr.s_addr;
  peer->port = from->sin_port;
  peer->last_sequence = 0;
  peer->last_chat_sequence = 0;
  peer->last_seen = game->tick;
  peer->active = 1;
  peer->got_sequence = 0;
  peer->got_chat_sequence = 0;
  peer->player = (uint8_t)slot;
  memset(&game->input[slot], 0, sizeof(game->input[slot]));
  game->input[slot].aim_yaw = game->yaw[entity];
  game->input[slot].aim_pitch = game->pitch[entity];
  memcpy(game->player_name[slot], name, PLAYER_NAME_LENGTH);
  game->player_name[slot][PLAYER_NAME_LENGTH] = '\0';
  return slot;
}

static void remove_peer(GameState *game, int peer_index) {
  Peer *peer = &game->peers[peer_index];
  uint8_t player = peer->player;
  uint32_t entity = game->player_entity[player];
  if (entity != NET_NONE && ecs_alive(&game->ecs, entity)) {
    for (uint32_t c = 0; c < ECS_MAX_COMPONENTS; ++c)
      ecs_remove(&game->ecs, entity, c);
    ecs_destroy(&game->ecs, entity);
  }
  for (uint32_t i = 0; i < ECS_MAX_ENTITIES; ++i)
    if (game->held_by[i] == player + 1u)
      game->held_by[i] = 0;
  game->player_entity[player] = NET_NONE;
  memset(&game->input[player], 0, sizeof(game->input[player]));
  peer->active = 0;
  peer->got_sequence = 0;
  peer->got_chat_sequence = 0;
}

static void server_relay_chat(GameState *game, int fd, int sender,
                              NetChatPacket *packet) {
  packet->player = game->peers[sender].player;
  packet->sequence = sequence_wire(game->next_chat_sequence++);
  memcpy(packet->name, game->player_name[packet->player], PLAYER_NAME_LENGTH);
  for (int i = 0; i < MAX_PLAYERS; ++i) {
    if (!game->peers[i].active)
      continue;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = game->peers[i].address;
    address.sin_port = game->peers[i].port;
    packet->session = session_wire(game->peers[i].session);
    (void)sendto(fd, packet, sizeof(*packet), 0,
                 (const struct sockaddr *)&address, sizeof(address));
  }
}

static void server_receive(GameState *game, int fd) {
  for (;;) {
    struct sockaddr_in from;
    socklen_t from_size = sizeof(from);
    ssize_t received = recvfrom(fd, game->net_rx, sizeof(game->net_rx), 0,
                                (struct sockaddr *)&from, &from_size);
    if (received < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      return;
    }
    if (received < 2 || game->net_rx[0] != NET_MAGIC)
      continue;

    if (game->net_rx[1] == NET_CHAT &&
        (size_t)received == sizeof(NetChatPacket)) {
      NetChatPacket packet;
      memcpy(&packet, game->net_rx, sizeof(packet));
      uint32_t session = session_host(packet.session);
      int peer_index = find_peer_session(game, session);
      if (peer_index < 0 || packet.length > CHAT_LENGTH)
        continue;
      Peer *peer = &game->peers[peer_index];
      uint16_t sequence = sequence_host(packet.sequence);
      if (peer->got_chat_sequence &&
          !sequence_newer(sequence, peer->last_chat_sequence))
        continue;
      peer->got_chat_sequence = 1;
      peer->last_chat_sequence = sequence;
      peer->last_seen = game->tick;
      peer->address = from.sin_addr.s_addr;
      peer->port = from.sin_port;
      for (uint32_t i = 0; i < packet.length; ++i)
        if ((packet.text[i] < 'A' || packet.text[i] > 'Z') &&
            (packet.text[i] < '0' || packet.text[i] > '9') &&
            packet.text[i] != ' ')
          packet.text[i] = ' ';
      server_relay_chat(game, fd, peer_index, &packet);
      continue;
    }

    if ((game->net_rx[1] != NET_INPUT &&
         game->net_rx[1] != NET_DISCONNECT) ||
        (size_t)received != sizeof(NetInputPacket))
      continue;
    NetInputPacket packet;
    memcpy(&packet, game->net_rx, sizeof(packet));
    uint32_t session = session_host(packet.session);
    if (session == 0 || !valid_wire_name(packet.name))
      continue;
    int peer_index = find_peer_session(game, session);
    if (game->net_rx[1] == NET_DISCONNECT) {
      if (peer_index >= 0 &&
          memcmp(game->player_name[peer_index], packet.name,
                 PLAYER_NAME_LENGTH) == 0)
        remove_peer(game, peer_index);
      continue;
    }
    if (peer_index < 0) {
      if (packet.claimed_player != NET_NONE ||
          session_was_retired(game, session))
        continue;
      peer_index = add_peer(game, &from, session, packet.name);
      if (peer_index < 0)
        continue;
    }
    Peer *peer = &game->peers[peer_index];
    if (memcmp(game->player_name[peer->player], packet.name,
               PLAYER_NAME_LENGTH) != 0 ||
        (packet.claimed_player != NET_NONE &&
         packet.claimed_player != peer->player))
      continue;
    uint16_t sequence = sequence_host(packet.sequence);
    if (peer->got_sequence && !sequence_newer(sequence, peer->last_sequence))
      continue;
    peer->got_sequence = 1;
    peer->last_sequence = sequence;
    peer->last_seen = game->tick;
    peer->address = from.sin_addr.s_addr;
    peer->port = from.sin_port;
    game->input[peer->player].move_x = clamp_i16(packet.move_x, -1, 1);
    game->input[peer->player].move_z = clamp_i16(packet.move_z, -1, 1);
    game->input[peer->player].aim_yaw =
        wrap_degrees(wire_i16(packet.aim_yaw));
    game->input[peer->player].aim_pitch =
        clamp_i16(wire_i16(packet.aim_pitch), -75, 75);
    game->input[peer->player].buttons =
        packet.buttons & (BTN_FIRE | BTN_GRAB | BTN_JUMP);
  }
}

static void server_expire_peers(GameState *game) {
  for (int i = 0; i < MAX_PLAYERS; ++i)
    if (game->peers[i].active &&
        tick_age(game->tick, game->peers[i].last_seen) > SIM_HZ * 10)
      remove_peer(game, i);
}

static void server_send_all(GameState *game, int fd) {
  for (int i = 0; i < MAX_PLAYERS; ++i) {
    if (!game->peers[i].active)
      continue;
    uint16_t ack = game->peers[i].got_sequence ? game->peers[i].last_sequence : 0;
    write_snapshot(game, game->peers[i].player, ack,
                   game->peers[i].session);
    send_peer_snapshot(game, fd, &game->peers[i]);
  }
}

static void server_run(uint16_t port) {
  GameState game;
  int fd = open_udp_server(port);
  double current_time = hires_time_in_seconds();
  double accumulator = 0.0;
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
    double new_time = hires_time_in_seconds();
    double frame_time = new_time - current_time;
    if (frame_time > 0.25)
      frame_time = 0.25;
    current_time = new_time;
    accumulator += frame_time;
    server_receive(&game, fd);
    while (accumulator >= SIM_DT) {
      server_tick(&game);
      server_expire_peers(&game);
      if (game.tick % SNAPSHOT_INTERVAL == 0)
        server_send_all(&game, fd);
      accumulator -= SIM_DT;
    }
    (void)nanosleep(&idle, NULL);
  }
  close(fd);
}

static void show_notice(GameState *game, const char *text) {
  snprintf(game->notice, sizeof(game->notice), "%.*s", NOTICE_LENGTH, text);
  game->notice_ticks = SIM_HZ * 5;
}

static void show_player_notice(GameState *game, const char *name,
                               const char *event) {
  snprintf(game->notice, sizeof(game->notice), "%.*s %s", PLAYER_NAME_LENGTH,
           name, event);
  game->notice_ticks = SIM_HZ * 5;
}

static void show_chat_notice(GameState *game,
                             const char name[PLAYER_NAME_LENGTH],
                             const char *text, uint8_t length) {
  char message[CHAT_LENGTH + 1];
  if (length > CHAT_LENGTH)
    length = CHAT_LENGTH;
  memcpy(message, text, length);
  message[length] = '\0';
  snprintf(game->notice, sizeof(game->notice), "%.*s: %.15s",
           PLAYER_NAME_LENGTH, name, message);
  game->notice_ticks = SIM_HZ * 5;
}

static void renew_client_session(GameState *game) {
  game->session = make_session_id(game->session);
  game->connected = 0;
  game->local_player = NET_NONE;
  game->visible_players = 0;
  game->got_snapshot = 0;
  game->got_chat_sequence = 0;
  game->next_sequence = 0;
  game->next_chat_sequence = 0;
  game->last_snapshot_tick = 0;
  game->last_chat_sequence = 0;
  game->snapshot_silence_ticks = 0;
}

static void reset_client_entity(GameState *game, uint32_t entity) {
  for (uint32_t component = 0; component < ECS_MAX_COMPONENTS; ++component)
    ecs_remove(&game->ecs, entity, component);
  clear_entity_data(game, entity);
}

static uint32_t client_entity_for(GameState *game, uint8_t server_entity,
                                  uint8_t kind) {
  uint8_t local = game->network_entity[server_entity];
  if (local != NET_NONE && ecs_alive(&game->ecs, local) &&
      ecs_has(&game->ecs, local, COMP_NETWORKED) && game->kind[local] == kind)
    return local;
  if (local != NET_NONE && ecs_alive(&game->ecs, local))
    ecs_destroy(&game->ecs, local);
  uint32_t created = ecs_create(&game->ecs);
  if (created == ECS_INVALID_ENTITY)
    return created;
  game->network_entity[server_entity] = (uint8_t)created;
  reset_client_entity(game, created);
  return created;
}

static void apply_snapshot(GameState *game, const NetSnapshot *snapshot) {
  size_t i;
  uint8_t player_mask = 0;
  bool was_connected = game->connected != 0;
  for (i = 0; i < snapshot->count; ++i)
    if (snapshot->entities[i].kind == KIND_PLAYER &&
        snapshot->entities[i].owner < MAX_PLAYERS)
      player_mask |= (uint8_t)(1u << snapshot->entities[i].owner);
  if (!was_connected) {
    show_notice(game, "LINK");
  } else {
    uint8_t left = (uint8_t)(game->visible_players & ~player_mask);
    uint8_t joined = (uint8_t)(player_mask & ~game->visible_players);
    for (uint32_t player = 0; player < MAX_PLAYERS; ++player) {
      uint8_t bit = (uint8_t)(1u << player);
      if ((left & bit) != 0)
        show_player_notice(game, game->player_name[player], "LEFT");
      if ((joined & bit) != 0)
        show_player_notice(game, snapshot->names[player], "JOINED");
    }
  }
  for (uint32_t player = 0; player < MAX_PLAYERS; ++player) {
    memcpy(game->player_name[player], snapshot->names[player],
           PLAYER_NAME_LENGTH);
    game->player_name[player][PLAYER_NAME_LENGTH] = '\0';
  }
  game->visible_players = player_mask;
  game->snapshot_seen = 0;
  game->local_player = snapshot->local_player;
  game->last_snapshot_tick = sequence_host(snapshot->tick);
  game->connected = 1;
  game->snapshot_silence_ticks = 0;
  for (i = 0; i < snapshot->count; ++i) {
    const NetEntity *wire = &snapshot->entities[i];
    uint32_t entity;
    bool had_entity;
    if (wire->entity >= ECS_MAX_ENTITIES || wire->kind == 0 ||
        wire->kind > KIND_CASING)
      continue;
    game->snapshot_seen |= UINT32_C(1) << wire->entity;
    uint8_t mapped = game->network_entity[wire->entity];
    had_entity = mapped != NET_NONE && ecs_alive(&game->ecs, mapped) &&
                 game->kind[mapped] == wire->kind;
    entity = client_entity_for(game, wire->entity, wire->kind);
    if (entity == ECS_INVALID_ENTITY)
      continue;
    if (had_entity) {
      game->previous_px[entity] = game->px[entity];
      game->previous_py[entity] = game->py[entity];
      game->previous_pz[entity] = game->pz[entity];
    }
    game->kind[entity] = wire->kind;
    game->owner[entity] = wire->owner;
    game->px[entity] = wire_i16(wire->x);
    game->py[entity] = wire_i16(wire->y);
    game->pz[entity] = wire_i16(wire->z);
    game->yaw[entity] = wire_i16(wire->yaw);
    game->pitch[entity] = wire_i16(wire->pitch);
    if (!had_entity) {
      game->previous_px[entity] = game->px[entity];
      game->previous_py[entity] = game->py[entity];
      game->previous_pz[entity] = game->pz[entity];
    }
    game->held_by[entity] = wire->flags;
    game->flash[entity] = wire->flags;
    if (wire->kind == KIND_CUBE || wire->kind == KIND_CASING)
      game->held_by[entity] = wire->flags;
    else if (wire->kind == KIND_PLAYER)
      game->flash[entity] = wire->flags;
    ecs_add(&game->ecs, entity, COMP_POSITION);
    ecs_add(&game->ecs, entity, COMP_RENDERABLE);
    ecs_add(&game->ecs, entity, COMP_NETWORKED);
    if (wire->kind == KIND_PLAYER)
      ecs_add(&game->ecs, entity, COMP_PLAYER);
    if (wire->kind == KIND_BULLET || wire->kind == KIND_IMPACT_MARK ||
        wire->kind == KIND_CASING)
      ecs_add(&game->ecs, entity, COMP_LIFETIME);
    if (wire->kind == KIND_BULLET)
      ecs_add(&game->ecs, entity, COMP_PROJECTILE);
  }
  game->interpolation_ticks = 0;
  for (uint32_t server_entity = 0; server_entity < ECS_MAX_ENTITIES;
       ++server_entity) {
    uint8_t local = game->network_entity[server_entity];
    if (local != NET_NONE &&
        (game->snapshot_seen & (UINT32_C(1) << server_entity)) == 0) {
      if (ecs_alive(&game->ecs, local))
        ecs_destroy(&game->ecs, local);
      game->network_entity[server_entity] = NET_NONE;
    }
  }
}

static void client_receive(GameState *game, int fd) {
  for (;;) {
    ssize_t received = recv(fd, game->net_rx, sizeof(game->net_rx), 0);
    if (received < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      if (game->connected)
        show_notice(game, "DISCONNECTED");
      renew_client_session(game);
      return;
    }
    if (received < 2 || (size_t)received > sizeof(NetSnapshot) ||
        game->net_rx[0] != NET_MAGIC)
      continue;
    if (game->net_rx[1] == NET_CHAT &&
        (size_t)received == sizeof(NetChatPacket)) {
      NetChatPacket packet;
      memcpy(&packet, game->net_rx, sizeof(packet));
      uint16_t sequence = sequence_host(packet.sequence);
      if (session_host(packet.session) != game->session ||
          packet.length > CHAT_LENGTH || !valid_wire_name(packet.name) ||
          (game->got_chat_sequence &&
           !sequence_newer(sequence, game->last_chat_sequence)))
        continue;
      game->got_chat_sequence = 1;
      game->last_chat_sequence = sequence;
      show_chat_notice(game, packet.name, packet.text, packet.length);
      continue;
    }
    if (game->net_rx[1] != NET_SNAPSHOT)
      continue;
    NetSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    memcpy(&snapshot, game->net_rx, (size_t)received);
    uint16_t tick = sequence_host(snapshot.tick);
    if (!snapshot_size_valid((size_t)received, &snapshot) ||
        session_host(snapshot.session) != game->session ||
        (game->got_snapshot &&
         !sequence_newer(tick, game->last_snapshot_tick)))
      continue;
    game->got_snapshot = 1;
    apply_snapshot(game, &snapshot);
  }
}

static void client_send(GameState *game, int fd, const struct sockaddr_in *server) {
  NetInputPacket packet;
  memset(&packet, 0, sizeof(packet));
  packet.magic = NET_MAGIC;
  packet.type = NET_INPUT;
  packet.claimed_player = game->local_player;
  packet.buttons = game->input[0].buttons & (BTN_FIRE | BTN_GRAB | BTN_JUMP);
  packet.session = session_wire(game->session);
  packet.sequence = sequence_wire(game->next_sequence++);
  packet.move_x = game->input[0].move_x;
  packet.move_z = game->input[0].move_z;
  packet.aim_yaw = host_i16(game->view_yaw);
  packet.aim_pitch = host_i16(game->view_pitch);
  memcpy(packet.name, game->requested_name, PLAYER_NAME_LENGTH);
  memcpy(game->net_tx, &packet, sizeof(packet));
  (void)sendto(fd, game->net_tx, sizeof(packet), 0,
               (const struct sockaddr *)server, sizeof(*server));
}

static void client_send_chat(GameState *game, int fd) {
  NetChatPacket packet;
  memset(&packet, 0, sizeof(packet));
  packet.magic = NET_MAGIC;
  packet.type = NET_CHAT;
  packet.player = game->local_player;
  packet.length = game->chat_length;
  packet.session = session_wire(game->session);
  packet.sequence = sequence_wire(game->next_chat_sequence++);
  memcpy(packet.name, game->requested_name, PLAYER_NAME_LENGTH);
  memcpy(packet.text, game->chat_input, game->chat_length);
  (void)send(fd, &packet, sizeof(packet), 0);
}

static void client_send_disconnect(const GameState *game, int fd) {
  NetInputPacket packet;
  memset(&packet, 0, sizeof(packet));
  packet.magic = NET_MAGIC;
  packet.type = NET_DISCONNECT;
  packet.claimed_player = game->local_player;
  packet.session = session_wire(game->session);
  memcpy(packet.name, game->requested_name, PLAYER_NAME_LENGTH);
  (void)send(fd, &packet, sizeof(packet), 0);
}

static void update_input(GameState *game, int fd) {
  Vector2 mouse;
  int mx = 0;
  game->look_delta_x = 0.0f;
  game->look_delta_y = 0.0f;
  int mz = 0;
  if (!game->chatting &&
      (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_T))) {
    game->chatting = 1;
    game->chat_length = 0;
    game->chat_input[0] = '\0';
    game->input[0].move_x = 0;
    game->input[0].move_z = 0;
    game->input[0].buttons = 0;
    while (GetCharPressed() != 0) {
    }
    (void)GetMouseDelta();
    return;
  }
  if (game->chatting) {
    int character;
    game->input[0].move_x = 0;
    game->input[0].move_z = 0;
    game->input[0].buttons = 0;
    if (IsKeyPressed(KEY_ESCAPE)) {
      game->chatting = 0;
      game->chat_length = 0;
      game->chat_input[0] = '\0';
    } else if (IsKeyPressed(KEY_ENTER)) {
      if (game->chat_length > 0)
        client_send_chat(game, fd);
      game->chatting = 0;
      game->chat_length = 0;
      game->chat_input[0] = '\0';
    } else {
      if (IsKeyPressed(KEY_BACKSPACE) && game->chat_length > 0)
        --game->chat_length;
      while ((character = GetCharPressed()) != 0) {
        if (character >= 'a' && character <= 'z')
          character -= 'a' - 'A';
        if (((character >= 'A' && character <= 'Z') ||
             (character >= '0' && character <= '9') || character == ' ') &&
            game->chat_length < CHAT_LENGTH)
          game->chat_input[game->chat_length++] = (char)character;
      }
      game->chat_input[game->chat_length] = '\0';
    }
    (void)GetMouseDelta();
    return;
  }
  if (IsKeyDown(KEY_A))
    --mx;
  if (IsKeyDown(KEY_D))
    ++mx;
  if (IsKeyDown(KEY_W))
    ++mz;
  if (IsKeyDown(KEY_S))
    --mz;
  game->input[0].move_x = (int8_t)mx;
  game->input[0].move_z = (int8_t)mz;
  game->input[0].buttons = 0;
  if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    game->input[0].buttons |= BTN_FIRE;
  if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsKeyDown(KEY_E))
    game->input[0].buttons |= BTN_GRAB;
  if (IsKeyDown(KEY_SPACE))
    game->input[0].buttons |= BTN_JUMP;
  if (IsKeyPressed(KEY_F1))
    game->help ^= 1u;
  if (IsKeyPressed(KEY_ESCAPE)) {
    game->mouse_captured ^= 1u;
    if (game->mouse_captured)
      DisableCursor();
    else
      EnableCursor();
  }
  if (!game->mouse_captured)
    return;
  mouse = GetMouseDelta();
  game->look_delta_x = mouse.x;
  game->look_delta_y = mouse.y;
  int delta_x = (int)lroundf(mouse.x);
  int delta_y = (int)lroundf(mouse.y);
  game->view_yaw = wrap_degrees((int)game->view_yaw - delta_x);
  game->view_pitch =
      clamp_i16((int)game->view_pitch - delta_y, -75, 75);
}

static uint32_t local_player_entity(const GameState *game) {
  uint32_t components[] = {COMP_PLAYER, COMP_POSITION};
  EcsQuery query = ecs_query(&game->ecs, components, 2);
  while (ecs_next(&query))
    if (game->owner[query.entity] == game->local_player)
      return query.entity;
  return ECS_INVALID_ENTITY;
}

static float interpolated_value(int16_t previous, int16_t current,
                                float alpha) {
  return ((float)previous + ((float)current - (float)previous) * alpha) /
         POS_SCALE;
}

static Camera3D make_camera(const GameState *game, float alpha) {
  Camera3D camera = {0};
  uint32_t entity = local_player_entity(game);
  Vector3 direction = {0.0f, 0.0f, 1.0f};
  camera.position = (Vector3){0.0f, 1.5f, 0.0f};
  if (entity != ECS_INVALID_ENTITY) {
    camera.position.x =
        interpolated_value(game->previous_px[entity], game->px[entity], alpha);
    camera.position.y = 1.5f +
                        interpolated_value(game->previous_py[entity],
                                           game->py[entity], alpha);
    camera.position.z =
        interpolated_value(game->previous_pz[entity], game->pz[entity], alpha);
    aim_vector_angles(game->view_yaw, game->view_pitch, &direction);
  }
  camera.target = (Vector3){camera.position.x + direction.x,
                             camera.position.y + direction.y,
                             camera.position.z + direction.z};
  camera.up = (Vector3){0.0f, 1.0f, 0.0f};
  camera.fovy = 72.0f;
  camera.projection = CAMERA_PERSPECTIVE;
  return camera;
}

static void draw_gun_model(Vector3 position, float yaw, float pitch,
                           float roll, float recoil, float slide_recoil,
                           float scale, bool firing) {
  rlPushMatrix();
  rlTranslatef(position.x, position.y, position.z);
  rlRotatef(yaw, 0.0f, 1.0f, 0.0f);
  rlRotatef(pitch, 1.0f, 0.0f, 0.0f);
  rlRotatef(roll, 0.0f, 0.0f, 1.0f);
  rlScalef(scale, scale, scale);
  Vector3 slide = {-0.32f, 0.0f, 0.30f - recoil - slide_recoil};
  Vector3 slide_size = {0.16f, 0.13f, 0.46f};
  Vector3 grip = {-0.32f, -0.15f, 0.18f - recoil};
  Vector3 grip_size = {0.13f, 0.26f, 0.15f};
  DrawCubeV(slide, slide_size, PAL_MID);
  DrawCubeWiresV(slide, slide_size, PAL_DARK);
  DrawCubeV(grip, grip_size, PAL_PURPLE);
  DrawCubeWiresV(grip, grip_size, PAL_DARK);
  if (firing) {
    Vector3 muzzle = {-0.32f, 0.0f, 0.56f - recoil};
    Vector3 flash_tip = {-0.32f, 0.0f, 0.72f - recoil};
    DrawCylinderEx(muzzle, flash_tip, 0.065f, 0.0f, 5, PAL_LIGHT);
  }
  rlPopMatrix();
}

static void draw_casing(const GameState *game, uint32_t entity,
                        Vector3 position) {
  float roll = (float)game->held_by[entity] * (360.0f / 256.0f);
  rlPushMatrix();
  rlTranslatef(position.x, position.y, position.z);
  rlRotatef((float)game->yaw[entity], 0.0f, 1.0f, 0.0f);
  rlRotatef((float)game->pitch[entity], 1.0f, 0.0f, 0.0f);
  rlRotatef(roll, 0.0f, 0.0f, 1.0f);
  DrawCylinderEx((Vector3){0.0f, -0.065f, 0.0f},
                 (Vector3){0.0f, 0.065f, 0.0f}, 0.028f, 0.024f, 6,
                 PAL_LIGHT);
  DrawCylinderEx((Vector3){0.0f, -0.071f, 0.0f},
                 (Vector3){0.0f, -0.061f, 0.0f}, 0.034f, 0.034f, 6,
                 PAL_MID);
  rlPopMatrix();
}

static void draw_world(GameState *game, float alpha) {
  uint32_t components[] = {COMP_POSITION, COMP_RENDERABLE};
  EcsQuery query = ecs_query(&game->ecs, components, 2);
  while (ecs_next(&query)) {
    uint32_t entity = query.entity;
    Vector3 position = {
        interpolated_value(game->previous_px[entity], game->px[entity], alpha),
        interpolated_value(game->previous_py[entity], game->py[entity], alpha),
        interpolated_value(game->previous_pz[entity], game->pz[entity], alpha)};
    if (game->kind[entity] == KIND_CUBE) {
      Color color = game->held_by[entity] ? PAL_LIGHT : PAL_MID;
      Vector3 size = {0.9f, 1.0f, 0.9f};
      DrawCubeV(position, size, color);
    } else if (game->kind[entity] == KIND_PLAYER) {
      if (game->owner[entity] != game->local_player) {
        bool hurt = game->flash[entity] > PLAYER_FIRE_FLASH_TICKS;
        Vector3 player_position = {position.x, position.y + 0.7f, position.z};
        Vector3 size = {0.55f, 1.4f, 0.55f};
        DrawCubeV(player_position, size, hurt ? PAL_LIGHT : PAL_MID);
        DrawCubeWiresV(player_position, size, BLACK);
        bool firing = game->flash[entity] > 0 &&
                      game->flash[entity] <= PLAYER_FIRE_FLASH_TICKS;
        float fire_phase = firing ? (float)game->flash[entity] /
                                        (float)PLAYER_FIRE_FLASH_TICKS
                                  : 0.0f;
        draw_gun_model((Vector3){position.x, position.y + 1.18f, position.z},
                       (float)game->yaw[entity], (float)-game->pitch[entity],
                       0.0f, fire_phase * 0.025f, fire_phase * 0.08f, 1.0f,
                       firing);
      }
    } else if (game->kind[entity] == KIND_BULLET) {
      Vector3 direction;
      aim_vector(game, entity, &direction);
      Vector3 shoulder = {position.x - direction.x * 0.20f,
                          position.y - direction.y * 0.20f,
                          position.z - direction.z * 0.20f};
      Vector3 base = {shoulder.x - direction.x * 0.34f,
                      shoulder.y - direction.y * 0.34f,
                      shoulder.z - direction.z * 0.34f};
      DrawCylinderEx(base, shoulder, 0.032f, 0.032f, 7, PAL_MID);
      DrawCylinderEx(shoulder, position, 0.032f, 0.0f, 7, PAL_LIGHT);
    } else if (game->kind[entity] == KIND_CASING) {
      draw_casing(game, entity, position);
    } else if (game->kind[entity] == KIND_IMPACT_MARK) {
      float size = game->flash[entity] >= 3 ? 0.18f
                   : game->flash[entity] == 2 ? 0.13f
                                              : 0.08f;
      Color color = game->flash[entity] >= 3 ? PAL_LIGHT
                    : game->flash[entity] == 2 ? PAL_MID
                                               : PAL_DARK;
      DrawCubeV(position, (Vector3){size, size, size}, color);
    }
  }
}

static int glyph_index(char character) {
  for (uint32_t i = 0; i < TOM_THUMB_3X5_COUNT; ++i)
    if (TOM_THUMB_3X5_CHARS[i] == character)
      return (int)i;
  return -1;
}

static void draw_small_text(int x, int y, const char *text, Color color) {
  for (int i = 0; text[i] != '\0'; ++i) {
    int index = glyph_index(text[i]);
    if (index >= 0)
      for (int row = 0; row < 5; ++row)
        for (int column = 0; column < 3; ++column)
          if ((tom_thumb_3x5[index] &
               (1u << ((4 - row) * 3 + 2 - column))) != 0)
            DrawRectangle(x + i * 4 + column, y + row, 1, 1, color);
  }
}

static void draw_number(int x, int y, unsigned value, Color color) {
  char digits[8];
  int count = 0;
  do {
    digits[count++] = (char)('0' + value % 10u);
    value /= 10u;
  } while (value != 0 && count < (int)sizeof(digits));
  for (int i = 0; i < count; ++i) {
    char text[2] = {digits[count - 1 - i], '\0'};
    draw_small_text(x + i * 4, y, text, color);
  }
}

static int small_text_width(const char *text) {
  size_t length = strlen(text);
  return length == 0 ? 0 : (int)length * 4 - 1;
}

static void draw_chat(const GameState *game) {
  if (!game->chatting)
    return;
  int width = 8 + small_text_width(game->chat_input);
  if (width < 7)
    width = 7;
  if (width > LOGICAL_W - 4)
    width = LOGICAL_W - 4;
  DrawRectangle(1, 1, width + 2, 7, PAL_DARK);
  draw_small_text(2, 2, "T:", PAL_MID);
  draw_small_text(10, 2, game->chat_input, PAL_LIGHT);
}

static void draw_hud(const GameState *game) {
  uint32_t entity = local_player_entity(game);
  unsigned fps = (unsigned)GetFPS();
  unsigned copy = fps;
  int digits = 1;
  while (copy >= 10u) {
    copy /= 10u;
    ++digits;
  }
  int fps_width = 15 + digits * 4;
  int fps_x = LOGICAL_W - fps_width - 2;
  if (!game->chatting && game->notice_ticks > 0 && game->notice[0] != '\0') {
    int width = small_text_width(game->notice);
    int y = width + 4 < fps_x ? 1 : 9;
    DrawRectangle(1, y, width + 2, 7, PAL_DARK);
    draw_small_text(2, y + 1, game->notice, PAL_LIGHT);
  }
  if (!game->chatting) {
    DrawRectangle(fps_x - 1, 1, fps_width + 2, 7, PAL_DARK);
    draw_small_text(fps_x, 2, "FPS", PAL_MID);
    draw_number(fps_x + 16, 2, fps, PAL_LIGHT);
  }
  if (entity != ECS_INVALID_ENTITY &&
      game->flash[entity] > PLAYER_FIRE_FLASH_TICKS) {
    Color hit_color = game->flash[entity] > PLAYER_HIT_FLASH_TICKS / 2
                          ? PAL_LIGHT
                          : PAL_MID;
    DrawRectangle(0, 0, LOGICAL_W, 1, hit_color);
    DrawRectangle(0, LOGICAL_H - 1, LOGICAL_W, 1, hit_color);
    DrawRectangle(0, 0, 1, LOGICAL_H, hit_color);
    DrawRectangle(LOGICAL_W - 1, 0, 1, LOGICAL_H, hit_color);
    DrawRectangle(40, 27, 3, 1, hit_color);
    DrawRectangle(49, 27, 3, 1, hit_color);
    DrawRectangle(42, 28, 1, 2, hit_color);
    DrawRectangle(49, 28, 1, 2, hit_color);
  }
  DrawRectangle(LOGICAL_W / 2, LOGICAL_H / 2, 1, 1, PAL_LIGHT);
  if (game->help) {
    DrawRectangle(1, 17, 42, 16, PAL_DARK);
    DrawRectangle(47, 17, 44, 16, PAL_DARK);
    draw_small_text(2, 19, "WASD MOVE", PAL_LIGHT);
    draw_small_text(2, 26, "MOUSE AIM", PAL_LIGHT);
    draw_small_text(48, 19, "SPACE JUMP", PAL_LIGHT);
    draw_small_text(48, 26, "E HOLD", PAL_LIGHT);
  }
  draw_chat(game);
}

static void update_viewmodel(GameState *game, float frame_time) {
  if (frame_time > 0.05f)
    frame_time = 0.05f;
  float target_x = game->look_delta_x * 0.010f;
  float target_y = -game->look_delta_y * 0.008f;
  if (target_x < -0.12f)
    target_x = -0.12f;
  if (target_x > 0.12f)
    target_x = 0.12f;
  if (target_y < -0.09f)
    target_y = -0.09f;
  if (target_y > 0.09f)
    target_y = 0.09f;
  float sway_blend = 1.0f - expf(-9.0f * frame_time);
  game->gun_sway_x += (target_x - game->gun_sway_x) * sway_blend;
  game->gun_sway_y += (target_y - game->gun_sway_y) * sway_blend;
  game->gun_slide *= expf(-22.0f * frame_time);
  game->gun_recoil_velocity +=
      (-75.0f * game->gun_recoil - 15.0f * game->gun_recoil_velocity) *
      frame_time;
  game->gun_recoil += game->gun_recoil_velocity * frame_time;
  if (game->gun_recoil < 0.0f) {
    game->gun_recoil = 0.0f;
    game->gun_recoil_velocity = 0.0f;
  }
  if (game->gun_recoil > 0.09f)
    game->gun_recoil = 0.09f;
}


static void render_frame(GameState *game, RenderTexture2D scene,
                         RenderTexture2D target, Shader outline, float alpha) {
  Camera3D camera = make_camera(game, alpha);
  BeginTextureMode(scene);
  ClearBackground(PAL_DARK);
  BeginMode3D(camera);
  DrawPlane((Vector3){0.0f, -0.02f, 0.0f}, (Vector2){32.0f, 32.0f}, PAL_PURPLE);
  draw_world(game, alpha);
  EndMode3D();
  Camera3D weapon_camera = {0};
  weapon_camera.position = (Vector3){0.0f, 0.0f, 0.0f};
  weapon_camera.target = (Vector3){0.0f, 0.0f, 1.0f};
  weapon_camera.up = (Vector3){0.0f, 1.0f, 0.0f};
  weapon_camera.fovy = 72.0f;
  weapon_camera.projection = CAMERA_PERSPECTIVE;
  BeginMode3D(weapon_camera);
  draw_gun_model(
      (Vector3){game->gun_sway_x * 0.45f,
                -0.18f + game->gun_sway_y * 0.45f, 0.75f},
      game->gun_sway_x * -20.0f, game->gun_sway_y * -16.0f,
      game->gun_sway_x * -12.0f, game->gun_recoil / 1.35f,
      game->gun_slide / 1.35f, 1.35f, game->gun_kick != 0);
  EndMode3D();
  EndTextureMode();

  BeginTextureMode(target);
  ClearBackground(PAL_DARK);
  BeginShaderMode(outline);
  DrawTextureRec(scene.texture,
                 (Rectangle){0.0f, 0.0f, (float)LOGICAL_W, -(float)LOGICAL_H},
                 (Vector2){0.0f, 0.0f}, WHITE);
  EndShaderMode();
  draw_hud(game);
  EndTextureMode();

  BeginDrawing();
  ClearBackground(PAL_DARK);
  int scale = GetScreenWidth() / LOGICAL_W;
  int height_scale = GetScreenHeight() / LOGICAL_H;
  if (height_scale < scale)
    scale = height_scale;
  if (scale < 1)
    scale = 1;
  int width = LOGICAL_W * scale;
  int height = LOGICAL_H * scale;
  Rectangle source = {0.0f, 0.0f, (float)LOGICAL_W, -(float)LOGICAL_H};
  int destination_x = (GetScreenWidth() - width) / 2;
  int destination_y = (GetScreenHeight() - height) / 2;
  Rectangle destination = {(float)destination_x, (float)destination_y,
                           (float)width, (float)height};
  DrawTexturePro(target.texture, source, destination, (Vector2){0, 0}, 0.0f,
                 WHITE);
  EndDrawing();
}

static int client_run(const char *host, uint16_t port, const char *name) {
  GameState game;
  struct sockaddr_in server;
  RenderTexture2D scene;
  RenderTexture2D target;
  Shader outline;
  int fd = open_udp_client(host, port, &server);
  double current_time = hires_time_in_seconds();
  double accumulator = 0.0;
  if (fd < 0) {
    fprintf(stderr, "host must be an IPv4 address or localhost\n");
    return 1;
  }
  game_init(&game);
  renew_client_session(&game);
  game.view_yaw = 180;
  memcpy(game.requested_name, name, PLAYER_NAME_LENGTH + 1);
  memcpy(game.player_name[0], name, PLAYER_NAME_LENGTH + 1);
  show_notice(&game, "CONNECTING");
  game.mouse_captured = 1;
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
  InitWindow(920, 640, "tiny ray ECS");
  SetWindowMinSize(LOGICAL_W, LOGICAL_H);
  DisableCursor();
  scene = LoadRenderTexture(LOGICAL_W, LOGICAL_H);
  target = LoadRenderTexture(LOGICAL_W, LOGICAL_H);
  outline = LoadShaderFromMemory(NULL, OUTLINE_FRAGMENT_SHADER);
  float texel_size[2] = {1.0f / LOGICAL_W, 1.0f / LOGICAL_H};
  int texel_location = GetShaderLocation(outline, "texelSize");
  SetShaderValue(outline, texel_location, texel_size, SHADER_UNIFORM_VEC2);
  SetTextureFilter(scene.texture, TEXTURE_FILTER_POINT);
  SetTextureFilter(target.texture, TEXTURE_FILTER_POINT);
  while (!WindowShouldClose()) {
    double new_time = hires_time_in_seconds();
    double frame_time = new_time - current_time;
    if (frame_time > 0.25)
      frame_time = 0.25;
    current_time = new_time;
    accumulator += frame_time;
    update_input(&game, fd);
    client_receive(&game, fd);
    while (accumulator >= SIM_DT) {
      if (++game.command_ticks >= COMMAND_INTERVAL) {
        client_send(&game, fd, &server);
        game.command_ticks = 0;
      }
      if (game.interpolation_ticks < SNAPSHOT_INTERVAL)
        ++game.interpolation_ticks;
      if (game.notice_ticks > 0)
        --game.notice_ticks;
      if (game.connected && ++game.snapshot_silence_ticks > SIM_HZ * 2) {
        renew_client_session(&game);
        show_notice(&game, "DISCONNECTED");
      }
      if (game.fire_cooldown > 0)
        --game.fire_cooldown;
      if (game.gun_kick > 0)
        --game.gun_kick;
      if ((game.input[0].buttons & BTN_FIRE) != 0 &&
          game.fire_cooldown == 0) {
        game.fire_cooldown = FIRE_COOLDOWN_TICKS;
        game.gun_kick = 4;
        game.gun_slide = 0.13f;
        game.gun_recoil_velocity += 1.4f;
      }
      accumulator -= SIM_DT;
    }
    client_receive(&game, fd);
    update_viewmodel(&game, (float)frame_time);
    float alpha =
        ((float)game.interpolation_ticks + (float)(accumulator / SIM_DT)) /
        ((float)SIM_HZ / (float)SNAPSHOT_HZ);
    if (alpha > 1.0f)
      alpha = 1.0f;
    render_frame(&game, scene, target, outline, alpha);
  }
  client_send_disconnect(&game, fd);
  EnableCursor();
  UnloadShader(outline);
  UnloadRenderTexture(scene);
  UnloadRenderTexture(target);
  CloseWindow();
  close(fd);
  return 0;
}

static void print_usage(const char *program) {
  fprintf(stderr, "usage: %s --server [port]\n", program);
  fprintf(stderr,
          "       %s --client <ipv4|localhost> <port> <four-letter-name>\n",
          program);
}

int main(int argc, char **argv) {
  uint16_t port;
  char name[PLAYER_NAME_LENGTH + 1];
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
