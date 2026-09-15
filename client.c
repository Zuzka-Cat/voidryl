#ifndef CLIENT_C
#define CLIENT_C

#include "rlgl.h"
#include "shared.c"

#define PAL_DARK ((Color){16, 8, 24, 255})
#define PAL_PURPLE ((Color){60, 30, 102, 255})
#define PAL_MID ((Color){155, 123, 208, 255})
#define PAL_LIGHT ((Color){246, 213, 232, 255})

#define VIEWMODEL_SCALE 1.35f
#define P99_SIGHT_TIP ((Vector3){-0.32f, 0.098f, 0.48f})
#define OUTLINE_WIRE_EXPAND 1.04f
#define MARK_SPLAT_BASE 0.08f
#define MARK_SPLAT_STEP 0.04f
#define MARK_SPATTER_BASE 2
#define MARK_SPATTER_LIFE 12
#define MARK_SPATTER_LIFE_STEP 4
#define MARK_DRAW_NUDGE 0.008f
#define HELP_COL0_X 2
#define HELP_COL1_X (LOGICAL_W / 2 + 1)
#define HELP_TITLE_Y 3
#define HELP_ROWS_Y 11
#define HELP_ROW_DY 6
#define HELP_KEY_ADV 24
#define STAT_NUM_ADV 20
#define STAT_UPDATE_DT 0.1

static u32 local_player_entity(const GameState *g);

static int open_udp_client(String host, u16 port, struct sockaddr_in *server) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct in_addr address;
  if (fd < 0 || !parse_ipv4(host, &address)) {
    if (fd >= 0)
      close(fd);
    return -1;
  }
  *server = (struct sockaddr_in){
      .sin_family = AF_INET, .sin_addr = address, .sin_port = htons(port)};
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
  g->last_snapshot_ack = 0;
  g->snapshot_silence_ticks = 0;
  for (u32 e = 0; e < ECS_MAX_ENTITIES; ++e)
    g->weapon_dirty_seq[e] = 0;
  client_clear_world(g);
  g->gun_kick = 0;
  g->gun_recoil = g->gun_recoil_velocity = 0.0f;
  g->gun_rise = g->gun_rise_velocity = 0.0f;
  g->gun_slide = 0.0f;
  g->aim_blend = 0.0f;
  g->screen_shake = 0.0f;
  g->walk_phase = 0.0f;
  g->walk_blend = 0.0f;
  g->move_sway = (Vector2){0, 0};
  g->move_vel = (Vector2){0, 0};
  g->sprint_blend = 0.0f;
  memset(g->ping.slot_valid, 0, sizeof(g->ping.slot_valid));
  g->ping.rtt_ms = 0.0;
  g->ping.have = 0;
}

