#ifndef CLIENT_C
#define CLIENT_C

#include "rlgl.h"
#include "shared.c"

static const Color PAL_DARK = {16, 8, 24, 255};
static const Color PAL_PURPLE = {60, 30, 102, 255};
static const Color PAL_MID = {155, 123, 208, 255};
static const Color PAL_LIGHT = {246, 213, 232, 255};

static u32 local_player_entity(const GameState *g);

static int open_udp_client(String host, u16 port, struct sockaddr_in *server) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct in_addr address;
  if (fd < 0 || !parse_ipv4(host, &address)) {
    if (fd >= 0)
      close(fd);
    return -1;
  }
  *server = (struct sockaddr_in){.sin_family = AF_INET,
                                 .sin_addr = address,
                                 .sin_port = htons(port)};
  if (connect(fd, (const struct sockaddr *)server, sizeof(*server)) < 0) {
    close(fd);
    return -1;
  }
  socket_nonblocking(fd);
  return fd;
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
  // Proper world reset: a new session invalidates every server entity id,
  // so drop the whole snapshot-driven world here. The world starts empty
  // (client_init) and is rebuilt only by snapshots, which is also what
  // fixes the old ghost-cube duplication at the root.
  client_clear_world(g);
  g->gun_kick = 0;
  g->gun_recoil = g->gun_recoil_velocity = 0.0f;
  g->gun_rise = g->gun_rise_velocity = 0.0f;
  g->gun_slide = 0.0f;
  g->aim_blend = 0.0f;
  g->screen_shake = 0.0f;
  g->walk_phase = 0.0f;
  g->walk_blend = 0.0f;
  g->move_sway_x = 0.0f;
  g->move_sway_z = 0.0f;
  g->move_vel_x = 0.0f;
  g->move_vel_z = 0.0f;
  g->sprint_blend = 0.0f;
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
  if (!was) {
    notice_set(g, STR("LINKED"));
  } else {
    u8 left = (u8)(g->visible_players & ~mask);
    u8 joined = (u8)(mask & ~g->visible_players);
    for (u32 p = 0; p < MAX_PLAYERS; ++p) {
      u8 bit = (u8)(1u << p);
      if (left & bit) {
        String name = game_player_name(g, p);
        notice_name_status(g, name, STR("DROPPED"));
      }
      if (joined & bit) {
        String jname = {(u8 *)snap->names[p], PLAYER_NAME_LENGTH,
                        PLAYER_NAME_LENGTH};
        notice_name_status(g, jname, STR("LINKED"));
      }
    }
  }
  for (u32 p = 0; p < MAX_PLAYERS; ++p)
    for (i32 i = 0; i < PLAYER_NAME_LENGTH; ++i)
      g->player_name[p][i] = snap->names[p][i];
  g->visible_players = mask;
  g->snapshot_seen = 0;
  g->local_player = snap->local_player;
  g->last_snapshot_tick = snap->tick;
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
    i16 nx = w->x, ny = w->y, nz = w->z;
    i16 dx = (i16)(nx - g->px[e]);
    i16 dy = (i16)(ny - g->py[e]);
    i16 dz = (i16)(nz - g->pz[e]);
    bool snap_pos =
        !had || abs(dx) > 240 || abs(dz) > 240 || dy > 48 || dy < -240;
    g->vx[e] = snap_pos ? 0 : dx;
    g->vy[e] = snap_pos ? 0 : dy;
    g->vz[e] = snap_pos ? 0 : dz;
    if (had && w->kind == KIND_PLAYER) {
      g->lag_yaw[e] = lag_encode(wrap_degrees(w->yaw - g->yaw[e]));
      g->lag_pitch[e] = lag_encode((int)w->pitch - (int)g->pitch[e]);
    } else if (w->kind == KIND_PLAYER) {
      g->lag_yaw[e] = LAG_NEUTRAL;
      g->lag_pitch[e] = LAG_NEUTRAL;
    }
    g->kind[e] = w->kind;
    g->owner[e] = w->owner;
    g->px[e] = nx;
    g->py[e] = ny;
    g->pz[e] = nz;
    g->yaw[e] = w->yaw;
    g->pitch[e] = w->pitch;
    g->flash[e] = w->flags;
    g->held_by[e] = w->extra;
    if (w->kind == KIND_CUBE)
      g->scale[e] = scale_decode(w->scale);
    if (w->kind == KIND_PLAYER) {
      if (!had)
        weapon_set_from_snapshot(g, e, w->weapon, w->ammo,
                                 w->reloading != 0);
      else
        weapon_apply_snapshot(g, e, w->weapon, w->ammo,
                              w->reloading != 0);
      // Replicated presentation for the remote renderer (local entity
      // keeps its predicted floats instead).
      g->r_aim[e] = w->aim_q;
      g->r_phase[e] = w->walk_phase_q;
      g->r_blend[e] = w->walk_blend_q;
      g->r_reload[e] = w->reload_q;
      g->r_sprint[e] = w->sprint_q;
      g->r_leanx[e] = w->lean_x_q;
      g->r_leanz[e] = w->lean_z_q;
    }
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
          notice_set(g, STR("DROPPED"));
        renew_client_session(g);
      }
      return;
    }
    if (!net_magic_ok(buf, (i32)n) || (size_t)n > sizeof(NetSnapshot))
      continue;
    if (net_packet_ok(buf, (i32)n, NET_PACKET_CHAT,
                      (i32)sizeof(NetChatPacket))) {
      NetChatPacket packet;
      memcpy(&packet, buf, sizeof(packet));
      String wname = {(u8 *)packet.name, PLAYER_NAME_LENGTH,
                      PLAYER_NAME_LENGTH};
      if (packet.session != g->session)
        continue;
      if (!net_validate_chat_fields(wname, packet.length, packet.sequence,
                                    g->got_chat_sequence != 0,
                                    g->last_chat_sequence))
        continue;
      g->got_chat_sequence = 1;
      g->last_chat_sequence = packet.sequence;
      String text = {packet.text, packet.length, CHAT_LENGTH};
      notice_chat_msg(g, wname, text);
      continue;
    }
    if (!net_packet_ok(buf, (i32)n, NET_PACKET_SNAPSHOT,
                       (i32)sizeof(NetSnapshot)))
      continue;
    NetSnapshot snap;
    memcpy(&snap, buf, sizeof(snap));
    if (!net_validate_snapshot_fields(&snap, g->session, g->got_snapshot != 0,
                                      g->last_snapshot_tick))
      continue;
    g->got_snapshot = 1;
    apply_snapshot(g, &snap);
  }
}

