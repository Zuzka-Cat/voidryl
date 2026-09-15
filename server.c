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
    f32 yaw = (f32)g->yaw[e] * DEG2RAD;
    f32 dx = (f32)in->move_z * sinf(yaw) - (f32)in->move_x * cosf(yaw);
    f32 dz = (f32)in->move_z * cosf(yaw) + (f32)in->move_x * sinf(yaw);
    f32 len = sqrtf(dx * dx + dz * dz);
    if (len > 1.0f) {
      dx /= len;
      dz /= len;
    }
    bool aiming = (in->buttons & BTN_AIM) != 0;
    bool sprinting =
        present_sprint_want(in->buttons, in->move_x, in->move_z);
    f32 speed = 3.0f;
    if (aiming)
      speed *= weapon_aim_move_scale(g->weapon_id[e]);
    else if (sprinting)
      speed = 4.5f;
    i16 step_x = (i16)lroundf(dx * speed);
    i16 step_z = (i16)lroundf(dz * speed);
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
    i16 nx = (i16)clamp_int((int)g->px[e] + step_x, -ARENA_POS, ARENA_POS);
    i16 nz = (i16)clamp_int((int)g->pz[e] + step_z, -ARENA_POS, ARENA_POS);
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
    if (p >= MAX_PLAYERS)
      continue;
    g->yaw[q.entity] = wrap_degrees(g->input[p].aim_yaw);
    g->pitch[q.entity] = (i16)clamp_int(g->input[p].aim_pitch, -75, 75);
  }
}