static u32 client_entity_for(GameState *g, u8 server_entity, u8 kind) {
  if (server_entity >= ECS_MAX_ENTITIES)
    return ECS_INVALID_ENTITY;
  u8 local = g->network_entity[server_entity];
  if (local != NET_PACKET_NONE && ecs_alive(&g->ecs, local) &&
      ecs_has(&g->ecs, local, COMP_NETWORKED) && g->kind[local] == kind)
    return local;
  if (local != NET_PACKET_NONE && ecs_alive(&g->ecs, local))
    ecs_destroy(&g->ecs, local);
  u32 e = ecs_create(&g->ecs);
  if (e == ECS_INVALID_ENTITY || e >= ECS_MAX_ENTITIES)
    return ECS_INVALID_ENTITY;
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
  g->last_snapshot_ack = snap->acknowledged_sequence;
  g->connected = 1;
  g->snapshot_silence_ticks = 0;
  for (u8 i = 0; i < snap->count; ++i) {
    const NetEntity *w = &snap->entities[i];
    if (w->entity >= ECS_MAX_ENTITIES || w->kind == 0 || w->kind > KIND_FLOOR)
      continue;
    if (w->owner >= MAX_PLAYERS)
      continue;
    g->snapshot_seen |= UINT32_C(1) << w->entity;
    u8 mapped = g->network_entity[w->entity];
    bool had = mapped != NET_PACKET_NONE && ecs_alive(&g->ecs, mapped) &&
               g->kind[mapped] == w->kind;
    u32 e = client_entity_for(g, w->entity, w->kind);
    if (e == ECS_INVALID_ENTITY)
      continue;
    i16 nx = w->x, ny = w->y, nz = w->z;
    i16 dx = (i16)(nx - g->pos[e].x);
    i16 dy = (i16)(ny - g->pos[e].y);
    i16 dz = (i16)(nz - g->pos[e].z);
    bool snap_pos =
        !had || abs(dx) > 240 || abs(dz) > 240 || dy > 48 || dy < -240;
    g->vel[e].x = snap_pos ? 0 : dx;
    g->vel[e].y = snap_pos ? 0 : dy;
    g->vel[e].z = snap_pos ? 0 : dz;
    if (had && w->kind == KIND_PLAYER) {
      g->lag[e].yaw = lag_encode(wrap_degrees(w->yaw - g->aim[e].yaw));
      g->lag[e].pitch = lag_encode((int)w->pitch - (int)g->aim[e].pitch);
    } else if (w->kind == KIND_PLAYER) {
      g->lag[e] = (LagAim){LAG_NEUTRAL, LAG_NEUTRAL};
    }
    g->kind[e] = w->kind;
    g->owner[e] = w->owner;
    g->pos[e] = (IVec3){nx, ny, nz};
    g->aim[e] = (Aim){w->yaw, w->pitch};
    g->flash[e] = w->flags;
    g->held_by[e] = w->extra;
    if (w->kind == KIND_CUBE)
      g->scale[e] = scale_decode(w->scale);
    if (w->kind == KIND_CASING) {
      u8 sid = w->weapon < WEAPON_COUNT ? w->weapon : WEAPON_P99;
      g->weapon_id[e] = sid;
    }
    if (w->kind == KIND_PLAYER) {
      if (!had) {
        weapon_set_from_snapshot(g, e, w->weapon, w->ammo, w->reloading != 0);
        g->weapon_dirty_seq[e] = snap->acknowledged_sequence;
      } else {
        weapon_apply_snapshot(g, e, w->weapon, w->ammo, w->reloading != 0,
                              w->reload_q, snap->acknowledged_sequence);
      }
      g->r_reload[e] = w->reload_q;
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
  u8 buf[NET_MAX_DATAGRAM];
  for (;;) {
    ssize_t raw = recv(fd, buf, sizeof(buf), 0);
    if (raw < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        if (g->connected)
          notice_set(g, STR("DROPPED"));
        renew_client_session(g);
      }
      return;
    }
    if (raw == 0 || raw > (ssize_t)sizeof(buf))
      continue;
    i32 n = (i32)raw;
    NetChatPacket chat;
    if (net_chat_decode(buf, n, &chat)) {
      String wname = {(u8 *)chat.name, PLAYER_NAME_LENGTH, PLAYER_NAME_LENGTH};
      if (chat.session != g->session)
        continue;
      if (!net_validate_chat_fields(wname, chat.length, chat.sequence,
                                    g->got_chat_sequence != 0,
                                    g->last_chat_sequence))
        continue;
      g->got_chat_sequence = 1;
      g->last_chat_sequence = chat.sequence;
      String text = {chat.text, chat.length, CHAT_LENGTH};
      notice_chat_msg(g, wname, text);
      continue;
    }
    NetSnapshot snap;
    if (!net_snapshot_decode(buf, n, &snap))
      continue;
    if (!net_validate_snapshot_fields(&snap, g->session, g->got_snapshot != 0,
                                      g->last_snapshot_tick))
      continue;
    {
      u16 ack = snap.acknowledged_sequence;
      u32 slot = (u32)ack & PING_WINDOW_MASK;
      if (g->ping.slot_valid[slot] && g->ping.send_seq[slot] == ack) {
        double now = hires_time_in_seconds();
        double sample = (now - g->ping.send_time[slot]) * PING_MS_PER_SEC;
        if (sample < 0.0)
          sample = 0.0;
        if (sample > PING_MAX_MS)
          sample = PING_MAX_MS;
        if (!g->ping.have) {
          g->ping.rtt_ms = sample;
          g->ping.have = 1;
        } else {
          g->ping.rtt_ms = g->ping.rtt_ms * (1.0 - PING_SMOOTH_NEW) +
                           sample * PING_SMOOTH_NEW;
        }
      }
    }
    g->got_snapshot = 1;
    apply_snapshot(g, &snap);
  }
}

static void client_send_input(GameState *g, int fd, u8 type) {
  NetInputPacket packet = {
      .magic = NET_MAGIC,
      .type = type,
      .claimed_player = g->local_player,
      .session = g->session,
  };
  String req = game_requested_name(g);
  memcpy(packet.name, req.items, PLAYER_NAME_LENGTH);
  if (type == NET_PACKET_INPUT) {
    packet.buttons = g->input[0].buttons & BTN_ALL;
    packet.sequence = g->next_sequence++;
    packet.move_x = g->input[0].move_x;
    packet.move_z = g->input[0].move_z;
    packet.aim_yaw = g->view.yaw;
    packet.aim_pitch = g->view.pitch;
    u32 slot = (u32)packet.sequence & PING_WINDOW_MASK;
    g->ping.send_time[slot] = hires_time_in_seconds();
    g->ping.send_seq[slot] = packet.sequence;
    g->ping.slot_valid[slot] = 1;
  }
  (void)send(fd, &packet, sizeof(packet), 0);
}

static void client_send_chat(GameState *g, int fd) {
  if (g->chat_length > CHAT_LENGTH)
    return;
  NetChatPacket packet = {
      .magic = NET_MAGIC,
      .type = NET_PACKET_CHAT,
      .player = g->local_player,
      .length = g->chat_length,
      .session = g->session,
      .sequence = g->next_chat_sequence++,
  };
  String req = game_requested_name(g);
  String chat = game_chat_input(g);
  memcpy(packet.name, req.items, PLAYER_NAME_LENGTH);
  if (chat.length > 0)
    memcpy(packet.text, chat.items, (size_t)chat.length);
  (void)send(fd, &packet, sizeof(packet), 0);
}

static void update_input(GameState *g, int fd) {
  g->look_delta = (Vector2){0, 0};
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
  u32 sens_e = local_player_entity(g);
  u8 sens_wid =
      sens_e != ECS_INVALID_ENTITY ? g->weapon_id[sens_e] : WEAPON_P99;
  f32 sens = aiming ? weapon_aim_sens_scale(sens_wid) : 1.0f;
  g->look_delta.x = mouse.x * sens;
  g->look_delta.y = mouse.y * sens;
  g->view.yaw = wrap_degrees((int)g->view.yaw - (int)lroundf(g->look_delta.x));
  g->view.pitch = (i16)clamp_int(
      (int)g->view.pitch - (int)lroundf(g->look_delta.y), -75, 75);
}

static u32 local_player_entity(const GameState *g) {
  u32 comps[] = {COMP_PLAYER, COMP_POSITION};
  EcsQuery q = ecs_query(&g->ecs, comps, 2);
  while (ecs_next(&q))
    if (g->owner[q.entity] == g->local_player)
      return q.entity;
  return ECS_INVALID_ENTITY;
}

static Camera3D make_camera(const GameState *g, f32 a) {
  u32 e = local_player_entity(g);
  Vector3 dir = {0.0f, 0.0f, 1.0f};
  u8 wid = e != ECS_INVALID_ENTITY ? g->weapon_id[e] : WEAPON_P99;
  f32 hip_fov = weapon_hip_fov(wid);
  f32 aim_fov = weapon_aim_fov(wid);
  Camera3D cam = {.position = (Vector3){0.0f, EYE_HEIGHT_M, 0.0f},
                  .up = (Vector3){0.0f, 1.0f, 0.0f},
                  .fovy = hip_fov,
                  .projection = CAMERA_PERSPECTIVE};
  if (e != ECS_INVALID_ENTITY) {
    Vector3 p = entity_pos_m(g, e, a);
    cam.position = player_eye_m(p);
    dir = sight_dir((f32)g->view.yaw, (f32)g->view.pitch);
  }
  cam.fovy = hip_fov * (1.0f - g->aim_blend) + aim_fov * g->aim_blend;
  cam.target = vec3_add(cam.position, dir);
  return cam;
}

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

static void ads_target_for_tip(const WeaponDef *def, Vector3 tip, f32 scale,
                               f32 *tx, f32 *ty) {
  f32 cr = cosf(def->ads_cant_deg * DEG2RAD);
  f32 sr = sinf(def->ads_cant_deg * DEG2RAD);
  f32 cp = cosf(def->ads_pitch_deg * DEG2RAD);
  f32 sp = sinf(def->ads_pitch_deg * DEG2RAD);
  f32 x0 = tip.x * scale, y0 = tip.y * scale, z0 = tip.z * scale;
  f32 x1 = x0 * cr - y0 * sr;
  f32 y1 = x0 * sr + y0 * cr;
  f32 y2 = y1 * cp - z0 * sp;
  *tx = -x1;
  *ty = -y2;
}

static void gun_ads_target(u8 wid, const WeaponDef *def, f32 *tx, f32 *ty) {
  switch (wid) {
  case WEAPON_P99:
    ads_target_for_tip(def, P99_SIGHT_TIP, VIEWMODEL_SCALE, tx, ty);
    break;
  default:
    *tx = def->hip_off.x;
    *ty = def->hip_off.y;
    break;
  }
}

static void draw_box_wires(Vector3 p, Vector3 s, Color wire) {
  DrawCubeWiresV(p,
                 (Vector3){s.x * OUTLINE_WIRE_EXPAND, s.y * OUTLINE_WIRE_EXPAND,
                           s.z * OUTLINE_WIRE_EXPAND},
                 wire);
}

static void draw_gun_p99(f32 recoil, f32 slide, f32 scale, bool firing,
                         f32 reload_t) {
  rlPushMatrix();
  rlScalef(scale, scale, scale);
  gun_reload_pose_p99(reload_t);
  f32 slide_off = recoil + slide;
  f32 frame_off = recoil * 0.6f;
  f32 grip_off = recoil * 0.5f;
  Vector3 sl = {-0.32f, 0.02f, 0.30f - slide_off};
  Vector3 sls = {0.14f, 0.10f, 0.48f};
  DrawCubeV(sl, sls, PAL_MID);
  draw_box_wires(sl, sls, PAL_DARK);
  Vector3 fr = {-0.32f, -0.055f, 0.275f - frame_off};
  Vector3 frs = {0.14f, 0.05f, 0.43f};
  DrawCubeV(fr, frs, PAL_PURPLE);
  draw_box_wires(fr, frs, PAL_DARK);
  Vector3 gr = {-0.32f, -0.20f, 0.14f - grip_off};
  Vector3 grs = {0.10f, 0.28f, 0.13f};
  DrawCubeV(gr, grs, PAL_PURPLE);
  draw_box_wires(gr, grs, PAL_DARK);
  Vector3 gb = {-0.32f, -0.125f, 0.30f - frame_off};
  DrawCubeV(gb, (Vector3){0.02f, 0.015f, 0.22f}, PAL_DARK);
  Vector3 gf = {-0.32f, -0.095f, 0.40f - frame_off};
  DrawCubeV(gf, (Vector3){0.02f, 0.06f, 0.02f}, PAL_DARK);
  Vector3 grr = {-0.32f, -0.095f, 0.20f - frame_off};
  DrawCubeV(grr, (Vector3){0.02f, 0.06f, 0.02f}, PAL_DARK);
  DrawCubeV((Vector3){-0.32f, -0.085f, 0.28f - frame_off},
            (Vector3){0.018f, 0.05f, 0.018f}, PAL_DARK);
  DrawCubeV((Vector3){-0.385f, 0.045f, 0.18f - slide_off},
            (Vector3){0.012f, 0.025f, 0.14f}, PAL_DARK);
  DrawCubeV((Vector3){-0.32f, 0.02f, 0.10f - slide_off},
            (Vector3){0.145f, 0.06f, 0.015f}, PAL_DARK);
  DrawCubeV((Vector3){-0.32f, 0.02f, 0.135f - slide_off},
            (Vector3){0.145f, 0.06f, 0.015f}, PAL_DARK);
  f32 sz = -slide_off;
  Vector3 rss = {0.022f, 0.028f, 0.04f};
  Vector3 rl = {-0.375f, 0.080f, 0.09f + sz};
  Vector3 rr = {-0.265f, 0.080f, 0.09f + sz};
  DrawCubeV(rl, rss, PAL_DARK);
  draw_box_wires(rl, rss, PAL_MID);
  DrawCubeV(rr, rss, PAL_DARK);
  draw_box_wires(rr, rss, PAL_MID);
  Vector3 fs = {-0.32f, 0.082f, 0.48f + sz};
  Vector3 fss = {0.022f, 0.032f, 0.022f};
  DrawCubeV(fs, fss, PAL_DARK);
  draw_box_wires(fs, fss, PAL_MID);
  if (firing)
    DrawCylinderEx((Vector3){-0.32f, 0.0f, 0.56f - frame_off},
                   (Vector3){-0.32f, 0.0f, 0.72f - frame_off}, 0.065f, 0.0f, 5,
                   PAL_LIGHT);
  rlPopMatrix();
}

static void draw_gun_fallback(f32 recoil, f32 slide, f32 scale, bool firing) {
  (void)firing;
  rlPushMatrix();
  rlScalef(scale, scale, scale);
  Vector3 sl = {-0.32f, 0.0f, 0.30f - recoil - slide};
  DrawCubeV(sl, (Vector3){0.16f, 0.13f, 0.46f}, PAL_MID);
  draw_box_wires(sl, (Vector3){0.16f, 0.13f, 0.46f}, PAL_DARK);
  Vector3 gr = {-0.32f, -0.15f, 0.18f - recoil};
  DrawCubeV(gr, (Vector3){0.13f, 0.26f, 0.15f}, PAL_PURPLE);
  draw_box_wires(gr, (Vector3){0.13f, 0.26f, 0.15f}, PAL_DARK);
  rlPopMatrix();
}

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
  draw_box_wires(p, s, PAL_DARK);
}

