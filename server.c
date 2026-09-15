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
    if (g->owner[e] >= MAX_PLAYERS)
      continue;
    InputCommand *in = &g->input[g->owner[e]];
    f32 yaw = (f32)g->aim[e].yaw * DEG2RAD;
    f32 dx = (f32)in->move_z * sinf(yaw) - (f32)in->move_x * cosf(yaw);
    f32 dz = (f32)in->move_z * cosf(yaw) + (f32)in->move_x * sinf(yaw);
    f32 len = sqrtf(dx * dx + dz * dz);
    if (len > 1.0f) {
      dx /= len;
      dz /= len;
    }
    bool aiming = (in->buttons & BTN_AIM) != 0;
    bool sprinting = present_sprint_want(in->buttons, in->move_x, in->move_z);
    f32 speed = 3.0f;
    if (aiming)
      speed *= weapon_aim_move_scale(g->weapon_id[e]);
    else if (sprinting)
      speed = 4.5f;
    i16 step_x = (i16)lroundf(dx * speed);
    i16 step_z = (i16)lroundf(dz * speed);
    g->vel[e].x = step_x;
    g->vel[e].z = step_z;
    if ((in->buttons & BTN_JUMP) != 0 && g->pos[e].y == 0)
      g->vel[e].y = 6;
    if (g->pos[e].y > 0 || g->vel[e].y > 0) {
      g->pos[e].y = (i16)(g->pos[e].y + g->vel[e].y);
      if (g->tick % 6u == 0 && g->vel[e].y > -6)
        --g->vel[e].y;
      if (g->pos[e].y <= 0) {
        g->pos[e].y = 0;
        g->vel[e].y = 0;
      }
    }
    i16 nx = (i16)clamp_int((int)g->pos[e].x + step_x, -ARENA_POS, ARENA_POS);
    i16 nz = (i16)clamp_int((int)g->pos[e].z + step_z, -ARENA_POS, ARENA_POS);
    if (!cube_overlap(g, nx, g->pos[e].z, POS_SCALE / 4))
      g->pos[e].x = nx;
    if (!cube_overlap(g, g->pos[e].x, nz, POS_SCALE / 4))
      g->pos[e].z = nz;
  }
}

static void aim_system(GameState *g) {
  u32 comps[] = {COMP_AIM, COMP_PLAYER};
  EcsQuery q = ecs_query(&g->ecs, comps, 2);
  while (ecs_next(&q)) {
    u8 p = g->owner[q.entity];
    if (p >= MAX_PLAYERS)
      continue;
    g->aim[q.entity].yaw = wrap_degrees(g->input[p].aim_yaw);
    g->aim[q.entity].pitch = (i16)clamp_int(g->input[p].aim_pitch, -75, 75);
  }
}

static u32 held_cube_for(const GameState *g, u8 player) {
  u32 comps[] = {COMP_POSITION, COMP_SCALE, COMP_SOLID};
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
    if (player >= MAX_PLAYERS)
      continue;
    u32 held = held_cube_for(g, player);
    bool want = (g->input[player].buttons & BTN_GRAB) != 0;
    if (!want && held != ECS_INVALID_ENTITY) {
      g->held_by[held] = 0;
      held = ECS_INVALID_ENTITY;
    }
    if (want && held == ECS_INVALID_ENTITY) {
      Vector3 dir;
      u32 cubes[] = {COMP_POSITION, COMP_SCALE, COMP_SOLID};
      EcsQuery cq = ecs_query(&g->ecs, cubes, 3);
      f32 best_d = 1000.0f;
      u32 best = ECS_INVALID_ENTITY;
      aim_dir(g->aim[pe].yaw, g->aim[pe].pitch, &dir);
      while (ecs_next(&cq)) {
        u32 c = cq.entity;
        if (g->kind[c] != KIND_CUBE || g->held_by[c] != 0)
          continue;
        Vector3 off = {(f32)(g->pos[c].x - g->pos[pe].x) / POS_SCALE,
                       (f32)(g->pos[c].y - (g->pos[pe].y + 96)) / POS_SCALE,
                       (f32)(g->pos[c].z - g->pos[pe].z) / POS_SCALE};
        f32 d = vec3_len(off);
        f32 dot = d > 0.01f ? vec3_dot(off, dir) / d : -1.0f;
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
      f32 dist = hold_dist_for_pitch(g->aim[pe].pitch);
      f32 scale =
          clamp_float(dist / HOLD_BASE_DIST, CUBE_SCALE_MIN, CUBE_SCALE_MAX);
      g->scale[held] = scale;
      f32 yaw_rad = (f32)g->aim[pe].yaw * DEG2RAD;
      f32 hx = sinf(yaw_rad);
      f32 hz = cosf(yaw_rad);
      f32 half_h = 0.5f * CUBE_VIS_Y * scale * (f32)POS_SCALE;
      f32 half_xz = 0.5f * CUBE_VIS_X * scale * (f32)POS_SCALE;
      f32 limit_x = fmaxf((f32)ARENA_POS - half_xz, 0.0f);
      f32 limit_z = fmaxf((f32)ARENA_POS - half_xz, 0.0f);
      f32 tx = (f32)g->pos[pe].x + hx * dist * (f32)POS_SCALE;
      f32 tz = (f32)g->pos[pe].z + hz * dist * (f32)POS_SCALE;
      tx = clamp_float(tx, -limit_x, limit_x);
      tz = clamp_float(tz, -limit_z, limit_z);
      g->pos[held].x = (i16)lroundf(tx);
      g->pos[held].y = (i16)lroundf(half_h);
      g->pos[held].z = (i16)lroundf(tz);
    }
  }
}