static void client_send_input(GameState *g, int fd, u8 type) {
  NetInputPacket packet = {.magic = NET_MAGIC,
                           .type = type,
                           .claimed_player = g->local_player,
                           .session = g->session};
  String req = game_requested_name(g);
  memcpy(packet.name, req.items, PLAYER_NAME_LENGTH);
  if (type == NET_PACKET_INPUT) {
    packet.buttons = g->input[0].buttons & (BTN_FIRE | BTN_GRAB | BTN_JUMP |
                                            BTN_AIM | BTN_SPRINT);
    packet.sequence = g->next_sequence++;
    packet.move_x = g->input[0].move_x;
    packet.move_z = g->input[0].move_z;
    packet.aim_yaw = g->view_yaw;
    packet.aim_pitch = g->view_pitch;
  }
  (void)send(fd, &packet, sizeof(packet), 0);
}

static void client_send_chat(GameState *g, int fd) {
  NetChatPacket packet = {.magic = NET_MAGIC,
                          .type = NET_PACKET_CHAT,
                          .player = g->local_player,
                          .length = g->chat_length,
                          .session = g->session,
                          .sequence = g->next_chat_sequence++};
  String req = game_requested_name(g);
  String chat = game_chat_input(g);
  memcpy(packet.name, req.items, PLAYER_NAME_LENGTH);
  if (chat.length > 0)
    memcpy(packet.text, chat.items, (size_t)chat.length);
  (void)send(fd, &packet, sizeof(packet), 0);
}

