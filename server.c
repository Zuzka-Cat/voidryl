#ifndef SERVER_C
#define SERVER_C

#include "shared.c"

static volatile sig_atomic_t server_running = 1;

static void on_signal(int sig) {
  (void)sig;
  server_running = 0;
}

static int open_udp_server(u16 port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in address = {.sin_family = AF_INET,
                                .sin_addr.s_addr = htonl(INADDR_ANY),
                                .sin_port = htons(port)};
  int reuse = 1;
  if (fd < 0)
    return -1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
    close(fd);
    return -1;
  }
  socket_nonblocking(fd);
  return fd;
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
  muzzle = gun_point(g, pe, f, r, u, GUN_MUZZLE_OFFSET);
  target =
      gun_point(g, pe, f, r, u, (Vector3){BULLET_ZERO_DISTANCE, 0.0f, 0.0f});
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
  port =
      gun_point(g, pe, f, r, u, (Vector3){0.72f, GUN_MUZZLE_OFFSET.y, 0.03f});
  h = mix32(g->tick * 33u + e * 17u + (u32)g->owner[pe] * 91u + (u32)g->px[pe]);
  // Small jitter only: natural ejection pops slightly right/up/back and falls
  // nearby instead of flying far sideways.
  f32 jx = ((int)(h & 255u) - 128) / 320.0f;
  f32 jy = ((int)((h >> 8) & 255u) - 128) / 300.0f;
  f32 jz = ((int)((h >> 16) & 255u) - 128) / 320.0f;
  f32 side = 1.15f + jx;
  f32 up = 2.10f + jy;
  f32 back = 0.70f + jz * 0.25f;
  g->px[e] = (i16)lroundf(port.x * POS_SCALE);
  g->py[e] = (i16)lroundf(port.y * POS_SCALE);
  g->pz[e] = (i16)lroundf(port.z * POS_SCALE);
  // Note: gun_basis r points left, so eject right with -r for visibility.
  g->vx[e] = (i16)lroundf(-r.x * side + u.x * up - f.x * back);
  g->vy[e] = (i16)lroundf(-r.y * side + u.y * up - f.y * back);
  g->vz[e] = (i16)lroundf(-r.z * side + u.z * up - f.z * back);
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
    if (p >= MAX_PLAYERS)
      continue;
    bool want = (g->input[p].buttons & BTN_FIRE) != 0;
    bool holding = weapon_holding(g, e, want);
    weapon_tick(g, e);
    if (weapon_can_fire(g, e, want)) {
      if (spawn_bullet(g, e) != ECS_INVALID_ENTITY) {
        u8 wid = g->weapon_id[e];
        weapon_on_fired(g, e, holding);
        // Authoritative recoil: same pitch step the client predicts, so
        // bullets and snapshots stay in sync even before the echoed input
        // arrives. Persist it in the input so aim_system keeps it.
        g->pitch[e] = weapon_recoil_pitch(g->pitch[e], wid, holding);
        g->input[p].aim_pitch =
            weapon_recoil_pitch(g->input[p].aim_pitch, wid, holding);
        if (g->flash[e] <= PLAYER_FIRE_FLASH_TICKS)
          g->flash[e] = PLAYER_FIRE_FLASH_TICKS;
        spawn_casing(g, e);
      } else {
        weapon_on_blocked(g, e);
      }
    }
    weapon_set_prev(g, e, want);
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
    if (g->tick % 6u == 0 && g->vy[e] > -8)
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
  *snap = (NetSnapshot){.magic = NET_MAGIC,
                        .type = NET_PACKET_SNAPSHOT,
                        .local_player = recipient,
                        .session = session,
                        .tick = g->tick,
                        .acknowledged_sequence = ack};
  for (u32 p = 0; p < MAX_PLAYERS; ++p) {
    String name = game_player_name(g, p);
    memcpy(snap->names[p], name.items, PLAYER_NAME_LENGTH);
  }
  EcsQuery q = ecs_query(&g->ecs, comps, 3);
  while (snap->count < NET_ENTITY_LIMIT && ecs_next(&q)) {
    u32 e = q.entity;
    u8 weapon = WEAPON_P99, ammo = 0, reloading = 0;
    if (g->kind[e] == KIND_PLAYER) {
      weapon = g->weapon_id[e];
      ammo = g->weapon_ammo[e];
      reloading = (u8)(g->weapon_reload[e] != 0 ? 1 : 0);
    }
    snap->entities[snap->count++] =
        (NetEntity){.entity = (u8)e,
                    .kind = g->kind[e],
                    .owner = g->owner[e],
                    .flags = g->flash[e],
                    .extra = g->held_by[e],
                    .weapon = weapon,
                    .ammo = ammo,
                    .reloading = reloading,
                    .x = g->px[e],
                    .y = g->py[e],
                    .z = g->pz[e],
                    .yaw = g->yaw[e],
                    .pitch = g->pitch[e]};
  }
}