static Vector3 apply_spread(Vector3 dir, Vector3 right, Vector3 up, f32 spread,
                            u32 h) {
  if (spread <= 0.001f)
    return dir;
  u32 h2 = mix32(h ^ 0x85ebca6bu);
  f32 u1 = (f32)(h & 0xFFFFu) / 32767.5f - 1.0f;
  f32 u2 = (f32)(h2 & 0xFFFFu) / 32767.5f - 1.0f;
  f32 s = spread * DEG2RAD;
  return vec3_norm(vec3_add(
      dir, vec3_mul(vec3_add(vec3_mul(right, u1), vec3_mul(up, u2)), s)));
}

static u32 spawn_bullet(GameState *g, u32 pe) {
  if (count_kind(g, KIND_BULLET) >= MAX_BULLETS)
    return ECS_INVALID_ENTITY;
  u32 e = take_entity(g);
  if (e == ECS_INVALID_ENTITY)
    return e;
  clear_entity_data(g, e);
  Vector3 feet = entity_pos_m(g, pe, 1.0f);
  f32 yaw = (f32)g->aim[pe].yaw;
  f32 pitch = (f32)g->aim[pe].pitch;
  Vector3 fwd, right, up;
  look_basis(yaw, pitch, &fwd, &right, &up);
  u8 owner = g->owner[pe];
  bool aiming = owner < MAX_PLAYERS && (g->input[owner].buttons & BTN_AIM) != 0;
  u32 h = mix32(g->tick * 2654435761u ^ (u32)pe * 40503u ^ (u32)owner * 65599u ^
                (u32)g->weapon_ammo[pe] * 97u ^ 0x9e3779b9u);
  Vector3 dir = apply_spread(fwd, right, up,
                             weapon_spread_deg(g->weapon_id[pe], aiming), h);
  Vector3 eye = player_eye_m(feet);
  Vector3 spawn = vec3_add(eye, vec3_mul(dir, SHOT_SPAWN_M));
  g->pos[e].x = (i16)lroundf(spawn.x * (f32)POS_SCALE);
  g->pos[e].y = (i16)lroundf(spawn.y * (f32)POS_SCALE);
  g->pos[e].z = (i16)lroundf(spawn.z * (f32)POS_SCALE);
  g->vel[e].x = (i16)lroundf(dir.x * BULLET_SPEED);
  g->vel[e].y = (i16)lroundf(dir.y * BULLET_SPEED);
  g->vel[e].z = (i16)lroundf(dir.z * BULLET_SPEED);
  g->aim[e].yaw = wrap_degrees((int)lroundf(atan2f(dir.x, dir.z) * RAD2DEG));
  g->aim[e].pitch = (i16)clamp_int(
      (int)lroundf(asinf(clamp_float(dir.y, -1.0f, 1.0f)) * RAD2DEG), -89, 89);
  g->owner[e] = g->owner[pe];
  g->kind[e] = KIND_BULLET;
  g->lifetime[e] = BULLET_LIFETIME_TICKS;
  add_kind_components(g, e, KIND_BULLET);
  return e;
}