static void update_input(GameState *g, int fd) {
  g->look_delta_x = g->look_delta_y = 0.0f;
  g->help = IsKeyDown(KEY_F1) ? 1u : 0u;
  if (g->help) {
    g->input[0].move_x = g->input[0].move_z = 0;
    g->input[0].buttons = 0;
    (void)GetMouseDelta();
    while (GetKeyPressed() != 0) {
    }
    return;
  }
  if (!g->chatting && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_T))) {
    g->chatting = 1;
    g->chat_length = 0;
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
    } else if (IsKeyPressed(KEY_ENTER)) {
      if (g->chat_length > 0)
        client_send_chat(g, fd);
      g->chatting = g->chat_length = 0;
    } else {
      if (IsKeyPressed(KEY_BACKSPACE) && g->chat_length > 0)
        --g->chat_length;
      while ((c = GetKeyPressed()) != 0) {
        c = CHAR_TO_UPPER(c);
        if (CHAR_IS_CHAT(c) && g->chat_length < CHAT_LENGTH)
          g->chat_input[g->chat_length++] = (u8)c;
      }
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
  if (IsKeyDown(KEY_E))
    g->input[0].buttons |= BTN_GRAB;
  bool aiming = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
  if (aiming)
    g->input[0].buttons |= BTN_AIM;
  if (IsKeyDown(KEY_SPACE))
    g->input[0].buttons |= BTN_JUMP;
  if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
    g->input[0].buttons |= BTN_SPRINT;
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
  // ADS precision from the weapon config: slower, finer look while zoomed.
  u32 sens_e = local_player_entity(g);
  u8 sens_wid = sens_e != ECS_INVALID_ENTITY ? g->weapon_id[sens_e] : WEAPON_P99;
  f32 sens = aiming ? weapon_aim_sens_scale(sens_wid) : 1.0f;
  f32 sdx = mouse.x * sens;
  f32 sdy = mouse.y * sens;
  g->look_delta_x = sdx;
  g->look_delta_y = sdy;
  g->view_yaw = wrap_degrees((int)g->view_yaw - (int)lroundf(sdx));
  g->view_pitch = clamp_i16((int)g->view_pitch - (int)lroundf(sdy), -75, 75);
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
  u32 e = local_player_entity(g);
  Vector3 dir = {0.0f, 0.0f, 1.0f};
  u8 wid = e != ECS_INVALID_ENTITY ? g->weapon_id[e] : WEAPON_P99;
  f32 hip_fov = weapon_hip_fov(wid);
  f32 aim_fov = weapon_aim_fov(wid);
  Camera3D cam = {.position = (Vector3){0.0f, 1.5f, 0.0f},
                   .up = (Vector3){0.0f, 1.0f, 0.0f},
                   .fovy = hip_fov,
                   .projection = CAMERA_PERSPECTIVE};
  if (e != ECS_INVALID_ENTITY) {
    Vector3 p = entity_pos(g, e, a);
    cam.position = (Vector3){p.x, 1.5f + p.y, p.z};
    aim_dir(g->view_yaw, g->view_pitch, &dir);
  }
  cam.fovy = hip_fov * (1.0f - g->aim_blend) + aim_fov * g->aim_blend;
  cam.target = (Vector3){cam.position.x + dir.x, cam.position.y + dir.y,
                          cam.position.z + dir.z};
  return cam;
}

// P99-only reload presentation. One configurable timer drives it: the anim
// reads weapon_reload_progress (== reload_ticks), so count and motion can
// never drift apart. Choreography over t in 0..1: smooth down (0-0.38),
// brief hold/mag-out wait (to 0.58), quick forward thud/mag-slam (to 0.72),
// then a snappy whip back with a whisper of overshoot past ready.
// Never call this for another weapon id; each future gun gets its own
// gun_reload_pose_<name>.
static f32 reload_ss01(f32 x) {
  if (x <= 0.0f)
    return 0.0f;
  if (x >= 1.0f)
    return 1.0f;
  return x * x * (3.0f - 2.0f * x);
}

static void gun_reload_pose_p99(f32 reload_t) {
  if (reload_t <= 0.0f)
    return;
  f32 env = reload_ss01(reload_t / 0.38f);
  f32 thud = 0.0f;
  if (reload_t > 0.58f && reload_t < 0.72f)
    thud = sinf(PI * (reload_t - 0.58f) / 0.14f);
  f32 ret = 1.0f;
  if (reload_t > 0.72f) {
    f32 u = (reload_t - 0.72f) / 0.28f;
    if (u > 1.0f)
      u = 1.0f;
    f32 c1 = 1.70158f, c3 = c1 + 1.0f;
    f32 um = u - 1.0f;
    ret = 1.0f - (1.0f + c3 * um * um * um + c1 * um * um);
  }
  f32 ang = (env * 34.0f + thud * 5.0f) * ret;
  f32 fwd = (env * 0.09f + thud * 0.05f) * ret;
  f32 dwn = env * 0.03f * ret;
  rlTranslatef(0.0f, -dwn, fwd);
  rlTranslatef(-0.32f, -0.05f, 0.25f);
  rlRotatef(ang, 1.0f, 0.0f, 0.0f);
  rlTranslatef(0.32f, 0.05f, -0.25f);
}

static void draw_gun_p99(f32 recoil, f32 slide, f32 scale, bool firing,
                         f32 reload_t) {
  rlPushMatrix();
  rlScalef(scale, scale, scale);
  gun_reload_pose_p99(reload_t);
  Vector3 sl = {-0.32f, 0.0f, 0.30f - recoil - slide};
  Vector3 sls = {0.16f, 0.13f, 0.46f};
  Vector3 gr = {-0.32f, -0.15f, 0.18f - recoil};
  Vector3 grs = {0.13f, 0.26f, 0.15f};
  DrawCubeV(sl, sls, PAL_MID);
  DrawCubeWiresV(sl, sls, PAL_DARK);
  DrawCubeV(gr, grs, PAL_PURPLE);
  DrawCubeWiresV(gr, grs, PAL_DARK);
  // Sights: two slim rear blocks flush with the housing sides (wide empty
  // notch between them), plus one slim front block. All three ride the slide.
  f32 sz = -recoil - slide;
  Vector3 rss = {0.024f, 0.045f, 0.05f};
  Vector3 rl = {-0.378f, 0.085f, 0.09f + sz};
  Vector3 rr = {-0.262f, 0.085f, 0.09f + sz};
  DrawCubeV(rl, rss, PAL_DARK);
  DrawCubeWiresV(rl, rss, PAL_MID);
  DrawCubeV(rr, rss, PAL_DARK);
  DrawCubeWiresV(rr, rss, PAL_MID);
  Vector3 fs = {-0.32f, 0.088f, 0.48f + sz};
  Vector3 fss = {0.024f, 0.055f, 0.024f};
  DrawCubeV(fs, fss, PAL_DARK);
  DrawCubeWiresV(fs, fss, PAL_MID);
  if (firing)
    DrawCylinderEx((Vector3){-0.32f, 0.0f, 0.56f - recoil},
                   (Vector3){-0.32f, 0.0f, 0.72f - recoil}, 0.065f, 0.0f, 5,
                   PAL_LIGHT);
  rlPopMatrix();
}