static u32 held_cube_for(const GameState *g, u8 player) {
  u32 comps[] = {COMP_POSITION, COMP_RENDERABLE, COMP_NETWORKED, COMP_SCALE};
  EcsQuery q = ecs_query(&g->ecs, comps, 4);
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
      u32 cubes[] = {COMP_POSITION, COMP_RENDERABLE, COMP_NETWORKED,
                     COMP_SCALE};
      EcsQuery cq = ecs_query(&g->ecs, cubes, 4);
      f32 best_d = 1000.0f;
      u32 best = ECS_INVALID_ENTITY;
      aim_dir(g->yaw[pe], g->pitch[pe], &dir);
      while (ecs_next(&cq)) {
        u32 c = cq.entity;
        if (g->kind[c] != KIND_CUBE || g->held_by[c] != 0)
          continue;
        Vector3 off = {(f32)(g->px[c] - g->px[pe]) / POS_SCALE,
                       (f32)(g->py[c] - (g->py[pe] + 96)) / POS_SCALE,
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
      // Grounded perspective play: the cube slides on the floor around the
      // holder. Yaw steers it, pitch drives distance/scale (look down ->
      // close/tiny, look forward/far -> distant/giant). Y is always
      // grounded at half height so it never floats. Scale persists after
      // release.
      Vector3 dir;
      aim_dir(g->yaw[pe], g->pitch[pe], &dir);
      (void)dir;
      f32 dist = hold_dist_for_pitch(g->pitch[pe]);
      f32 scale = clamp_float(dist / HOLD_BASE_DIST, CUBE_SCALE_MIN, CUBE_SCALE_MAX);
      g->scale[held] = scale;
      f32 yaw_rad = (f32)g->yaw[pe] * DEG2RAD;
      f32 hx = sinf(yaw_rad);
      f32 hz = cosf(yaw_rad);
      f32 half_h = 32.0f * scale;
      f32 half_xz = 28.8f * scale;
      f32 limit_x = fmaxf((f32)ARENA_POS - half_xz, 0.0f);
      f32 limit_z = fmaxf((f32)ARENA_POS - half_xz, 0.0f);
      f32 tx = (f32)g->px[pe] + hx * dist * (f32)POS_SCALE;
      f32 tz = (f32)g->pz[pe] + hz * dist * (f32)POS_SCALE;
      tx = clamp_float(tx, -limit_x, limit_x);
      tz = clamp_float(tz, -limit_z, limit_z);
      g->px[held] = (i16)lroundf(tx);
      g->py[held] = (i16)lroundf(half_h);
      g->pz[held] = (i16)lroundf(tz);
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
  // Per-weapon imprecision: slight cone, tighter while aiming.
  {
    u8 owner = g->owner[pe];
    bool aiming = owner < MAX_PLAYERS &&
                  (g->input[owner].buttons & BTN_AIM) != 0;
    f32 spread = weapon_spread_deg(g->weapon_id[pe], aiming);
    if (spread > 0.001f) {
      u32 h = mix32(g->tick * 2654435761u ^ (u32)pe * 40503u ^
                    (u32)owner * 65599u ^ (u32)g->weapon_ammo[pe] * 97u ^
                    0x9e3779b9u);
      u32 h2 = mix32(h ^ 0x85ebca6bu);
      f32 u1 = (f32)(h & 0xFFFFu) / 32767.5f - 1.0f;
      f32 u2 = (f32)(h2 & 0xFFFFu) / 32767.5f - 1.0f;
      f32 s = spread * DEG2RAD;
      dir.x += (r.x * u1 + u.x * u2) * s;
      dir.y += (r.y * u1 + u.y * u2) * s;
      dir.z += (r.z * u1 + u.z * u2) * s;
      f32 dlen = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
      if (dlen > 0.001f) {
        dir.x /= dlen;
        dir.y /= dlen;
        dir.z /= dlen;
      }
    }
  }
  g->px[e] = (i16)lroundf(muzzle.x * POS_SCALE);
  g->py[e] = (i16)lroundf(muzzle.y * POS_SCALE);
  g->pz[e] = (i16)lroundf(muzzle.z * POS_SCALE);
  // Inherit shooter velocity so strafing/falling doesn't make tracers drift
  // opposite in the shooter's own view. Same fixed-point units (units/tick),
  // so directly addable: moving right adds slight rightward flight, moving
  // left adds slight leftward flight, same for fwd/back/up/down. Without
  // this a right strafe (3 u/tick vs 32 u/tick bullet) reads as the bullet
  // flying left.
  f32 bvx = dir.x * BULLET_SPEED + (f32)g->vx[pe];
  f32 bvy = dir.y * BULLET_SPEED + (f32)g->vy[pe];
  f32 bvz = dir.z * BULLET_SPEED + (f32)g->vz[pe];
  g->vx[e] = (i16)lroundf(bvx);
  g->vy[e] = (i16)lroundf(bvy);
  g->vz[e] = (i16)lroundf(bvz);
  f32 blen = sqrtf(bvx * bvx + bvy * bvy + bvz * bvz);
  if (blen > 0.001f) {
    f32 s = clamp_float(bvy / blen, -1.0f, 1.0f);
    g->yaw[e] = wrap_degrees((int)lroundf(atan2f(bvx, bvz) * RAD2DEG));
    g->pitch[e] = (i16)clamp_int((int)lroundf(asinf(s) * RAD2DEG), -89, 89);
  } else {
    g->yaw[e] = wrap_degrees((int)lroundf(atan2f(dir.x, dir.z) * RAD2DEG));
    g->pitch[e] =
        (i16)clamp_int((int)lroundf(asinf(dir.y) * RAD2DEG), -89, 89);
  }
  g->owner[e] = g->owner[pe];
  g->kind[e] = KIND_BULLET;
  g->lifetime[e] = BULLET_LIFETIME_TICKS;
  add_kind_components(g, e, KIND_BULLET);
  return e;
}

// Shooter velocity inherited by ejected brass. Horizontal is full (1.0):
// brass leaves with the gun's velocity plus the ejector kick, so in the
// shooter's own view the walk cancels out and ejection always reads
// rightward at ejector speed. Anything less lets a strafe (3 u/tick)
// outrun the brass drift (~1.5 u/tick), which reads as shells flying
// backwards. Vertical is damped (0.3): the game jump is superhuman and full
// inheritance would fountain brass meters high.
#define CASING_INHERIT_XZ 1.0f
#define CASING_INHERIT_Y 0.3f

static void spawn_casing(GameState *g, u32 pe) {
  Vector3 f, r, u, port;
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
  gun_basis(g, pe, &f, &r, &u);
  // Ejection port: top-right of the slide, above the trigger guard.
  // gun_basis r points screen-right, so a positive port_side puts the port
  // on the right where the P99 cut is, keeping the spawn visible instead of
  // crossing through the gun.
  port = gun_point(g, pe, f, r, u,
                   (Vector3){sh->port_fwd, sh->port_side, sh->port_up});
  h = mix32(g->tick * 33u + e * 17u + (u32)g->owner[pe] * 91u + (u32)g->px[pe]);
  // P99 ejection: outward right, up at ~30-45 deg elevation, slightly
  // rearward (extractor pivots the case around its right rim, fixed ejector
  // kicks the left base). Jitter is +/- shell jitter in m/s.
  f32 jx = ((f32)((int)(h & 255u) - 128) / 128.0f) * sh->jitter;
  f32 jy = ((f32)((int)((h >> 8) & 255u) - 128) / 128.0f) * sh->jitter;
  f32 jz = ((f32)((int)((h >> 16) & 255u) - 128) / 128.0f) * sh->jitter;
  f32 right_ms = sh->eject_right + jx;
  f32 up_ms = sh->eject_up + jy;
  f32 back_ms = sh->eject_back + jz * 0.5f;
  // Fixed-point velocity is units/tick: m/s * POS_SCALE * SIM_DT (0.64x).
  f32 k = (f32)POS_SCALE * SIM_DT;
  f32 side = right_ms * k;
  f32 up = up_ms * k;
  f32 back = back_ms * k;
  g->px[e] = (i16)lroundf(port.x * POS_SCALE);
  g->py[e] = (i16)lroundf(port.y * POS_SCALE);
  g->pz[e] = (i16)lroundf(port.z * POS_SCALE);
  // gun_basis r already points screen-right, so eject right with +r.
  // Plus full horizontal shooter velocity (same fixed-point units, so
  // directly addable): the case is flung from a moving gun, and matching the
  // observer's motion keeps the rightward snap reading correctly in view.
  g->vx[e] =
      (i16)(lroundf(r.x * side + u.x * up - f.x * back) +
            lroundf((f32)g->vx[pe] * CASING_INHERIT_XZ));
  g->vy[e] =
      (i16)(lroundf(r.y * side + u.y * up - f.y * back) +
            lroundf((f32)g->vy[pe] * CASING_INHERIT_Y));
  g->vz[e] =
      (i16)(lroundf(r.z * side + u.z * up - f.z * back) +
            lroundf((f32)g->vz[pe] * CASING_INHERIT_XZ));
  g->yaw[e] = wrap_degrees(g->yaw[pe] + 50 + (int)(h % 41u));
  g->pitch[e] = (i16)(18 + (int)((h >> 5) % 29u));
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

// Backoff from the exact contact point along travel, in fixed-point units:
// the flat splat then sits just off the face instead of inside the solid.
#define MARK_BACKOFF 2.0f

static void make_impact_mark(GameState *g, u32 e, i16 x, i16 y, i16 z,
                             u8 extra) {
  if (count_kind(g, KIND_IMPACT_MARK) >= MAX_IMPACT_MARKS)
    destroy_oldest(g, KIND_IMPACT_MARK);
  g->px[e] = x;
  g->py[e] = y;
  g->pz[e] = z;
  // Packed face normal + strength + spatter material. Yaw/pitch are left
  // untouched: they still carry the bullet's travel dir, which the client
  // reflects for the spatter cone.
  g->held_by[e] = extra;
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
    // Real gravity (~9.8m/s^2): 1 unit per 16 ticks == 0.0625 units/tick.
    // Rises ~0.15-0.25s, then drops 1-2m right/rearward like real 9mm brass.
    if (g->tick % 16u == 0 && g->vy[e] > -8)
      --g->vy[e];
    if (g->py[e] <= 2) {
      g->py[e] = 2;
      if (g->vy[e] <= -2) {
        // Brass bounce: restitution ~0.35, horizontal loses ~25%.
        g->vy[e] = (i16)(-g->vy[e] * 3 / 8);
        g->vx[e] = (i16)(g->vx[e] * 3 / 4);
        g->vz[e] = (i16)(g->vz[e] * 3 / 4);
      } else {
        // Resting/rolling: light friction so cases roll instead of halting.
        g->vy[e] = 0;
        g->vx[e] = (i16)(g->vx[e] * 15 / 16);
        g->vz[e] = (i16)(g->vz[e] * 15 / 16);
        if (abs(g->vx[e]) < 1)
          g->vx[e] = 0;
        if (abs(g->vz[e]) < 1)
          g->vz[e] = 0;
      }
    }
    if (g->vx[e] || g->vy[e] || g->vz[e]) {
      bool airborne = g->py[e] > 2 || g->vy[e] != 0;
      if (airborne) {
        // Rapid end-over-end tumble in flight.
        g->yaw[e] = wrap_degrees(g->yaw[e] + 9 + (g->held_by[e] & 7));
        g->pitch[e] =
            wrap_degrees(g->pitch[e] + 11 + ((g->held_by[e] >> 3) & 7));
        g->held_by[e] = (u8)(g->held_by[e] + 13u);
      } else {
        // Slow roll on the ground.
        g->yaw[e] = wrap_degrees(g->yaw[e] + 2 + (g->held_by[e] & 3));
        g->pitch[e] =
            wrap_degrees(g->pitch[e] + 2 + ((g->held_by[e] >> 2) & 3));
        g->held_by[e] = (u8)(g->held_by[e] + 5u);
      }
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

// Exact hitscan lives in shared.c (seg_box_hit over COMP_SOLID).
static void projectile_system(GameState *g) {
  u32 comps[] = {COMP_POSITION, COMP_VELOCITY, COMP_PROJECTILE, COMP_LIFETIME};
  EcsQuery q = ecs_query(&g->ecs, comps, 4);
  int bound = ARENA * POS_SCALE;
  while (ecs_next(&q)) {
    u32 e = q.entity;
    Vector3 p0 = {(float)g->px[e], (float)g->py[e], (float)g->pz[e]};
    Vector3 d = {(float)g->vx[e], (float)g->vy[e], (float)g->vz[e]};
    // Nearest solid along the tick segment. Floor, cubes and players all
    // answer the same segment query, so contact resolution below never
    // branches on what was hit. Held cubes ride their holder and the owner
    // cannot hit themselves: both are hit-eligibility rules, not mark logic.
    bool hit = false;
    float best = 1.0f;
    Vector3 bn = {0.0f, 1.0f, 0.0f};
    u32 bs = ECS_INVALID_ENTITY;
    u32 solids[] = {COMP_SOLID};
    EcsQuery sq = ecs_query(&g->ecs, solids, 1);
    while (ecs_next(&sq)) {
      u32 s = sq.entity;
      if (s == e)
        continue;
      if (g->kind[s] == KIND_CUBE && g->held_by[s] != 0)
        continue;
      if (g->kind[s] == KIND_PLAYER && g->owner[s] == g->owner[e])
        continue;
      Vector3 bmin, bmax, n;
      float t;
      solid_bounds(g, s, &bmin, &bmax);
      if (seg_box_hit(p0, d, bmin, bmax, &t, &n) && (!hit || t < best)) {
        hit = true;
        best = t;
        bn = n;
        bs = s;
      }
    }
    if (!hit) {
      g->px[e] = (i16)((int)g->px[e] + g->vx[e]);
      g->py[e] = (i16)((int)g->py[e] + g->vy[e]);
      g->pz[e] = (i16)((int)g->pz[e] + g->vz[e]);
      if (g->py[e] > bound || g->px[e] < -bound || g->px[e] > bound ||
          g->pz[e] < -bound || g->pz[e] > bound)
        ecs_destroy(&g->ecs, e);
      continue;
    }
    // Victim hit-feedback (direction indicator). Players are the only
    // entities carrying flash state, so this stays type-aware; the mark
    // itself is built uniformly below.
    if (bs != ECS_INVALID_ENTITY && g->kind[bs] == KIND_PLAYER) {
      u32 src = e;
      u8 ow = g->owner[e];
      if (ow < MAX_PLAYERS && g->player_entity[ow] != NET_PACKET_NONE &&
          ecs_alive(&g->ecs, g->player_entity[ow]))
        src = g->player_entity[ow];
      mark_hit_dir(g, bs, src);
    }
    float spd = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    float back = spd > 0.001f ? MARK_BACKOFF / spd : 0.0f;
    float bt = best - back;
    if (bt < 0.0f)
      bt = 0.0f;
    i16 mx = (i16)lroundf(p0.x + d.x * bt);
    i16 my = (i16)lroundf(p0.y + d.y * bt);
    i16 mz = (i16)lroundf(p0.z + d.z * bt);
    // Strength is the normal share of the impact velocity: dead-on hits
    // splat big, grazes leave a dot.
    float frac = spd > 0.001f
                     ? fabsf(d.x * bn.x + d.y * bn.y + d.z * bn.z) / spd
                     : 0.0f;
    u8 str = (u8)(frac * (float)MARK_STR_LEVELS);
    if (str >= MARK_STR_LEVELS)
      str = MARK_STR_LEVELS - 1u;
    u8 skind = g->kind[bs];
    u8 mat = skind < sizeof(SOLID_MAT) ? SOLID_MAT[skind] : MARK_MAT_PURPLE;
    make_impact_mark(g, e, mx, my, mz,
                     mark_extra_pack(mark_code_for_vec(bn), str, mat));
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

// Authoritative presentation smoothing per player, replicated so remotes
// render the same ADS/bob/lean/reload/sprint flavor. Steps the shared
// present_* helpers — the exact ones the client uses for local prediction.
static void present_system(GameState *g) {
  for (u32 p = 0; p < MAX_PLAYERS; ++p) {
    u32 e = g->player_entity[p];
    if (e == NET_PACKET_NONE || !ecs_alive(&g->ecs, e) ||
        g->kind[e] != KIND_PLAYER)
      continue;
    u8 buttons = g->input[p].buttons;
    bool reloading = g->weapon_reload[e] != 0;
    present_blend_tick(&g->sv_aim[p],
                       present_aim_want(buttons, reloading), 12.0f, SIM_DT);
    present_walk_tick(&g->sv_phase[p], &g->sv_blend[p], g->input[p].move_x,
                      g->input[p].move_z, SIM_DT);
    present_lean_tick(&g->sv_leanx[p], &g->sv_velx[p],
                      (f32)g->input[p].move_x, SIM_DT);
    present_lean_tick(&g->sv_leanz[p], &g->sv_velz[p],
                      (f32)g->input[p].move_z, SIM_DT);
    present_blend_tick(&g->sv_sprint[p],
                       present_sprint_want(buttons, g->input[p].move_x,
                                           g->input[p].move_z),
                       8.0f, SIM_DT);
  }
}

static void server_tick(GameState *g) {
  aim_system(g);
  movement_system(g);
  holding_system(g);
  weapon_system(g);
  present_system(g);
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
    u8 aim_q = 0, phase_q = 0, blend_q = 0, reload_q = 0, sprint_q = 0;
    u8 scale = 0;
    i8 lean_x_q = 0, lean_z_q = 0;
    if (g->kind[e] == KIND_CUBE) {
      scale = scale_encode(scale_value(g, e));
    } else if (g->kind[e] == KIND_CASING) {
      // Shell type rides the weapon field so every client renders the
      // per-gun casing size. No protocol change: 0 decodes to P99.
      weapon = g->weapon_id[e] < WEAPON_COUNT ? g->weapon_id[e] : WEAPON_P99;
    } else if (g->kind[e] == KIND_PLAYER) {
      weapon = g->weapon_id[e];
      ammo = g->weapon_ammo[e];
      reloading = (u8)(g->weapon_reload[e] != 0 ? 1 : 0);
      if (g->owner[e] < MAX_PLAYERS) {
        u8 p = g->owner[e];
        aim_q = encode_01(g->sv_aim[p]);
        phase_q = encode_01(g->sv_phase[p] / (2.0f * PI));
        blend_q = encode_01(g->sv_blend[p]);
        sprint_q = encode_01(g->sv_sprint[p]);
        lean_x_q = encode_11(g->sv_leanx[p]);
        lean_z_q = encode_11(g->sv_leanz[p]);
        reload_q =
            encode_reload_q(reloading != 0, weapon_reload_progress(g, e));
      }
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
                    .aim_q = aim_q,
                    .walk_phase_q = phase_q,
                    .walk_blend_q = blend_q,
                    .reload_q = reload_q,
                    .sprint_q = sprint_q,
                    .scale = scale,
                    .lean_x_q = lean_x_q,
                    .lean_z_q = lean_z_q,
                    .x = g->px[e],
                    .y = g->py[e],
                    .z = g->pz[e],
                    .yaw = g->yaw[e],
                    .pitch = g->pitch[e]};
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
  (void)sendto(fd, wire, (size_t)wire_len, 0,
               (const struct sockaddr *)&address, sizeof(address));
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
  if (peer_index < 0 || peer_index >= MAX_PLAYERS)
    return;
  Peer *peer = &g->peers[peer_index];
  u8 player = peer->player;
  if (player >= MAX_PLAYERS)
    return;
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
                              const NetChatPacket *received) {
  if (sender < 0 || sender >= MAX_PLAYERS || !g->peers[sender].active ||
      g->peers[sender].player >= MAX_PLAYERS)
    return;
  // Rebuild: never reflect sender-controlled player/sequence.
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
    (void)sendto(fd, &out, sizeof(out), 0,
                 (const struct sockaddr *)&address, sizeof(address));
  }
}

static void server_handle_chat(GameState *g, int fd,
                                 const struct sockaddr_in *from,
                                 NetChatPacket *packet) {
  int pi = find_peer_session(g, packet->session);
  if (pi < 0 || pi >= MAX_PLAYERS)
    return;
  String wname = {(u8 *)packet->name, PLAYER_NAME_LENGTH,
                  PLAYER_NAME_LENGTH};
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
  String wname = {(u8 *)packet->name, PLAYER_NAME_LENGTH,
                  PLAYER_NAME_LENGTH};
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
  g->input[peer->player].aim_pitch =
      (i16)clamp_int(packet->aim_pitch, -75, 75);
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
    // Drop malformed datagrams.
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