static void draw_remote_player(const GameState *g, u32 e, Vector3 pos, f32 a) {
  bool hurt = g->flash[e] > PLAYER_FIRE_FLASH_TICKS;
  bool firing = g->flash[e] > 0 && g->flash[e] <= PLAYER_FIRE_FLASH_TICKS;
  f32 fire = firing ? (f32)g->flash[e] / (f32)PLAYER_FIRE_FLASH_TICKS : 0.0f;
  f32 yaw_r = present_lerp_angle(g->aim[e].yaw, lag_decode(g->lag[e].yaw), a);
  f32 pitch_r =
      present_lerp_angle(g->aim[e].pitch, lag_decode(g->lag[e].pitch), a);
  f32 reload_t =
      g->r_reload[e] == 0 ? 0.0f : (f32)(g->r_reload[e] - 1) / 254.0f;
  Color body = hurt ? PAL_LIGHT : PAL_MID;
  rlPushMatrix();
  rlTranslatef(pos.x, pos.y, pos.z);
  rlRotatef(yaw_r, 0.0f, 1.0f, 0.0f);
  draw_cube_lit((Vector3){0.0f, PLAYER_BODY_Y * 0.5f, 0.0f},
                (Vector3){PLAYER_BODY_X, PLAYER_BODY_Y, PLAYER_BODY_Z}, body);
  draw_cube_lit((Vector3){0.0f, PLAYER_HEAD_Y, 0.06f},
                (Vector3){PLAYER_HEAD_S, PLAYER_HEAD_S, PLAYER_HEAD_S},
                hurt ? PAL_LIGHT : PAL_PURPLE);
  rlPopMatrix();
  GunPose gp = player_gun_pose(g, e, pos, yaw_r, pitch_r, a);
  rlPushMatrix();
  rlTranslatef(gp.origin.x, gp.origin.y, gp.origin.z);
  rlRotatef(gp.yaw, 0.0f, 1.0f, 0.0f);
  rlRotatef(-gp.pitch, 1.0f, 0.0f, 0.0f);
  rlTranslatef(GUN_CENTER_X_M, 0.0f, 0.0f);
  draw_gun_local(g->weapon_id[e], fire * 0.03f, fire * 0.08f, 1.0f, firing,
                 reload_t);
  rlPopMatrix();
}