// Fallback for unknown/future weapon ids: plain housing, deliberately no
// P99 whip. Add draw_gun_<name> + gun_reload_pose_<name> and wire them into
// draw_gun_local instead of reusing the P99 animation.
static void draw_gun_fallback(f32 recoil, f32 slide, f32 scale, bool firing) {
  (void)firing;
  rlPushMatrix();
  rlScalef(scale, scale, scale);
  Vector3 sl = {-0.32f, 0.0f, 0.30f - recoil - slide};
  DrawCubeV(sl, (Vector3){0.16f, 0.13f, 0.46f}, PAL_MID);
  DrawCubeWiresV(sl, (Vector3){0.16f, 0.13f, 0.46f}, PAL_DARK);
  Vector3 gr = {-0.32f, -0.15f, 0.18f - recoil};
  DrawCubeV(gr, (Vector3){0.13f, 0.26f, 0.15f}, PAL_PURPLE);
  DrawCubeWiresV(gr, (Vector3){0.13f, 0.26f, 0.15f}, PAL_DARK);
  rlPopMatrix();
}

// Per-weapon viewmodel dispatcher. To add a gun (e.g. P90), add a WeaponId +
// WeaponDef entry (with its own ADS pose/FOV/spread), write
// draw_gun_p90 + gun_reload_pose_p90, and add a case below. Game logic stays
// in shared.c; unknown ids fall back to the plain housing and never play
// the P99 whip.
static void draw_gun_local(u8 weapon_id, f32 recoil, f32 slide, f32 scale,
                           bool firing, f32 reload_t) {
  switch (weapon_id) {
  case WEAPON_P99:
    draw_gun_p99(recoil, slide, scale, firing, reload_t);
    break;
  default:
    draw_gun_fallback(recoil, slide, scale, firing);
    break;
  }
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
  f32 yaw_lag = (f32)lag_decode(g->lag_yaw[e]) * lag;
  f32 pitch_lag = (f32)lag_decode(g->lag_pitch[e]) * lag;
  // Replicated presentation, decoded once and posed through the same shared
  // helpers as the local viewmodel (server smooths, we just apply).
  const WeaponDef *rdef = weapon_def(g->weapon_id[e]);
  f32 ab = present_unq01(g->r_aim[e]);
  f32 rph = present_unq01(g->r_phase[e]) * 2.0f * PI;
  f32 rwb = present_unq01(g->r_blend[e]);
  f32 rlx = present_unq11(g->r_leanx[e]);
  f32 rlz = present_unq11(g->r_leanz[e]);
  f32 sb = present_unq01(g->r_sprint[e]);
  f32 reload_t = g->r_reload[e] == 0
                     ? 0.0f
                     : (f32)(g->r_reload[e] - 1) / 254.0f;
  f32 lox, loz;
  present_lean_offsets(rlx, rlz, &lox, &loz);
  f32 rbox, rboy, rboz, rboroll;
  present_bob_offsets(rph, rwb, 1.0f - 0.5f * ab, &rbox, &rboy, &rboz,
                      &rboroll);
  Color body = hurt ? PAL_LIGHT : PAL_MID;
  rlPushMatrix();
  rlTranslatef(pos.x, pos.y, pos.z);
  rlRotatef((f32)g->yaw[e], 0.0f, 1.0f, 0.0f);
  draw_cube_lit((Vector3){0.0f, 0.72f, 0.0f}, (Vector3){0.50f, 1.36f, 0.30f},
                body);
  draw_cube_lit((Vector3){0.0f, 1.44f, 0.06f}, (Vector3){0.26f, 0.26f, 0.26f},
                hurt ? PAL_LIGHT : PAL_PURPLE);
  rlPushMatrix();
  // Third-person read of the same pose: gun lifts toward the face and takes
  // the per-gun cant/trim/tilt with aim and sprint, plus replicated lean
  // and bob. Reload whips in place via the shared dispatcher (see draw_gun).
  rlTranslatef(-0.22f - yaw_lag * 0.018f + ab * 0.10f + lox + rbox,
               1.16f + pitch_lag * 0.009f + fire * 0.05f + ab * 0.10f + rboy,
               0.30f - fire * 0.02f + loz + rboz);
  // True recoil climb for remotes plus stronger turn bob with up/down
  // cross-coupling from yaw motion.
  rlRotatef((f32)-g->pitch[e] - pitch_lag * 0.80f - yaw_lag * 0.28f -
                fire * 13.0f + rdef->ads_pitch_deg * ab +
                rdef->sprint_tilt_deg * sb,
            1.0f, 0.0f, 0.0f);
  rlRotatef(-yaw_lag * 0.85f, 0.0f, 1.0f, 0.0f);
  rlRotatef(-yaw_lag * 0.30f + rdef->ads_cant_deg * ab + rboroll, 0.0f, 0.0f,
            1.0f);
  draw_gun_local(g->weapon_id[e], fire * 0.03f, fire * 0.08f, 1.0f, firing,
                 reload_t);
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
  DrawCylinderEx((Vector3){0.0f, -0.085f, 0.0f}, (Vector3){0.0f, 0.085f, 0.0f},
                 0.048f, 0.042f, 6, PAL_DARK);
  DrawCylinderEx((Vector3){0.0f, -0.082f, 0.0f}, (Vector3){0.0f, 0.082f, 0.0f},
                 0.044f, 0.038f, 6, PAL_LIGHT);
  DrawCylinderEx((Vector3){0.0f, -0.089f, 0.0f}, (Vector3){0.0f, -0.075f, 0.0f},
                 0.052f, 0.052f, 6, PAL_MID);
  rlPopMatrix();
}