#define CASING_INHERIT_XZ 1.0f
#define CASING_INHERIT_Y 0.3f

static void spawn_casing(GameState *g, u32 pe) {
  u32 h, e;
  if (count_kind(g, KIND_CASING) >= MAX_CASINGS)
    destroy_oldest(g, KIND_CASING);
  e = take_entity(g);
  if (e == ECS_INVALID_ENTITY)
    return;
  clear_entity_data(g, e);
  u8 wid = g->weapon_id[pe];
  if (wid >= WEAPON_COUNT)
    wid = WEAPON_P99;
  const ShellDef *sh = shell_def(wid);
  Vector3 feet = entity_pos_m(g, pe, 1.0f);
  GunPose gp = player_gun_pose(g, pe, feet, (f32)g->aim[pe].yaw,
                               (f32)g->aim[pe].pitch, 1.0f);
  Vector3 port = gun_port_m(&gp);
  h = mix32(g->tick * 33u + e * 17u + (u32)g->owner[pe] * 91u +
            (u32)g->pos[pe].x);
  f32 jx = ((f32)((int)(h & 255u) - 128) / 128.0f) * sh->jitter;
  f32 jy = ((f32)((int)((h >> 8) & 255u) - 128) / 128.0f) * sh->jitter;
  f32 jz = ((f32)((int)((h >> 16) & 255u) - 128) / 128.0f) * sh->jitter;
  f32 k = (f32)POS_SCALE * SIM_DT;
  f32 side = (sh->eject.x + jx) * k;
  f32 up = (sh->eject.y + jy) * k;
  f32 back = (sh->eject.z + jz * 0.5f) * k;
  g->pos[e].x = (i16)lroundf(port.x * (f32)POS_SCALE);
  g->pos[e].y = (i16)lroundf(port.y * (f32)POS_SCALE);
  g->pos[e].z = (i16)lroundf(port.z * (f32)POS_SCALE);
  g->vel[e].x =
      (i16)(lroundf(gp.right.x * side + gp.up.x * up - gp.fwd.x * back) +
            lroundf((f32)g->vel[pe].x * CASING_INHERIT_XZ));
  g->vel[e].y =
      (i16)(lroundf(gp.right.y * side + gp.up.y * up - gp.fwd.y * back) +
            lroundf((f32)g->vel[pe].y * CASING_INHERIT_Y));
  g->vel[e].z =
      (i16)(lroundf(gp.right.z * side + gp.up.z * up - gp.fwd.z * back) +
            lroundf((f32)g->vel[pe].z * CASING_INHERIT_XZ));
  g->aim[e].yaw = wrap_degrees(g->aim[pe].yaw + 50 + (int)(h % 41u));
  g->aim[e].pitch = (i16)(18 + (int)((h >> 5) % 29u));
  g->held_by[e] = (u8)h;
  g->owner[e] = g->owner[pe];
  g->weapon_id[e] = wid;
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
        g->aim[e].pitch = weapon_recoil_pitch(g->aim[e].pitch, wid, holding);
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

static void make_impact_mark(GameState *g, u32 e, i16 x, i16 y, i16 z,
                             u8 extra) {
  if (count_kind(g, KIND_IMPACT_MARK) >= MAX_IMPACT_MARKS)
    destroy_oldest(g, KIND_IMPACT_MARK);
  g->pos[e] = (IVec3){x, y, z};
  g->held_by[e] = extra;
  g->kind[e] = KIND_IMPACT_MARK;
  g->lifetime[e] = IMPACT_MARK_LIFETIME_TICKS;
  g->flash[e] = 3;
  g->vel[e] = (IVec3){0, 0, 0};
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
    g->pos[e].x = (i16)(g->pos[e].x + g->vel[e].x);
    g->pos[e].y = (i16)(g->pos[e].y + g->vel[e].y);
    g->pos[e].z = (i16)(g->pos[e].z + g->vel[e].z);
    if (g->tick % 16u == 0 && g->vel[e].y > -8)
      --g->vel[e].y;
    if (g->pos[e].y <= 2) {
      g->pos[e].y = 2;
      if (g->vel[e].y <= -2) {
        g->vel[e].y = (i16)(-g->vel[e].y * 3 / 8);
        g->vel[e].x = (i16)(g->vel[e].x * 3 / 4);
        g->vel[e].z = (i16)(g->vel[e].z * 3 / 4);
      } else {
        g->vel[e].y = 0;
        g->vel[e].x = (i16)(g->vel[e].x * 15 / 16);
        g->vel[e].z = (i16)(g->vel[e].z * 15 / 16);
        if (abs(g->vel[e].x) < 1)
          g->vel[e].x = 0;
        if (abs(g->vel[e].z) < 1)
          g->vel[e].z = 0;
      }
    }
    if (g->vel[e].x || g->vel[e].y || g->vel[e].z) {
      bool airborne = g->pos[e].y > 2 || g->vel[e].y != 0;
      if (airborne) {
        g->aim[e].yaw = wrap_degrees(g->aim[e].yaw + 9 + (g->held_by[e] & 7));
        g->aim[e].pitch =
            wrap_degrees(g->aim[e].pitch + 11 + ((g->held_by[e] >> 3) & 7));
        g->held_by[e] = (u8)(g->held_by[e] + 13u);
      } else {
        g->aim[e].yaw = wrap_degrees(g->aim[e].yaw + 2 + (g->held_by[e] & 3));
        g->aim[e].pitch =
            wrap_degrees(g->aim[e].pitch + 2 + ((g->held_by[e] >> 2) & 3));
        g->held_by[e] = (u8)(g->held_by[e] + 5u);
      }
    }
  }
}