static Color shell_pal_color(u8 idx) {
  switch (idx) {
  case 0:
    return PAL_DARK;
  case 1:
    return PAL_PURPLE;
  case 2:
    return PAL_MID;
  default:
    return PAL_LIGHT;
  }
}

static void draw_casing(const GameState *g, u32 e, Vector3 p) {
  u8 sid = g->weapon_id[e] < WEAPON_COUNT ? g->weapon_id[e] : WEAPON_P99;
  const ShellDef *sh = shell_def(sid);
  f32 half = sh->len * 0.5f;
  f32 rad = sh->radius;
  f32 rim = sh->rim_radius;
  Color body = shell_pal_color(sh->body_pal);
  Color rimc = shell_pal_color(sh->rim_pal);
  f32 roll = (f32)g->held_by[e] * (360.0f / 256.0f);
  rlPushMatrix();
  rlTranslatef(p.x, p.y, p.z);
  rlRotatef((f32)g->aim[e].yaw, 0.0f, 1.0f, 0.0f);
  rlRotatef((f32)g->aim[e].pitch, 1.0f, 0.0f, 0.0f);
  rlRotatef(roll, 0.0f, 0.0f, 1.0f);
  DrawCylinderEx((Vector3){0.0f, -half * 1.06f, 0.0f},
                 (Vector3){0.0f, half * 1.06f, 0.0f}, rim * 1.08f, rad * 1.08f,
                 6, PAL_DARK);
  DrawCylinderEx((Vector3){0.0f, -half, 0.0f}, (Vector3){0.0f, half, 0.0f}, rad,
                 rad * 0.92f, 6, body);
  DrawCylinderEx((Vector3){0.0f, -half * 1.06f, 0.0f},
                 (Vector3){0.0f, -half * 0.72f, 0.0f}, rim, rim, 6, rimc);
  rlPopMatrix();
}