static void draw_world(const GameState *g, f32 a) {
  u32 comps[] = {COMP_POSITION, COMP_RENDERABLE};
  EcsQuery q = ecs_query(&g->ecs, comps, 2);
  while (ecs_next(&q)) {
    u32 e = q.entity;
    Vector3 p = entity_pos(g, e, a);
    switch (g->kind[e]) {
    case KIND_CUBE: {
      f32 s = scale_value(g, e);
      draw_cube_lit(p, (Vector3){0.9f * s, 1.0f * s, 0.9f * s},
                    g->held_by[e] ? PAL_LIGHT : PAL_MID);
      break;
    }
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
    if (TOM_THUMB_3X5_CHARS[i] == (char)c)
      return (int)i;
  return -1;
}

static void draw_small_text(int x, int y, String text, Color color) {
  for (i32 i = 0; i < text.length; ++i) {
    int idx = glyph_index(text.items[i]);
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
    u8 ch = digits[n - 1 - i];
    String t = {&ch, 1, 1};
    draw_small_text(x + i * 4, y, t, color);
  }
}

static int ammo_field_width(unsigned mag) {
  int d = 1;
  unsigned m = mag;
  while (m >= 10u) {
    m /= 10u;
    ++d;
  }
  return d < 2 ? 2 : d;
}

static void draw_number_padded(int x, int y, unsigned value, int width,
                               Color color) {
  u8 buf[8];
  if (width > (int)sizeof(buf))
    width = (int)sizeof(buf);
  for (int i = width - 1; i >= 0; --i) {
    buf[i] = (u8)('0' + value % 10u);
    value /= 10u;
  }
  for (int i = 0; i < width; ++i) {
    String t = {&buf[i], 1, 1};
    draw_small_text(x + i * 4, y, t, color);
  }
}

static Color ammo_color(unsigned ammo, unsigned mag, bool reloading) {
  if (reloading) {
    int blink = ((int)(GetTime() * 8.0)) & 1;
    return blink ? PAL_LIGHT : PAL_MID;
  }
  if (mag == 0)
    return PAL_LIGHT;
  f32 f = (f32)ammo / (f32)mag;
  if (f > 0.6f)
    return PAL_LIGHT;
  if (f > 0.30f)
    return PAL_MID;
  if (f <= 0.001f)
    return PAL_PURPLE;
  // Low ammo: cool pulse between mid and dark-purple.
  int blink = ((int)(GetTime() * 6.0)) & 1;
  return blink ? PAL_MID : PAL_PURPLE;
}

static int small_text_width(String text) {
  return text.length ? (int)text.length * 4 - 1 : 0;
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

static void draw_help_line(int y, String text, Color color) {
  int w = small_text_width(text);
  int x = (LOGICAL_W - w) / 2;
  if (x < 1)
    x = 1;
  draw_small_text(x, y, text, color);
}

static void draw_hud(GameState *g) {
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
  if (!g->chatting && g->notice_ticks && g->notice_length) {
    String note = game_notice(g);
    int w = small_text_width(note);
    int y = w + 4 < fps_x ? 1 : 9;
    DrawRectangle(1, y, w + 2, 7, PAL_DARK);
    draw_small_text(2, y + 1, note, PAL_LIGHT);
  }
  if (!g->chatting) {
    DrawRectangle(fps_x - 1, 1, fps_w + 2, 7, PAL_DARK);
    draw_small_text(fps_x, 2, STR("FPS"), PAL_MID);
    draw_number(fps_x + 16, 2, fps, PAL_LIGHT);
  }
  if (e != ECS_INVALID_ENTITY)
    draw_hit_indicator(g, e);
  if (g->aim_blend < 0.5f)
    DrawRectangle(LOGICAL_W / 2, LOGICAL_H / 2, 1, 1, PAL_LIGHT);
  if (e != ECS_INVALID_ENTITY && g->connected) {
    bool reloading = weapon_is_reloading(g, e);
    unsigned ammo = weapon_ammo(g, e);
    unsigned mag = weapon_mag_size(g, e);
    if (ammo > mag)
      ammo = mag;
    int width = ammo_field_width(mag);
    int num_w = width * 4 - 1;
    int bx = LOGICAL_W - num_w - 3;
    DrawRectangle(bx, LOGICAL_H - 9, num_w + 2, 7, PAL_DARK);
    draw_number_padded(bx + 1, LOGICAL_H - 8, ammo, width,
                       ammo_color(ammo, mag, reloading));
  }
  if (g->chatting) {
    String chat = game_chat_input(g);
    int w = 8 + small_text_width(chat);
    if (w < 7)
      w = 7;
    if (w > LOGICAL_W - 4)
      w = LOGICAL_W - 4;
    DrawRectangle(1, 1, w + 2, 7, PAL_DARK);
    draw_small_text(2, 2, STR("T:"), PAL_MID);
    draw_small_text(10, 2, chat, PAL_LIGHT);
  }
  if (g->help) {
    DrawRectangle(0, 0, LOGICAL_W, LOGICAL_H, PAL_DARK);
    DrawRectangleLines(0, 0, LOGICAL_W, LOGICAL_H, PAL_MID);
    draw_help_line(3, STR("HOLD F1 PAUSED HELP"), PAL_MID);
    draw_help_line(9, STR("WASD MOVE"), PAL_LIGHT);
    draw_help_line(15, STR("MOUSE LOOK"), PAL_LIGHT);
    draw_help_line(21, STR("LMB FIRE"), PAL_LIGHT);
    draw_help_line(27, STR("RMB AIM"), PAL_LIGHT);
    draw_help_line(33, STR("SPACE JUMP"), PAL_LIGHT);
    draw_help_line(39, STR("SHIFT+W SPRINT"), PAL_LIGHT);
    draw_help_line(45, STR("E HOLD CUBE"), PAL_LIGHT);
    draw_help_line(51, STR("ENTER/T CHAT"), PAL_LIGHT);
    draw_help_line(57, STR("ESC MOUSE"), PAL_LIGHT);
  }
}

static void update_viewmodel(GameState *g, f32 dt) {
  if (dt > 0.05f)
    dt = 0.05f;
  // Local prediction steps the shared present_* helpers — the exact ones
  // the server runs per player for replication.
  u32 aim_e = local_player_entity(g);
  bool aim_reloading = aim_e != ECS_INVALID_ENTITY &&
                       weapon_is_reloading(g, aim_e);
  present_blend_tick(&g->aim_blend,
                     present_aim_want(g->input[0].buttons, aim_reloading),
                     12.0f, dt);
  present_walk_tick(&g->walk_phase, &g->walk_blend, g->input[0].move_x,
                    g->input[0].move_z, dt);
  present_lean_tick(&g->move_sway_x, &g->move_vel_x,
                    (f32)g->input[0].move_x, dt);
  present_lean_tick(&g->move_sway_z, &g->move_vel_z,
                    (f32)g->input[0].move_z, dt);
  present_blend_tick(&g->sprint_blend,
                     present_sprint_want(g->input[0].buttons,
                                         g->input[0].move_x,
                                         g->input[0].move_z),
                     8.0f, dt);
  // Screen shake decay (view-texture offset only, never the camera).
  g->screen_shake *= expf(-11.0f * dt);
  if (g->screen_shake < 0.02f)
    g->screen_shake = 0.0f;
  // Turn bob: side-side tuned down ~25%, steadier while aiming.
  f32 steady = 1.0f - g->aim_blend * 0.55f;
  f32 tx = g->look_delta_x * 0.048f * steady;
  f32 ty = -g->look_delta_y * 0.045f * steady;
  tx = fmaxf(-0.50f, fminf(0.50f, tx));
  ty = fmaxf(-0.45f, fminf(0.45f, ty));
  f32 k = 1.0f - expf(-5.2f * dt);
  g->gun_sway_x += (tx - g->gun_sway_x) * k;
  g->gun_sway_y += (ty - g->gun_sway_y) * k;
  g->gun_slide *= expf(-22.0f * dt);
  g->gun_recoil_velocity +=
      (-75.0f * g->gun_recoil - 15.0f * g->gun_recoil_velocity) * dt;
  g->gun_recoil += g->gun_recoil_velocity * dt;
  if (g->gun_recoil < 0.0f)
    g->gun_recoil = g->gun_recoil_velocity = 0.0f;
  if (g->gun_recoil > 0.081f)
    g->gun_recoil = 0.081f;
  // True recoil climb: separate upward spring for muzzle rise.
  g->gun_rise_velocity +=
      (-95.0f * g->gun_rise - 13.0f * g->gun_rise_velocity) * dt;
  g->gun_rise += g->gun_rise_velocity * dt;
  if (g->gun_rise < 0.0f)
    g->gun_rise = g->gun_rise_velocity = 0.0f;
  if (g->gun_rise > 0.0765f)
    g->gun_rise = 0.0765f;
}

static void render_frame(GameState *g, RenderTexture2D view,
                         RenderTexture2D ui, f32 a) {
  Camera3D cam = make_camera(g, a);
  u32 local_e = local_player_entity(g);
  u8 local_wid =
      local_e != ECS_INVALID_ENTITY ? g->weapon_id[local_e] : WEAPON_P99;
  const WeaponDef *wdef = weapon_def(local_wid);
  // View layer: world + viewmodel only. HUD lives on the ui layer so screen
  // shake never touches it.
  BeginTextureMode(view);
  ClearBackground(PAL_DARK);
  BeginMode3D(cam);
  DrawPlane((Vector3){0.0f, -0.02f, 0.0f}, (Vector2){32.0f, 32.0f}, PAL_PURPLE);
  draw_world(g, a);
  EndMode3D();
  Camera3D wcam = {.position = (Vector3){0.0f, 0.0f, 0.0f},
                   .target = (Vector3){0.0f, 0.0f, 1.0f},
                   .up = (Vector3){0.0f, 1.0f, 0.0f},
                   .fovy = cam.fovy,
                   .projection = CAMERA_PERSPECTIVE};
  BeginMode3D(wcam);
  rlPushMatrix();
  f32 rise = g->gun_rise;
  f32 b = g->aim_blend;
  if (b < 0.0f)
    b = 0.0f;
  if (b > 1.0f)
    b = 1.0f;
  f32 sway_scale = 1.0f - b * 0.75f;
  f32 reload_t =
      local_e != ECS_INVALID_ENTITY ? weapon_reload_progress(g, local_e) : 0.0f;
  // Per-gun ADS pose from the WeaponDef via the shared helper, so each
  // future weapon ships its own zoom view. Walk bob keeps its ADS damping.
  f32 base_x, base_y;
  present_ads_pos(wdef, b, &base_x, &base_y);
  // Walk feel: steady lean rides the spring position, swing rides its
  // velocity (always continuous, never a step). Shared helpers — the same
  // ones draw_remote_player uses for replicated state.
  f32 lean_x, lean_z;
  present_lean_offsets(g->move_sway_x, g->move_sway_z, &lean_x, &lean_z);
  f32 swing_yaw = g->move_vel_x * 1.2f * sway_scale;
  f32 swing_pitch = -g->move_vel_z * 0.5f * sway_scale;
  f32 swing_roll = -g->move_vel_x * 0.6f * sway_scale;
  f32 bob_x, bob_y, bob_z, bob_roll;
  present_bob_offsets(g->walk_phase, g->walk_blend, 1.0f - 0.5f * b, &bob_x,
                      &bob_y, &bob_z, &bob_roll);
  // Sprint pose: muzzle tilts skyward with the sprint blend (per-gun).
  f32 sprint_pitch = wdef->sprint_tilt_deg * g->sprint_blend;
  rlTranslatef(base_x + g->gun_sway_x * 1.02f * sway_scale + lean_x + bob_x,
               base_y + g->gun_sway_y * 1.28f * sway_scale +
                   rise * 0.70f * (1.0f - b * 0.8f) + bob_y,
               0.75f + lean_z + bob_z);
  rlRotatef(g->gun_sway_x * -52.0f * sway_scale + swing_yaw, 0.0f, 1.0f,
            0.0f);
  // Up/down rotation: direct pitch response plus a smaller cross-coupling
  // from turning so yaw motion also tilts the gun vertically.
  // Reload whips in place inside draw_gun (no dip/tilt here).
  // ads_pitch_deg re-zeroes the zoomed muzzle angle (negative lifts it).
  rlRotatef((g->gun_sway_y * -46.0f + g->gun_sway_x * -11.0f) * sway_scale -
                rise * 230.0f * (1.0f - b * 0.85f) + swing_pitch +
                wdef->ads_pitch_deg * b + sprint_pitch,
            1.0f, 0.0f, 0.0f);
  rlRotatef(g->gun_sway_x * -29.0f * sway_scale + wdef->ads_cant_deg * b +
                bob_roll + swing_roll,
            0.0f, 0.0f, 1.0f);
  draw_gun_local(local_wid, g->gun_recoil / 1.35f, g->gun_slide / 1.35f, 1.35f,
                 g->gun_kick != 0, reload_t);
  rlPopMatrix();
  EndMode3D();
  EndTextureMode();

  // UI layer: HUD only, transparent so the view shows through. Never shaken.
  BeginTextureMode(ui);
  ClearBackground(BLANK);
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
  Rectangle ui_dst = dst;
  // Screen shake on the view texture only, never the camera or UI layer.
  // Integer screen-pixel offsets only: fractional dst offsets fight the
  // point filter + integer scaling and read as blur.
  if (g->screen_shake > 0.01f) {
    int range = (int)(g->screen_shake * 2.0f + 0.5f);
    if (range > 0) {
      dst.x += (f32)GetRandomValue(-range, range);
      dst.y += (f32)GetRandomValue(-range, range);
    }
  }
  DrawTexturePro(view.texture, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
  DrawTexturePro(ui.texture, src, ui_dst, (Vector2){0, 0}, 0.0f, WHITE);
  EndDrawing();
}

static void client_predict_weapon(GameState *g) {
  u32 e = local_player_entity(g);
  if (e == ECS_INVALID_ENTITY)
    return;
  bool want = (g->input[0].buttons & BTN_FIRE) != 0;
  bool holding = weapon_holding(g, e, want);
  weapon_tick(g, e);
  if (weapon_can_fire(g, e, want)) {
    const WeaponDef *def = weapon_def(g->weapon_id[e]);
    u8 wid = g->weapon_id[e];
    weapon_on_fired(g, e, holding);
    g->gun_kick = 4;
    // Tap is soft, holding dumps the mag faster with a harder kick.
    g->gun_slide = holding ? def->slide_hold : def->slide_tap;
    g->gun_recoil_velocity += holding ? def->recoil_vel_hold : def->recoil_vel_tap;
    g->gun_rise_velocity += holding ? def->rise_vel_hold : def->rise_vel_tap;
    // View-texture shake (never the camera) on every shot.
    g->screen_shake = holding ? 1.0f : 0.7f;
    // True recoil: muzzle climbs, so push the aim up. Same step the server
    // applies to the authoritative pitch on confirmation.
    g->view_pitch = weapon_recoil_pitch(g->view_pitch, wid, holding);
  }
  weapon_set_prev(g, e, want);
}

static int client_run(String host, u16 port, String name) {
  GameState game;
  struct sockaddr_in server;
  int fd = open_udp_client(host, port, &server);
  double current = hires_time_in_seconds(), acc = 0.0;
  if (fd < 0) {
    fprintf(stderr, "host must be an IPv4 address or localhost\n");
    return 1;
  }
  client_init(&game);
  renew_client_session(&game);
  game.view_yaw = 180;
  for (i32 i = 0; i < PLAYER_NAME_LENGTH; ++i) {
    game.requested_name[i] = name.items[i];
    game.player_name[0][i] = name.items[i];
  }
  notice_set(&game, STR("LINKING"));
  game.mouse_captured = 1;
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
  InitWindow(920, 640, "voidryl");
  SetWindowMinSize(LOGICAL_W, LOGICAL_H);
  DisableCursor();
  RenderTexture2D view = LoadRenderTexture(LOGICAL_W, LOGICAL_H);
  SetTextureFilter(view.texture, TEXTURE_FILTER_POINT);
  RenderTexture2D ui = LoadRenderTexture(LOGICAL_W, LOGICAL_H);
  SetTextureFilter(ui.texture, TEXTURE_FILTER_POINT);
  while (!WindowShouldClose()) {
    double now = hires_time_in_seconds();
    double dt = now - current;
    if (dt > 0.25)
      dt = 0.25;
    current = now;
    update_input(&game, fd);
    if (game.help) {
      f32 frozen_alpha = (f32)game.interpolation_ticks / (f32)SNAPSHOT_INTERVAL;
      if (frozen_alpha > 1.0f)
        frozen_alpha = 1.0f;
      render_frame(&game, view, ui, frozen_alpha);
      continue;
    }
    acc += dt;
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
        notice_set(&game, STR("DROPPED"));
      }
      if (game.gun_kick > 0)
        --game.gun_kick;
      client_predict_weapon(&game);
      acc -= SIM_DT;
    }
    client_receive(&game, fd);
    update_viewmodel(&game, (f32)dt);
    f32 alpha = ((f32)game.interpolation_ticks + (f32)(acc / SIM_DT)) /
                (f32)SNAPSHOT_INTERVAL;
    if (alpha > 1.0f)
      alpha = 1.0f;
    render_frame(&game, view, ui, alpha);
  }
  client_send_input(&game, fd, NET_PACKET_DISCONNECT);
  EnableCursor();
  UnloadRenderTexture(view);
  UnloadRenderTexture(ui);
  CloseWindow();
  close(fd);
  return 0;
}

#endif
