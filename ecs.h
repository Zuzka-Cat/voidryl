#ifndef ECS_H
#define ECS_H

#include <stdbool.h>
#include <stdint.h>

#define ECS_MAX_ENTITIES 32u
#define ECS_MAX_COMPONENTS 8u
#define ECS_INVALID_ENTITY UINT32_MAX

typedef struct ECS {
  uint64_t mask[ECS_MAX_COMPONENTS];
  uint32_t dead;
  uint8_t count;
} ECS;

typedef struct EcsQuery {
  const ECS *ecs;
  uint64_t bits;
  uint64_t wanted;
  uint32_t entity;
} EcsQuery;

static inline bool ecs_alive(const ECS *e, uint32_t ent) {
  return ent < e->count && !(e->dead & (UINT32_C(1) << ent));
}

static inline uint32_t ecs_create(ECS *e) {
  uint32_t live_range =
      e->count == 32u ? UINT32_MAX : (UINT32_C(1) << e->count) - 1u;
  uint32_t free_bits = e->dead & live_range;
  if (free_bits) {
    uint32_t ent = (uint32_t)__builtin_ctz(free_bits);
    e->dead &= ~(UINT32_C(1) << ent);
    return ent;
  }
  if (e->count >= ECS_MAX_ENTITIES)
    return ECS_INVALID_ENTITY;
  return e->count++;
}

static inline void ecs_add(ECS *e, uint32_t ent, uint32_t component) {
  if (ent < ECS_MAX_ENTITIES && component < ECS_MAX_COMPONENTS &&
      ecs_alive(e, ent))
    e->mask[component] |= UINT64_C(1) << ent;
}

static inline void ecs_remove(ECS *e, uint32_t ent, uint32_t component) {
  if (ent < ECS_MAX_ENTITIES && component < ECS_MAX_COMPONENTS)
    e->mask[component] &= ~(UINT64_C(1) << ent);
}

static inline bool ecs_has(const ECS *e, uint32_t ent, uint32_t component) {
  return ent < ECS_MAX_ENTITIES && component < ECS_MAX_COMPONENTS &&
         (e->mask[component] & (UINT64_C(1) << ent)) != 0;
}

static inline void ecs_destroy(ECS *e, uint32_t ent) {
  if (!ecs_alive(e, ent))
    return;
  for (uint32_t c = 0; c < ECS_MAX_COMPONENTS; ++c)
    e->mask[c] &= ~(UINT64_C(1) << ent);
  e->dead |= UINT32_C(1) << ent;
  while (e->count && (e->dead & (UINT32_C(1) << (e->count - 1u)))) {
    e->dead &= ~(UINT32_C(1) << (e->count - 1u));
    --e->count;
  }
}

static inline EcsQuery ecs_query(const ECS *e, const uint32_t *components,
                                 uint32_t n) {
  EcsQuery q = {e, UINT64_MAX, UINT64_MAX, ECS_INVALID_ENTITY};
  if (n > ECS_MAX_COMPONENTS)
    n = ECS_MAX_COMPONENTS;
  for (uint32_t i = 0; i < n; ++i)
    if (components[i] < ECS_MAX_COMPONENTS)
      q.wanted &= e->mask[components[i]];
  q.bits = q.wanted;
  if (e->count < ECS_MAX_ENTITIES)
    q.bits &= (UINT64_C(1) << e->count) - 1u;
  return q;
}

static inline bool ecs_next(EcsQuery *q) {
  if (!q->bits)
    return false;
  q->entity = (uint32_t)__builtin_ctzll(q->bits);
  q->bits &= q->bits - 1u;
  return true;
}

static inline uint32_t ecs_count(const ECS *e, const uint32_t *components,
                                 uint32_t n) {
  EcsQuery q = ecs_query(e, components, n);
  return (uint32_t)__builtin_popcountll(q.bits);
}

#endif