static void mark_hit_dir(GameState *g, u32 victim, u32 from) {
  int deg = (int)lroundf(atan2f((f32)(g->pos[from].x - g->pos[victim].x),
                                (f32)(g->pos[from].z - g->pos[victim].z)) *
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
    Vector3 dir = sight_dir((f32)g->aim[e].yaw, (f32)g->aim[e].pitch);
    Vector3 p0 = {(f32)g->pos[e].x, (f32)g->pos[e].y, (f32)g->pos[e].z};
    Vector3 d = vec3_mul(dir, BULLET_SPEED);
    TraceHit h = trace_solids(g, p0, d, e, g->owner[e], 1.0f);
    if (!h.hit) {
      g->pos[e].x = (i16)lroundf(p0.x + d.x);
      g->pos[e].y = (i16)lroundf(p0.y + d.y);
      g->pos[e].z = (i16)lroundf(p0.z + d.z);
      if (g->pos[e].y > bound || g->pos[e].x < -bound || g->pos[e].x > bound ||
          g->pos[e].z < -bound || g->pos[e].z > bound)
        ecs_destroy(&g->ecs, e);
      continue;
    }
    if (h.entity != ECS_INVALID_ENTITY && g->kind[h.entity] == KIND_PLAYER) {
      u32 src = e;
      u8 ow = g->owner[e];
      if (ow < MAX_PLAYERS && g->player_entity[ow] != NET_PACKET_NONE &&
          ecs_alive(&g->ecs, g->player_entity[ow]))
        src = g->player_entity[ow];
      mark_hit_dir(g, h.entity, src);
    }
    Vector3 hit = vec3_add(p0, vec3_mul(d, h.t));
    hit = vec3_add(hit, vec3_mul(h.n, MARK_NUDGE));
    f32 spd = vec3_len(d);
    f32 frac = spd > 0.001f ? fabsf(vec3_dot(d, h.n)) / spd : 0.0f;
    u8 str = (u8)(frac * (float)MARK_STR_LEVELS);
    if (str >= MARK_STR_LEVELS)
      str = MARK_STR_LEVELS - 1u;
    u8 skind = g->kind[h.entity];
    u8 mat = skind < sizeof(SOLID_MAT) ? SOLID_MAT[skind] : MARK_MAT_PURPLE;
    make_impact_mark(g, e, (i16)lroundf(hit.x), (i16)lroundf(hit.y),
                     (i16)lroundf(hit.z),
                     mark_extra_pack(mark_code_for_vec(h.n), str, mat));
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
    u8 reload_q = 0;
    u8 scale = 0;
    if (g->kind[e] == KIND_CUBE) {
      scale = scale_encode(scale_value(g, e));
    } else if (g->kind[e] == KIND_CASING) {
      weapon = g->weapon_id[e] < WEAPON_COUNT ? g->weapon_id[e] : WEAPON_P99;
    } else if (g->kind[e] == KIND_PLAYER) {
      weapon = g->weapon_id[e];
      ammo = g->weapon_ammo[e];
      reloading = (u8)(g->weapon_reload[e] != 0 ? 1 : 0);
      reload_q = encode_reload_q(reloading != 0, weapon_reload_progress(g, e));
    }
    snap->entities[snap->count++] = (NetEntity){.entity = (u8)e,
                                                .kind = g->kind[e],
                                                .owner = g->owner[e],
                                                .flags = g->flash[e],
                                                .extra = g->held_by[e],
                                                .weapon = weapon,
                                                .ammo = ammo,
                                                .reloading = reloading,
                                                .reload_q = reload_q,
                                                .scale = scale,
                                                .x = g->pos[e].x,
                                                .y = g->pos[e].y,
                                                .z = g->pos[e].z,
                                                .yaw = g->aim[e].yaw,
                                                .pitch = g->aim[e].pitch};
  }
}

