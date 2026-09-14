#ifndef CLIENT_C
#define CLIENT_C

#include "rlgl.h"
#include "shared.c"

static const Color PAL_DARK = {16, 8, 24, 255};
static const Color PAL_PURPLE = {60, 30, 102, 255};
static const Color PAL_MID = {155, 123, 208, 255};
static const Color PAL_LIGHT = {246, 213, 232, 255};

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
  g->gun_kick = 0;
  g->gun_recoil = g->gun_recoil_velocity = 0.0f;
  g->gun_rise = g->gun_rise_velocity = 0.0f;
  g->gun_slide = 0.0f;
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
    if (w->kind == KIND_PLAYER) {
      if (!had)
        weapon_set_from_snapshot(g, e, w->weapon, w->ammo,
                                 w->reloading != 0);
      else
        weapon_apply_snapshot(g, e, w->weapon, w->ammo,
                              w->reloading != 0);
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
    packet.buttons = g->input[0].buttons & (BTN_FIRE | BTN_GRAB | BTN_JUMP);
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
  u32 e = local_player_entity(g);
  Vector3 dir = {0.0f, 0.0f, 1.0f};
  Camera3D cam = {.position = (Vector3){0.0f, 1.5f, 0.0f},
                  .up = (Vector3){0.0f, 1.0f, 0.0f},
                  .fovy = 72.0f,
                  .projection = CAMERA_PERSPECTIVE};
  if (e != ECS_INVALID_ENTITY) {
    Vector3 p = entity_pos(g, e, a);
    cam.position = (Vector3){p.x, 1.5f + p.y, p.z};
    aim_dir(g->view_yaw, g->view_pitch, &dir);
  }
  cam.target = (Vector3){cam.position.x + dir.x, cam.position.y + dir.y,
                         cam.position.z + dir.z};
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
  f32 yaw_lag = (f32)lag_decode(g->lag_yaw[e]) * lag;
  f32 pitch_lag = (f32)lag_decode(g->lag_pitch[e]) * lag;
  bool reloading = weapon_is_reloading(g, e);
  Color body = hurt ? PAL_LIGHT : PAL_MID;
  rlPushMatrix();
  rlTranslatef(pos.x, pos.y, pos.z);
  rlRotatef((f32)g->yaw[e], 0.0f, 1.0f, 0.0f);
  draw_cube_lit((Vector3){0.0f, 0.72f, 0.0f}, (Vector3){0.50f, 1.36f, 0.30f},
                body);
  draw_cube_lit((Vector3){0.0f, 1.44f, 0.06f}, (Vector3){0.26f, 0.26f, 0.26f},
                hurt ? PAL_LIGHT : PAL_PURPLE);
  rlPushMatrix();
  rlTranslatef(-0.22f - yaw_lag * 0.018f, 1.16f + pitch_lag * 0.009f +
                                              fire * 0.05f -
                                              (reloading ? 0.10f : 0.0f),
               0.30f - fire * 0.02f);
  // True recoil climb for remotes plus stronger turn bob with up/down
  // cross-coupling from yaw motion.
  rlRotatef((f32)-g->pitch[e] - pitch_lag * 0.80f - yaw_lag * 0.28f -
                fire * 13.0f + (reloading ? 26.0f : 0.0f),
            1.0f, 0.0f, 0.0f);
  rlRotatef(-yaw_lag * 0.85f, 0.0f, 1.0f, 0.0f);
  rlRotatef(-yaw_lag * 0.30f, 0.0f, 0.0f, 1.0f);
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
  DrawRectangle(LOGICAL_W / 2, LOGICAL_H / 2, 1, 1, PAL_LIGHT);
  if (e != ECS_INVALID_ENTITY && g->connected) {
    bool reloading = weapon_is_reloading(g, e);
    unsigned ammo = weapon_ammo(g, e);
    unsigned mag = weapon_mag_size(g, e);
    if (ammo > mag)
      ammo = mag;
    if (reloading) {
      DrawRectangle(LOGICAL_W - 32, LOGICAL_H - 9, 30, 7, PAL_DARK);
      draw_small_text(LOGICAL_W - 31, LOGICAL_H - 8, STR("R"), PAL_MID);
      draw_small_text(LOGICAL_W - 26, LOGICAL_H - 8, STR("RELOAD"), PAL_LIGHT);
    } else {
      DrawRectangle(LOGICAL_W - 26, LOGICAL_H - 9, 24, 7, PAL_DARK);
      draw_number(LOGICAL_W - 25, LOGICAL_H - 8, ammo, PAL_LIGHT);
      int num_w = ammo >= 10 ? 7 : 3;
      int slash_x = LOGICAL_W - 25 + num_w + 2;
      draw_small_text(slash_x, LOGICAL_H - 8, STR("/"), PAL_MID);
      draw_number(slash_x + 4, LOGICAL_H - 8, mag, PAL_MID);
    }
  }
  if (g->help) {
    DrawRectangle(1, 17, 42, 16, PAL_DARK);
    DrawRectangle(47, 17, 44, 16, PAL_DARK);
    draw_small_text(2, 19, STR("WASD MOVE"), PAL_LIGHT);
    draw_small_text(2, 26, STR("MOUSE AIM"), PAL_LIGHT);
    draw_small_text(48, 19, STR("SPACE JUMP"), PAL_LIGHT);
    draw_small_text(48, 26, STR("E HOLD"), PAL_LIGHT);
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
}

static void update_viewmodel(GameState *g, f32 dt) {
  if (dt > 0.05f)
    dt = 0.05f;
  // Stronger turn bob: larger target, slightly slower follow for more lag.
  f32 tx = g->look_delta_x * 0.062f;
  f32 ty = -g->look_delta_y * 0.045f;
  tx = fmaxf(-0.65f, fminf(0.65f, tx));
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
  if (g->gun_recoil > 0.09f)
    g->gun_recoil = 0.09f;
  // True recoil climb: separate upward spring for muzzle rise.
  g->gun_rise_velocity +=
      (-95.0f * g->gun_rise - 13.0f * g->gun_rise_velocity) * dt;
  g->gun_rise += g->gun_rise_velocity * dt;
  if (g->gun_rise < 0.0f)
    g->gun_rise = g->gun_rise_velocity = 0.0f;
  if (g->gun_rise > 0.085f)
    g->gun_rise = 0.085f;
}

static void render_frame(GameState *g, RenderTexture2D target, f32 a) {
  Camera3D cam = make_camera(g, a);
  BeginTextureMode(target);
  ClearBackground(PAL_DARK);
  BeginMode3D(cam);
  DrawPlane((Vector3){0.0f, -0.02f, 0.0f}, (Vector2){32.0f, 32.0f}, PAL_PURPLE);
  draw_world(g, a);
  EndMode3D();
  Camera3D wcam = {.position = (Vector3){0.0f, 0.0f, 0.0f},
                   .target = (Vector3){0.0f, 0.0f, 1.0f},
                   .up = (Vector3){0.0f, 1.0f, 0.0f},
                   .fovy = 72.0f,
                   .projection = CAMERA_PERSPECTIVE};
  BeginMode3D(wcam);
  rlPushMatrix();
  f32 rise = g->gun_rise;
  u32 local_e = local_player_entity(g);
  bool local_reloading = local_e != ECS_INVALID_ENTITY &&
                         weapon_is_reloading(g, local_e);
  f32 reload_dip = local_reloading ? 0.09f : 0.0f;
  rlTranslatef(g->gun_sway_x * 1.28f, -0.18f + g->gun_sway_y * 1.28f +
                                        rise * 0.70f - reload_dip,
               0.75f);
  rlRotatef(g->gun_sway_x * -68.0f, 0.0f, 1.0f, 0.0f);
  // Up/down rotation: direct pitch response plus a smaller cross-coupling
  // from turning so yaw motion also tilts the gun vertically.
  rlRotatef(g->gun_sway_y * -46.0f + g->gun_sway_x * -14.0f - rise * 230.0f +
                (local_reloading ? 24.0f : 0.0f),
            1.0f, 0.0f, 0.0f);
  rlRotatef(g->gun_sway_x * -38.0f, 0.0f, 0.0f, 1.0f);
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
  game_init(&game);
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
    render_frame(&game, target, alpha);
  }
  client_send_input(&game, fd, NET_PACKET_DISCONNECT);
  EnableCursor();
  UnloadRenderTexture(target);
  CloseWindow();
  close(fd);
  return 0;
}

#endif
