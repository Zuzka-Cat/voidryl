#ifndef NETWORKING_C
#define NETWORKING_C

#include "shared.c"

/* Transport setup and shared wire format. */
static i32 net_snapshot_wire_size(u8 count) {
  if (count > NET_ENTITY_LIMIT)
    return -1;
  return (i32)(NET_SNAPSHOT_HEADER_SIZE + (i32)count * (i32)sizeof(NetEntity) +
               NET_SNAPSHOT_NAMES_SIZE);
}

static bool valid_wire_name(String name) {
  if (name.length != PLAYER_NAME_LENGTH)
    return false;
  for (i32 i = 0; i < PLAYER_NAME_LENGTH; ++i) {
    if (!CHAR_IS_UPPER(name.items[i]))
      return false;
  }
  return true;
}

static bool parse_ipv4(String text, struct in_addr *address) {
  char tmp[64];
  String use = text;
  if (string_eq(text, STR("localhost")))
    use = STR("127.0.0.1");
  if (use.length <= 0 || use.length >= (i32)sizeof(tmp))
    return false;
  for (i32 i = 0; i < use.length; ++i) {
    tmp[i] = (char)use.items[i];
  }
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

/* Preserve the original fixed-size packet whenever it fits. Larger world
 * states use a compact per-kind wire representation: the first 20 bytes of
 * each NetEntity, followed by 3+6 aim bytes for players only. */
static i32 net_snapshot_compact_size(const NetSnapshot *snap) {
  if (!snap || snap->count > NET_ENTITY_LIMIT)
    return -1;
  i32 size = (i32)(NET_SNAPSHOT_HEADER_SIZE + NET_SNAPSHOT_NAMES_SIZE);
  u32 players = 0;
  for (u32 i = 0; i < snap->count; ++i) {
    size += (i32)NET_ENTITY_BASE_WIRE_SIZE;
    if (snap->entities[i].kind == KIND_PLAYER) {
      if (++players > MAX_PLAYERS)
        return -1;
      size += (i32)NET_PLAYER_AIM_WIRE_SIZE;
    }
  }
  return size <= (i32)NET_MAX_DATAGRAM ? size : -1;
}

static bool net_snapshot_wire_ok(const u8 *buf, i32 n) {
  if (!buf || n < (i32)(NET_SNAPSHOT_HEADER_SIZE + NET_SNAPSHOT_NAMES_SIZE) ||
      n > (i32)NET_MAX_DATAGRAM ||
      buf[offsetof(NetSnapshot, magic)] != NET_MAGIC)
    return false;
  u8 type = buf[offsetof(NetSnapshot, type)];
  if (type != NET_PACKET_SNAPSHOT && type != NET_PACKET_SNAPSHOT_COMPACT)
    return false;
  u8 count = buf[offsetof(NetSnapshot, count)];
  if (count > NET_ENTITY_LIMIT)
    return false;
  if (type == NET_PACKET_SNAPSHOT)
    return n == net_snapshot_wire_size(count); /* Legacy 30-byte records. */
  i32 pos = (i32)NET_SNAPSHOT_HEADER_SIZE;
  u32 players = 0;
  for (u32 i = 0; i < count; ++i) {
    if (pos + (i32)NET_ENTITY_BASE_WIRE_SIZE > n - NET_SNAPSHOT_NAMES_SIZE)
      return false;
    u8 kind = buf[pos + offsetof(NetEntity, kind)];
    pos += (i32)NET_ENTITY_BASE_WIRE_SIZE;
    if (kind == KIND_PLAYER) {
      if (++players > MAX_PLAYERS ||
          pos + (i32)NET_PLAYER_AIM_WIRE_SIZE > n - NET_SNAPSHOT_NAMES_SIZE)
        return false;
      pos += (i32)NET_PLAYER_AIM_WIRE_SIZE;
    }
  }
  return pos + NET_SNAPSHOT_NAMES_SIZE == n;
}

static i32 net_snapshot_encode(const NetSnapshot *snap, u8 *out, i32 out_cap) {
  if (!snap || !out || snap->count > NET_ENTITY_LIMIT)
    return -1;
  i32 want = net_snapshot_wire_size(snap->count);
  if (want >= 0 && want <= (i32)NET_MAX_DATAGRAM && want <= out_cap) {
    memcpy(out, snap, NET_SNAPSHOT_HEADER_SIZE);
    out[offsetof(NetSnapshot, type)] = NET_PACKET_SNAPSHOT;
    if (snap->count)
      memcpy(out + NET_SNAPSHOT_HEADER_SIZE, snap->entities,
             (size_t)snap->count * sizeof(NetEntity));
    memcpy(out + NET_SNAPSHOT_HEADER_SIZE +
               (size_t)snap->count * sizeof(NetEntity),
           snap->names, NET_SNAPSHOT_NAMES_SIZE);
    return want;
  }
  want = net_snapshot_compact_size(snap);
  if (want < 0 || out_cap < want)
    return -1;
  memcpy(out, snap, NET_SNAPSHOT_HEADER_SIZE);
  out[offsetof(NetSnapshot, type)] = NET_PACKET_SNAPSHOT_COMPACT;
  size_t pos = NET_SNAPSHOT_HEADER_SIZE;
  for (u32 i = 0; i < snap->count; ++i) {
    const NetEntity *e = &snap->entities[i];
    memcpy(out + pos, e, NET_ENTITY_BASE_WIRE_SIZE);
    pos += NET_ENTITY_BASE_WIRE_SIZE;
    if (e->kind == KIND_PLAYER) {
      memcpy(out + pos, &e->aim_dir, sizeof(e->aim_dir));
      pos += sizeof(e->aim_dir);
      memcpy(out + pos, &e->aim_point, sizeof(e->aim_point));
      pos += sizeof(e->aim_point);
    }
  }
  memcpy(out + pos, snap->names, NET_SNAPSHOT_NAMES_SIZE);
  return want;
}

static bool net_snapshot_decode(const u8 *buf, i32 n, NetSnapshot *out) {
  if (!out || !net_snapshot_wire_ok(buf, n))
    return false;
  u8 count = buf[offsetof(NetSnapshot, count)];
  *out = (NetSnapshot){0};
  memcpy(out, buf, NET_SNAPSHOT_HEADER_SIZE);
  out->type =
      NET_PACKET_SNAPSHOT; /* Internal representation is format-neutral. */
  out->count = count;
  if (buf[offsetof(NetSnapshot, type)] == NET_PACKET_SNAPSHOT) {
    if (count)
      memcpy(out->entities, buf + NET_SNAPSHOT_HEADER_SIZE,
             (size_t)count * sizeof(NetEntity));
    memcpy(out->names,
           buf + NET_SNAPSHOT_HEADER_SIZE + (size_t)count * sizeof(NetEntity),
           NET_SNAPSHOT_NAMES_SIZE);
    return true;
  }
  size_t pos = NET_SNAPSHOT_HEADER_SIZE;
  for (u32 i = 0; i < count; ++i) {
    NetEntity *e = &out->entities[i];
    memcpy(e, buf + pos, NET_ENTITY_BASE_WIRE_SIZE);
    pos += NET_ENTITY_BASE_WIRE_SIZE;
    e->aim_dir = (AimVectorQ){0, 0, 127};
    e->aim_point = (IVec3){0, 0, POS_SCALE};
    if (e->kind == KIND_PLAYER) {
      memcpy(&e->aim_dir, buf + pos, sizeof(e->aim_dir));
      pos += sizeof(e->aim_dir);
      memcpy(&e->aim_point, buf + pos, sizeof(e->aim_point));
      pos += sizeof(e->aim_point);
    }
  }
  memcpy(out->names, buf + pos, NET_SNAPSHOT_NAMES_SIZE);
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
  out->aim_dir.x = (i8)buf[offsetof(NetInputPacket, aim_dir.x)];
  out->aim_dir.y = (i8)buf[offsetof(NetInputPacket, aim_dir.y)];
  out->aim_dir.z = (i8)buf[offsetof(NetInputPacket, aim_dir.z)];
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

/* UDP socket lifecycle. */
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

/* Client receive, input and chat sending. */
// have smaller functions than these huge conglomerates
static void client_receive_chat(GameState *g, const NetChatPacket *chat) {
  String name = {(u8 *)chat->name, PLAYER_NAME_LENGTH, PLAYER_NAME_LENGTH};
  if (chat->session != g->session ||
      !net_validate_chat_fields(name, chat->length, chat->sequence,
                                g->got_chat_sequence != 0,
                                g->last_chat_sequence)) {
    return;
  }
  g->got_chat_sequence = 1;
  g->last_chat_sequence = chat->sequence;
  String message = {(u8 *)chat->text, chat->length, CHAT_LENGTH};
  notice_chat_msg(g, name, message);
}

static void client_record_ping(GameState *g, u16 ack) {
  u32 slot = (u32)ack & PING_WINDOW_MASK;
  if (!g->ping.slot_valid[slot] || g->ping.send_seq[slot] != ack) {
    return;
  }
  double sample =
      (hires_time_in_seconds() - g->ping.send_time[slot]) * PING_MS_PER_SEC;
  if (sample < 0.0) {
    sample = 0.0;
  }
  if (sample > PING_MAX_MS) {
    sample = PING_MAX_MS;
  }
  if (!g->ping.have) {
    g->ping.rtt_ms = sample;
    g->ping.have = 1;
  } else {
    g->ping.rtt_ms =
        g->ping.rtt_ms * (1.0 - PING_SMOOTH_NEW) + sample * PING_SMOOTH_NEW;
  }
}

static void client_receive(GameState *g, int fd) {
  u8 buf[NET_MAX_DATAGRAM];
  for (;;) {
    ssize_t raw = recv(fd, buf, sizeof(buf), 0);
    if (raw < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        if (g->connected) {
          notice_set(g, STR("DROPPED"));
        }
        renew_client_session(g);
      }
      return;
    }
    if (raw == 0 || raw > (ssize_t)sizeof(buf)) {
      continue;
    }
    i32 n = (i32)raw;
    NetChatPacket chat;
    if (net_chat_decode(buf, n, &chat)) {
      client_receive_chat(g, &chat);
      continue;
    }
    NetSnapshot snap;
    if (!net_snapshot_decode(buf, n, &snap) ||
        !net_validate_snapshot_fields(&snap, g->session, g->got_snapshot != 0,
                                      g->last_snapshot_tick)) {
      continue;
    }
    client_record_ping(g, snap.acknowledged_sequence);
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
    packet.aim_dir =
        aim_vector_encode(sight_dir((f32)g->view.yaw, (f32)g->view.pitch));
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

/* Server snapshot creation and datagram transport. */
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
    switch (g->kind[e]) {
    case KIND_CUBE:
      scale = scale_encode(scale_value(g, e));
      break;
    case KIND_CASING:
      weapon = g->weapon_id[e] < WEAPON_COUNT ? g->weapon_id[e] : WEAPON_P99;
      break;
    case KIND_PLAYER:
      weapon = g->weapon_id[e];
      ammo = g->weapon_ammo[e];
      reloading = (u8)(g->weapon_reload[e] != 0);
      reload_q = encode_reload_q(reloading != 0, weapon_reload_progress(g, e));
      if (g->owner[e] < MAX_PLAYERS) {
        scale = pack_player_locomotion(&g->input[g->owner[e]]);
      }
      break;
    default:
      break;
    }
    snap->entities[snap->count++] = (NetEntity){
        .entity = (u8)e,
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
        .pitch = g->aim[e].pitch,
        .aim_dir =
            g->kind[e] == KIND_PLAYER ? g->aim_dir[e] : (AimVectorQ){0, 0, 127},
        .aim_point = g->kind[e] == KIND_PLAYER ? g->aim_point[e]
                                               : (IVec3){0, 0, POS_SCALE}};
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
  g->input[peer->player].aim_dir = aim_vector_pitch_limited(packet->aim_dir);
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

static void server_send_all(GameState *g, int fd) {
  NetSnapshot snap;
  bool have_peer = false;
  for (int i = 0; i < MAX_PLAYERS; ++i) {
    have_peer |= g->peers[i].active && g->peers[i].player < MAX_PLAYERS;
  }
  if (!have_peer)
    return;
  /* Sight targets are presentation-only and need one raycast per snapshot,
   * not one on each of the five intervening simulation ticks. */
  system_aim_point(g);
  for (int i = 0; i < MAX_PLAYERS; ++i) {
    if (!g->peers[i].active || g->peers[i].player >= MAX_PLAYERS)
      continue;
    u16 ack = g->peers[i].got_sequence ? g->peers[i].last_sequence : 0;
    write_snapshot(g, &snap, g->peers[i].player, ack, g->peers[i].session);
    send_peer_snapshot(fd, &g->peers[i], &snap);
  }
}

#endif