static void send_peer_snapshot(int fd, const Peer *peer,
                               const NetSnapshot *snap) {
  u8 wire[NET_MAX_DATAGRAM];
  i32 wire_len = net_snapshot_encode(snap, wire, (i32)sizeof(wire));
  if (wire_len < 0)
    return;
  struct sockaddr_in address = {.sin_family = AF_INET,
                                .sin_addr.s_addr = peer->address,
                                .sin_port = peer->port};
  (void)sendto(fd, wire, (size_t)wire_len, 0, (const struct sockaddr *)&address,
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
  g->pos[e].x = (i16)(((int)player - (int)MAX_PLAYERS / 2) * 2 * POS_SCALE);
  g->pos[e].z = 5 * POS_SCALE;
  g->aim[e].yaw = 180;
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
  g->input[slot] = (InputCommand){.aim_yaw = g->aim[e].yaw};
  for (i32 i = 0; i < PLAYER_NAME_LENGTH; ++i)
    g->player_name[slot][i] = name.items[i];
  return slot;
}

static void release_held_system(GameState *g, u8 player) {
  u32 comps[] = {COMP_SCALE};
  EcsQuery q = ecs_query(&g->ecs, comps, 1);
  while (ecs_next(&q))
    if (g->held_by[q.entity] == player + 1u)
      g->held_by[q.entity] = 0;
}

static void remove_peer(GameState *g, int peer_index) {
  if (peer_index < 0 || peer_index >= MAX_PLAYERS)
    return;
  Peer *peer = &g->peers[peer_index];
  u8 player = peer->player;
  if (player >= MAX_PLAYERS)
    return;
  u32 e = g->player_entity[player];
  if (e != NET_PACKET_NONE && ecs_alive(&g->ecs, e))
    ecs_destroy(&g->ecs, e);
  release_held_system(g, player);
  g->player_entity[player] = NET_PACKET_NONE;
  g->input[player] = (InputCommand){0};
  peer->active = peer->got_sequence = peer->got_chat_sequence = 0;
}

static void server_relay_chat(GameState *g, int fd, int sender,
                              const NetChatPacket *received) {
  if (sender < 0 || sender >= MAX_PLAYERS || !g->peers[sender].active ||
      g->peers[sender].player >= MAX_PLAYERS)
    return;
  if (received->length > CHAT_LENGTH)
    return;
  NetChatPacket out = {
      .magic = NET_MAGIC,
      .type = NET_PACKET_CHAT,
      .player = g->peers[sender].player,
      .length = received->length,
      .sequence = g->next_chat_sequence++,
  };
  String src = game_player_name(g, out.player);
  memcpy(out.name, src.items, PLAYER_NAME_LENGTH);
  memcpy(out.text, received->text, out.length);
  for (int i = 0; i < MAX_PLAYERS; ++i) {
    if (!g->peers[i].active)
      continue;
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_addr.s_addr = g->peers[i].address,
                                  .sin_port = g->peers[i].port};
    out.session = g->peers[i].session;
    (void)sendto(fd, &out, sizeof(out), 0, (const struct sockaddr *)&address,
                 sizeof(address));
  }
}