static void send_peer_snapshot(int fd, const Peer *peer,
                               const NetSnapshot *snap) {
  struct sockaddr_in address = {.sin_family = AF_INET,
                                .sin_addr.s_addr = peer->address,
                                .sin_port = peer->port};
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

static int peer_with_name(const GameState *g, String name) {
  for (int i = 0; i < MAX_PLAYERS; ++i) {
    String have = {(u8 *)g->player_name[i], PLAYER_NAME_LENGTH,
                   PLAYER_NAME_LENGTH};
    if (string_eq(have, name))
      return i;
  }
  return -1;
}

static void remove_peer(GameState *g, int peer_index);

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
  weapon_init_entity(g, e);
  add_kind_components(g, e, KIND_PLAYER);
  return e;
}

static int add_peer(GameState *g, const struct sockaddr_in *from, u32 session,
                    String name) {
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
  u32 retired_session = g->peers[slot].session != session
                            ? g->peers[slot].session
                            : g->peers[slot].retired_session;
  g->peers[slot] = (Peer){.session = session,
                          .retired_session = retired_session,
                          .address = from->sin_addr.s_addr,
                          .port = from->sin_port,
                          .last_seen = g->tick,
                          .active = 1,
                          .player = (u8)slot};
  g->input[slot] = (InputCommand){.aim_yaw = g->yaw[e]};
  for (i32 i = 0; i < PLAYER_NAME_LENGTH; ++i)
    g->player_name[slot][i] = name.items[i];
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
  g->input[player] = (InputCommand){0};
  peer->active = peer->got_sequence = peer->got_chat_sequence = 0;
}

static void server_relay_chat(GameState *g, int fd, int sender,
                              NetChatPacket *packet) {
  packet->player = g->peers[sender].player;
  packet->sequence = g->next_chat_sequence++;
  String src = game_player_name(g, packet->player);
  memcpy(packet->name, src.items, PLAYER_NAME_LENGTH);
  for (int i = 0; i < MAX_PLAYERS; ++i) {
    if (!g->peers[i].active)
      continue;
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_addr.s_addr = g->peers[i].address,
                                  .sin_port = g->peers[i].port};
    packet->session = g->peers[i].session;
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
    if (!net_magic_ok(buf, (i32)n))
      continue;
    if (net_packet_ok(buf, (i32)n, NET_PACKET_CHAT,
                      (i32)sizeof(NetChatPacket))) {
      NetChatPacket packet;
      memcpy(&packet, buf, sizeof(packet));
      int pi = find_peer_session(g, packet.session);
      if (pi < 0)
        continue;
      String wname = {(u8 *)packet.name, PLAYER_NAME_LENGTH,
                      PLAYER_NAME_LENGTH};
      Peer *peer = &g->peers[pi];
      if (!net_validate_chat_fields(wname, packet.length, packet.sequence,
                                    peer->got_chat_sequence != 0,
                                    peer->last_chat_sequence))
        continue;
      peer->got_chat_sequence = 1;
      peer->last_chat_sequence = packet.sequence;
      peer->last_seen = g->tick;
      peer->address = from.sin_addr.s_addr;
      peer->port = from.sin_port;
      String text = {packet.text, packet.length, CHAT_LENGTH};
      net_sanitize_chat_text(text);
      server_relay_chat(g, fd, pi, &packet);
      continue;
    }
    if (!net_packet_ok(buf, (i32)n, NET_PACKET_INPUT,
                       (i32)sizeof(NetInputPacket)) &&
        !net_packet_ok(buf, (i32)n, NET_PACKET_DISCONNECT,
                       (i32)sizeof(NetInputPacket)))
      continue;
    NetInputPacket packet;
    memcpy(&packet, buf, sizeof(packet));
    u32 session = packet.session;
    String wname = {(u8 *)packet.name, PLAYER_NAME_LENGTH, PLAYER_NAME_LENGTH};
    if (!net_validate_input_fields(session, wname))
      continue;
    int pi = find_peer_session(g, session);
    if (buf[1] == NET_PACKET_DISCONNECT) {
      if (pi >= 0) {
        String have = game_player_name(g, (u32)pi);
        if (string_eq(have, wname))
          remove_peer(g, pi);
      }
      continue;
    }
    if (pi < 0) {
      if (packet.claimed_player != NET_PACKET_NONE ||
          session_was_retired(g, session))
        continue;
      pi = add_peer(g, &from, session, wname);
      if (pi < 0)
        continue;
    }
    Peer *peer = &g->peers[pi];
    String have = game_player_name(g, peer->player);
    if (!string_eq(have, wname) || (packet.claimed_player != NET_PACKET_NONE &&
                                    packet.claimed_player != peer->player))
      continue;
    u16 seq = packet.sequence;
    if (!net_seq_fresh(seq, peer->got_sequence != 0, peer->last_sequence))
      continue;
    peer->got_sequence = 1;
    peer->last_sequence = seq;
    peer->last_seen = g->tick;
    peer->address = from.sin_addr.s_addr;
    peer->port = from.sin_port;
    g->input[peer->player].move_x = clamp_i16(packet.move_x, -1, 1);
    g->input[peer->player].move_z = clamp_i16(packet.move_z, -1, 1);
    g->input[peer->player].aim_yaw = wrap_degrees(packet.aim_yaw);
    g->input[peer->player].aim_pitch = clamp_i16(packet.aim_pitch, -75, 75);
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

#endif