static void mark_basis(Vector3 n, Vector3 *u, Vector3 *v) {
  if (n.y != 0.0f) {
    *u = (Vector3){1.0f, 0.0f, 0.0f};
    *v = (Vector3){0.0f, 0.0f, 1.0f};
  } else if (n.x != 0.0f) {
    *u = (Vector3){0.0f, 0.0f, 1.0f};
    *v = (Vector3){0.0f, 1.0f, 0.0f};
  } else {
    *u = (Vector3){1.0f, 0.0f, 0.0f};
    *v = (Vector3){0.0f, 1.0f, 0.0f};
  }
}

static void draw_mark_quad(Vector3 c, Vector3 u, Vector3 v, f32 size) {
  f32 h = size * 0.5f;
  Vector3 a = {c.x + (u.x + v.x) * h, c.y + (u.y + v.y) * h,
               c.z + (u.z + v.z) * h};
  Vector3 b = {c.x + (u.x - v.x) * h, c.y + (u.y - v.y) * h,
               c.z + (u.z - v.z) * h};
  Vector3 cc = {c.x - (u.x + v.x) * h, c.y - (u.y + v.y) * h,
                c.z - (u.z + v.z) * h};
  Vector3 d = {c.x - (u.x - v.x) * h, c.y - (u.y - v.y) * h,
               c.z - (u.z - v.z) * h};
  DrawTriangle3D(a, b, cc, PAL_DARK);
  DrawTriangle3D(a, cc, d, PAL_DARK);
  DrawTriangle3D(a, cc, b, PAL_DARK);
  DrawTriangle3D(a, d, cc, PAL_DARK);
}

static void reflect_basis(Vector3 r, Vector3 *u, Vector3 *v) {
  Vector3 ref = fabsf(r.y) < 0.9f ? (Vector3){0.0f, 1.0f, 0.0f}
                                  : (Vector3){1.0f, 0.0f, 0.0f};
  *u = vec3_norm(vec3_cross(r, ref));
  *v = vec3_cross(r, *u);
}

static void draw_impact_mark(const GameState *g, u32 e, Vector3 p) {
  u8 ex = g->held_by[e];
  Vector3 n = mark_normal_vec(mark_ex_norm(ex));
  u8 str = mark_ex_str(ex);
  if (str >= MARK_STR_LEVELS)
    str = MARK_STR_LEVELS - 1u;
  Color mat = mark_ex_mat(ex) == MARK_MAT_MID ? PAL_MID : PAL_PURPLE;
  Vector3 u, v;
  mark_basis(n, &u, &v);
  Vector3 c = vec3_add(p, vec3_mul(n, MARK_DRAW_NUDGE));
  u32 seed = mix32((u32)e * 2654435761u + 7u);
  f32 ang = (f32)(seed % 360u) * DEG2RAD;
  f32 ca = cosf(ang), sa = sinf(ang);
  Vector3 ru = {u.x * ca + v.x * sa, u.y * ca + v.y * sa, u.z * ca + v.z * sa};
  Vector3 rv = {v.x * ca - u.x * sa, v.y * ca - u.y * sa, v.z * ca - u.z * sa};
  draw_mark_quad(c, ru, rv, MARK_SPLAT_BASE + MARK_SPLAT_STEP * (f32)str);
  Vector3 d;
  aim_dir(g->aim[e].yaw, g->aim[e].pitch, &d);
  float dn = vec3_dot(d, n);
  Vector3 r = vec3_sub(d, vec3_mul(n, 2.0f * dn));
  Vector3 cu, cv;
  reflect_basis(r, &cu, &cv);
  int age = (IMPACT_MARK_LIFETIME_TICKS - (int)g->lifetime[e]) * 2;
  int life = MARK_SPATTER_LIFE + MARK_SPATTER_LIFE_STEP * (int)str;
  if (age >= 0 && age < life) {
    f32 t = (f32)age * SIM_DT;
    f32 fade = 1.0f - (f32)age / (f32)life;
    int count = MARK_SPATTER_BASE + (int)str;
    for (int i = 0; i < count; ++i) {
      u32 hh = mix32(seed + (u32)i * 2246822519u);
      f32 aa = ((f32)(hh % 100u) / 100.0f - 0.5f) * 0.9f;
      f32 bb = ((f32)((hh >> 7) % 100u) / 100.0f - 0.5f) * 0.9f;
      Vector3 dir =
          vec3_norm(vec3_add(r, vec3_add(vec3_mul(cu, aa), vec3_mul(cv, bb))));
      f32 sp = 1.0f + (f32)((hh >> 14) % 100u) / 100.0f * 2.0f;
      Vector3 dp = {c.x + dir.x * sp * t, c.y + dir.y * sp * t - 4.9f * t * t,
                    c.z + dir.z * sp * t};
      f32 s = (0.030f + 0.010f * (f32)((hh >> 21) & 3u)) * fade + 0.008f;
      DrawCubeV(dp, (Vector3){s, s, s}, mat);
    }
  }
}