static void server_handle_chat(GameState *g, int fd,
                               const struct sockaddr_in *from,
                               NetChatPacket *packet) {
  int pi = find_peer_session(g, packet->session);
  if (pi < 0 || pi >= MAX_PLAYERS)
    return;
  String wname = {(u8 *)packet->name, PLAYER_NAME_LENGTH, PLAYER_NAME_LENGTH};
  Peer *peer = &g->peers[pi];
  if (peer->player >= MAX_PLAYERS)
    return;
  if (!net_validate_chat_fields(wname, packet->length, packet->sequence,
                                peer->got_chat_sequence != 0,
                                peer->last_chat_sequence))
    return;
  peer->got_chat_sequence = 1;
  peer->last_chat_sequence = packet->sequence;
  peer->last_seen = g->tick;
  peer->address = from->sin_addr.s_addr;
  peer->port = from->sin_port;
  String text = {packet->text, packet->length, CHAT_LENGTH};
  net_sanitize_chat_text(text);
  server_relay_chat(g, fd, pi, packet);
}

static void server_handle_input(GameState *g, int fd,
                                const struct sockaddr_in *from,
                                const NetInputPacket *packet) {
  (void)fd;
  String wname = {(u8 *)packet->name, PLAYER_NAME_LENGTH, PLAYER_NAME_LENGTH};
  if (!net_validate_input_fields(packet->session, wname))
    return;
  int pi = find_peer_session(g, packet->session);
  if (packet->type == NET_PACKET_DISCONNECT) {
    if (pi >= 0 && pi < MAX_PLAYERS) {
      String have = game_player_name(g, (u32)pi);
      if (string_eq(have, wname))
        remove_peer(g, pi);
    }
    return;
  }
  if (pi < 0) {
    if (packet->claimed_player != NET_PACKET_NONE ||
        session_was_retired(g, packet->session))
      return;
    pi = add_peer(g, from, packet->session, wname);
    if (pi < 0)
      return;
  }
  if (pi < 0 || pi >= MAX_PLAYERS)
    return;
  Peer *peer = &g->peers[pi];
  if (peer->player >= MAX_PLAYERS)
    return;
  String have = game_player_name(g, peer->player);
  if (!string_eq(have, wname) || (packet->claimed_player != NET_PACKET_NONE &&
                                  packet->claimed_player != peer->player))
    return;
  if (!net_seq_fresh(packet->sequence, peer->got_sequence != 0,
                     peer->last_sequence))
    return;
  peer->got_sequence = 1;
  peer->last_sequence = packet->sequence;
  peer->last_seen = g->tick;
  peer->address = from->sin_addr.s_addr;
  peer->port = from->sin_port;
  g->input[peer->player].move_x = (i8)clamp_int(packet->move_x, -1, 1);
  g->input[peer->player].move_z = (i8)clamp_int(packet->move_z, -1, 1);
  g->input[peer->player].aim_yaw = wrap_degrees(packet->aim_yaw);
  g->input[peer->player].aim_pitch = (i16)clamp_int(packet->aim_pitch, -75, 75);
  g->input[peer->player].buttons = packet->buttons & BTN_ALL;
}

static void server_receive(GameState *g, int fd) {
  u8 buf[NET_MAX_DATAGRAM];
  for (;;) {
    struct sockaddr_in from;
    socklen_t from_size = sizeof(from);
    ssize_t raw =
        recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_size);
    if (raw < 0)
      return;
    if (raw == 0 || raw > (ssize_t)sizeof(buf))
      continue;
    i32 n = (i32)raw;
    NetChatPacket chat;
    if (net_chat_decode(buf, n, &chat)) {
      server_handle_chat(g, fd, &from, &chat);
      continue;
    }
    NetInputPacket input;
    if (net_input_decode(buf, n, &input)) {
      server_handle_input(g, fd, &from, &input);
      continue;
    }
  }
}

static void expire_peers_system(GameState *g) {
  for (int i = 0; i < MAX_PLAYERS; ++i)
    if (g->peers[i].active &&
        tick_age(g->tick, g->peers[i].last_seen) > SIM_HZ * 10)
      remove_peer(g, i);
}

static void server_send_all(GameState *g, int fd) {
  NetSnapshot snap;
  for (int i = 0; i < MAX_PLAYERS; ++i) {
    if (!g->peers[i].active || g->peers[i].player >= MAX_PLAYERS)
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
      expire_peers_system(&game);
      if (game.tick % SNAPSHOT_INTERVAL == 0)
        server_send_all(&game, fd);
      acc -= SIM_DT;
    }
    (void)nanosleep(&idle, NULL);
  }
  close(fd);
}

#endif
