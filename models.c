#ifndef MODELS_C
#define MODELS_C

#include "rlgl.h"
#include "shared.c"

/* Weapon meshes, sight alignment, reload poses and ejected shells. */
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
  DrawCubeV((Vector3){-0.385f, 0.045f, 0.18f - slide_off},
            (Vector3){0.012f, 0.025f, 0.14f}, PAL_DARK);
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

#endif