static void draw_world(const GameState *g, f32 a) {
  u32 comps[] = {COMP_POSITION, COMP_RENDERABLE};
  EcsQuery q = ecs_query(&g->ecs, comps, 2);
  while (ecs_next(&q)) {
    u32 e = q.entity;
    Vector3 p = entity_pos_m(g, e, a);
    switch (g->kind[e]) {
    case KIND_CUBE: {
      f32 s = scale_value(g, e);
      draw_cube_lit(p,
                    (Vector3){CUBE_VIS_X * s, CUBE_VIS_Y * s, CUBE_VIS_Z * s},
                    g->held_by[e] ? PAL_LIGHT : PAL_MID);
      break;
    }
    case KIND_PLAYER:
      if (g->owner[e] != g->local_player)
        draw_remote_player(g, e, p, a);
      break;
    case KIND_BULLET: {
      Vector3 dir = sight_dir((f32)g->aim[e].yaw, (f32)g->aim[e].pitch);
      Vector3 shoulder = vec3_sub(p, vec3_mul(dir, 0.20f));
      Vector3 base = vec3_sub(shoulder, vec3_mul(dir, 0.34f));
      DrawCylinderEx(base, shoulder, 0.040f, 0.040f, 7, PAL_DARK);
      DrawCylinderEx(base, shoulder, 0.032f, 0.032f, 7, PAL_MID);
      DrawCylinderEx(shoulder, p, 0.032f, 0.0f, 7, PAL_LIGHT);
      break;
    }
    case KIND_CASING:
      draw_casing(g, e, p);
      break;
    case KIND_IMPACT_MARK:
      draw_impact_mark(g, e, p);
      break;
    case KIND_FLOOR:
      break;
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
  int blink = ((int)(GetTime() * 6.0)) & 1;
  return blink ? PAL_MID : PAL_PURPLE;
}

static int small_text_width(String text) {
  return text.length ? (int)text.length * 4 - 1 : 0;
}

static void draw_hit_indicator(const GameState *g, u32 e) {
  if (g->flash[e] <= PLAYER_FIRE_FLASH_TICKS)
    return;
  int rel = wrap_degrees(unpack_yaw(g->held_by[e]) - g->view.yaw);
  f32 r = (f32)rel * DEG2RAD;
  f32 s = sinf(r);
  f32 f = cosf(r);
  Color c = g->flash[e] > PLAYER_HIT_FLASH_TICKS / 2 ? PAL_LIGHT : PAL_MID;
  int cx = LOGICAL_W / 2, cy = LOGICAL_H / 2;
  int len = g->flash[e] > 12 ? 14 : 10;
  if (s < -0.20f) {
    int y = clamp_int(cy - (int)(f * 18.0f) - len / 2, 2, LOGICAL_H - len - 2);
    DrawRectangle(0, y, 3, len, c);
    DrawRectangle(3, y + len / 2 - 2, 2, 4, c);
  }
  if (s > 0.20f) {
    int y = clamp_int(cy - (int)(f * 18.0f) - len / 2, 2, LOGICAL_H - len - 2);
    DrawRectangle(LOGICAL_W - 3, y, 3, len, c);
    DrawRectangle(LOGICAL_W - 5, y + len / 2 - 2, 2, 4, c);
  }
  if (f > 0.50f) {
    int x = clamp_int(cx + (int)(s * 22.0f) - len / 2, 2, LOGICAL_W - len - 2);
    DrawRectangle(x, 0, len, 3, c);
  }
  if (f < -0.50f) {
    int x = clamp_int(cx + (int)(s * 22.0f) - len / 2, 2, LOGICAL_W - len - 2);
    DrawRectangle(x, LOGICAL_H - 3, len, 3, c);
  }
}

typedef struct HelpEntry {
  const char *key;
  const char *action;
} HelpEntry;

static const HelpEntry HELP_ENTRIES[] = {
    {"WASD", "MOVE"},  {"MOUSE", "LOOK"}, {"LMB", "FIRE"},  {"RMB", "AIM"},
    {"SPACE", "JUMP"}, {"SHIFT", "RUN"},  {"E", "GRAB"},    {"ENTER", "CHAT"},
    {"T", "CHAT"},     {"ESC", "MOUSE"},  {"TAB", "STATS"}, {"F1", "HELP"},
};
#define HELP_COUNT (sizeof(HELP_ENTRIES) / sizeof(HELP_ENTRIES[0]))

static void draw_help_entry(int x, int y, const char *key, const char *action) {
  i32 klen = cstr_len(key);
  i32 alen = cstr_len(action);
  String ks = {(u8 *)key, klen, klen};
  String as = {(u8 *)action, alen, alen};
  draw_small_text(x, y, ks, PAL_MID);
  draw_small_text(x + HELP_KEY_ADV, y, as, PAL_LIGHT);
}

static void draw_hud(GameState *g) {
  u32 e = local_player_entity(g);
  if (!g->chatting && g->notice_ticks && g->notice_length) {
    String note = game_notice(g);
    int w = small_text_width(note);
    DrawRectangle(1, 1, w + 2, 7, PAL_DARK);
    draw_small_text(2, 2, note, PAL_LIGHT);
  }
  if (e != ECS_INVALID_ENTITY)
    draw_hit_indicator(g, e);
  if (g->aim_blend < 0.5f)
    DrawRectangle(LOGICAL_W / 2, LOGICAL_H / 2, 1, 1, PAL_LIGHT);
  if (e != ECS_INVALID_ENTITY && g->connected) {
    bool reloading = weapon_is_reloading(g, e);
    unsigned ammo = weapon_ammo(g, e);
    unsigned mag = weapon_mag_size(g->weapon_id[e]);
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
    draw_small_text(2, 2, STR("T"), PAL_MID);
    draw_small_text(10, 2, chat, PAL_LIGHT);
  }
  if (IsKeyDown(KEY_TAB)) {
    static unsigned shown_fps, shown_ping;
    static double last_stat_update = -1.0;
    double now = GetTime();
    if (now - last_stat_update >= STAT_UPDATE_DT) {
      shown_fps = (unsigned)GetFPS();
      shown_ping =
          (g->connected && g->ping.have) ? (unsigned)(g->ping.rtt_ms + 0.5) : 0;
      last_stat_update = now;
    }
    unsigned fps = shown_fps;
    unsigned ping = shown_ping;
    unsigned fps_copy = fps;
    int fps_digits = 1;
    while (fps_copy >= 10u) {
      fps_copy /= 10u;
      ++fps_digits;
    }
    unsigned ping_copy = ping;
    int ping_digits = 1;
    while (ping_copy >= 10u) {
      ping_copy /= 10u;
      ++ping_digits;
    }
    int fps_w = STAT_NUM_ADV - 1 + fps_digits * 4;
    int ping_w = STAT_NUM_ADV - 1 + ping_digits * 4;
    DrawRectangle(1, 1, fps_w + 2, 7, PAL_DARK);
    draw_small_text(2, 2, STR("FPS"), PAL_MID);
    draw_number(2 + STAT_NUM_ADV, 2, fps, PAL_LIGHT);
    DrawRectangle(1, 9, ping_w + 2, 7, PAL_DARK);
    draw_small_text(2, 10, STR("PING"), PAL_MID);
    draw_number(2 + STAT_NUM_ADV, 10, ping, PAL_LIGHT);
  }
  if (g->help) {
    DrawRectangle(0, 0, LOGICAL_W, LOGICAL_H, PAL_DARK);
    DrawRectangleLines(0, 0, LOGICAL_W, LOGICAL_H, PAL_MID);
    draw_small_text(HELP_COL0_X, HELP_TITLE_Y, STR("HELP"), PAL_MID);
    for (u32 i = 0; i < HELP_COUNT; i += 2) {
      int y = HELP_ROWS_Y + (int)(i / 2u) * HELP_ROW_DY;
      draw_help_entry(HELP_COL0_X, y, HELP_ENTRIES[i].key,
                      HELP_ENTRIES[i].action);
      if (i + 1u < HELP_COUNT)
        draw_help_entry(HELP_COL1_X, y, HELP_ENTRIES[i + 1u].key,
                        HELP_ENTRIES[i + 1u].action);
    }
  }
}

static void update_viewmodel(GameState *g, f32 dt) {
  if (dt > 0.05f)
    dt = 0.05f;
  u32 aim_e = local_player_entity(g);
  bool aim_reloading =
      aim_e != ECS_INVALID_ENTITY && weapon_is_reloading(g, aim_e);
  present_blend_tick(&g->aim_blend,
                     present_aim_want(g->input[0].buttons, aim_reloading),
                     12.0f, dt);
  present_walk_tick(&g->walk_phase, &g->walk_blend, g->input[0].move_x,
                    g->input[0].move_z, dt);
  present_lean_tick(&g->move_sway.x, &g->move_vel.x, (f32)g->input[0].move_x,
                    dt);
  present_lean_tick(&g->move_sway.y, &g->move_vel.y, (f32)g->input[0].move_z,
                    dt);
  present_blend_tick(&g->sprint_blend,
                     present_sprint_want(g->input[0].buttons,
                                         g->input[0].move_x,
                                         g->input[0].move_z),
                     8.0f, dt);
  g->screen_shake *= expf(-11.0f * dt);
  if (g->screen_shake < 0.02f)
    g->screen_shake = 0.0f;
  f32 steady = 1.0f - g->aim_blend * 0.55f;
  f32 tx = g->look_delta.x * 0.048f * steady;
  f32 ty = -g->look_delta.y * 0.045f * steady;
  tx = clamp_float(tx, -0.50f, 0.50f);
  ty = clamp_float(ty, -0.45f, 0.45f);
  f32 k = 1.0f - expf(-5.2f * dt);
  g->gun_sway.x += (tx - g->gun_sway.x) * k;
  g->gun_sway.y += (ty - g->gun_sway.y) * k;
  g->gun_slide *= expf(-26.0f * dt);
  g->gun_recoil_velocity +=
      (-140.0f * g->gun_recoil - 13.0f * g->gun_recoil_velocity) * dt;
  g->gun_recoil += g->gun_recoil_velocity * dt;
  g->gun_recoil = clamp_float(g->gun_recoil, -0.02f, 0.09f);
  if (fabsf(g->gun_recoil) < 0.0005f && fabsf(g->gun_recoil_velocity) < 0.005f)
    g->gun_recoil = g->gun_recoil_velocity = 0.0f;
  g->gun_rise_velocity +=
      (-70.0f * g->gun_rise - 9.0f * g->gun_rise_velocity) * dt;
  g->gun_rise += g->gun_rise_velocity * dt;
  g->gun_rise = clamp_float(g->gun_rise, -0.018f, 0.085f);
  if (fabsf(g->gun_rise) < 0.0005f && fabsf(g->gun_rise_velocity) < 0.005f)
    g->gun_rise = g->gun_rise_velocity = 0.0f;
}

static void render_frame(GameState *g, RenderTexture2D view, RenderTexture2D ui,
                         f32 a) {
  Camera3D cam = make_camera(g, a);
  u32 local_e = local_player_entity(g);
  u8 local_wid =
      local_e != ECS_INVALID_ENTITY ? g->weapon_id[local_e] : WEAPON_P99;
  const WeaponDef *wdef = weapon_def(local_wid);
  BeginTextureMode(view);
  ClearBackground(PAL_DARK);
  BeginMode3D(cam);
  DrawPlane((Vector3){0.0f, 0.0f, 0.0f}, (Vector2){32.0f, 32.0f}, PAL_PURPLE);
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
  f32 b = clamp_float(g->aim_blend, 0.0f, 1.0f);
  f32 sway_scale = 1.0f - b * 0.75f;
  f32 reload_t =
      local_e != ECS_INVALID_ENTITY ? weapon_reload_progress(g, local_e) : 0.0f;
  f32 ads_tx, ads_ty;
  gun_ads_target(local_wid, wdef, &ads_tx, &ads_ty);
  f32 base_x = wdef->hip_off.x * (1.0f - b) + ads_tx * b;
  f32 base_y = wdef->hip_off.y * (1.0f - b) + ads_ty * b;
  Vector2 lean = present_lean_offsets(g->move_sway);
  f32 swing_yaw = g->move_vel.x * 1.2f * sway_scale;
  f32 swing_pitch = -g->move_vel.y * 0.5f * sway_scale;
  f32 swing_roll = -g->move_vel.x * 0.6f * sway_scale;
  PresentBob bob =
      present_bob_offsets(g->walk_phase, g->walk_blend, 1.0f - 0.5f * b);
  f32 sprint_pitch = wdef->sprint_tilt_deg * g->sprint_blend;
  rlTranslatef(base_x + g->gun_sway.x * 1.02f * sway_scale + lean.x + bob.pos.x,
               base_y + g->gun_sway.y * 1.28f * sway_scale +
                   rise * 0.70f * (1.0f - b * 0.8f) + bob.pos.y,
               0.75f + lean.y + bob.pos.z);
  rlRotatef(g->gun_sway.x * -52.0f * sway_scale + swing_yaw, 0.0f, 1.0f, 0.0f);
  rlRotatef((g->gun_sway.y * -46.0f + g->gun_sway.x * -11.0f) * sway_scale -
                rise * 230.0f * (1.0f - b * 0.85f) + swing_pitch +
                wdef->ads_pitch_deg * b + sprint_pitch,
            1.0f, 0.0f, 0.0f);
  rlRotatef(g->gun_sway.x * -29.0f * sway_scale + wdef->ads_cant_deg * b +
                bob.roll + swing_roll,
            0.0f, 0.0f, 1.0f);
  draw_gun_local(local_wid, g->gun_recoil / VIEWMODEL_SCALE,
                 g->gun_slide / VIEWMODEL_SCALE, VIEWMODEL_SCALE,
                 g->gun_kick != 0, reload_t);
  rlPopMatrix();
  EndMode3D();
  EndTextureMode();

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

static void weapon_predict_system(GameState *g) {
  u32 e = local_player_entity(g);
  if (e == ECS_INVALID_ENTITY)
    return;
  u8 before_ammo = g->weapon_ammo[e];
  u16 before_reload = g->weapon_reload[e];
  u8 before_id = g->weapon_id[e];
  bool want = (g->input[0].buttons & BTN_FIRE) != 0;
  bool holding = weapon_holding(g, e, want);
  weapon_tick(g, e);
  if (weapon_can_fire(g, e, want)) {
    const WeaponDef *def = weapon_def(g->weapon_id[e]);
    u8 wid = g->weapon_id[e];
    weapon_on_fired(g, e, holding);
    g->gun_kick = 4;
    g->gun_slide = holding ? def->slide_hold : def->slide_tap;
    g->gun_recoil_velocity +=
        holding ? def->recoil_vel_hold : def->recoil_vel_tap;
    g->gun_rise_velocity += holding ? def->rise_vel_hold : def->rise_vel_tap;
    g->screen_shake = holding ? 1.0f : 0.7f;
    g->view.pitch = weapon_recoil_pitch(g->view.pitch, wid, holding);
  }
  weapon_set_prev(g, e, want);
  if (g->weapon_ammo[e] != before_ammo ||
      g->weapon_reload[e] != before_reload || g->weapon_id[e] != before_id)
    g->weapon_dirty_seq[e] = g->next_sequence;
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
  game.view.yaw = 180;
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
      weapon_predict_system(&game);
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
