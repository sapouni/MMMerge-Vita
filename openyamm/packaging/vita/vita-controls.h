#pragma once
#include <SDL3/SDL_scancode.h>
#include <string.h>
#include <stdlib.h>

// Shared by the loader and settings screen; numbers are persistent INI values.
enum OyVitaAction { OY_NONE, OY_MAP, OY_REST, OY_SPELLBOOK, OY_INVENTORY,
    OY_NEXT_CHARACTER, OY_QUICK_REFERENCE, OY_FLY_UP, OY_FLY_DOWN, OY_LAND,
    OY_FORWARD, OY_BACKWARD, OY_LEFT, OY_RIGHT, OY_JUMP, OY_CAST, OY_ATTACK,
    OY_INTERACT, OY_ACTION_COUNT };
static const char *const oy_vita_action_names[] = {"None", "Map", "Rest", "Spellbook", "Inventory",
    "Next character", "Party overview", "Fly up", "Fly down", "Land", "Move forward", "Move back",
    "Strafe left", "Strafe right", "Jump", "Quick spell", "Attack", "Interact"};
static const SDL_Scancode oy_vita_action_keys[] = {SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_L,
    SDL_SCANCODE_R, SDL_SCANCODE_C, SDL_SCANCODE_I, SDL_SCANCODE_TAB, SDL_SCANCODE_Z,
    SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_HOME, SDL_SCANCODE_W, SDL_SCANCODE_S,
    SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_X, SDL_SCANCODE_Q, SDL_SCANCODE_F, SDL_SCANCODE_SPACE};
#define OY_VITA_ACTION_VALUES "0|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17"
#define OY_VITA_BINDINGS(X) \
 X("bind_up", "D-pad up", OY_MAP, 1) \
 X("bind_down", "D-pad down", OY_REST, 1) \
 X("bind_left", "D-pad left", OY_SPELLBOOK, 1) \
 X("bind_right", "D-pad right", OY_NEXT_CHARACTER, 1) \
 X("bind_l_up", "L + D-pad up", OY_FLY_UP, 2) \
 X("bind_l_down", "L + D-pad down", OY_FLY_DOWN, 2) \
 X("bind_l_left", "L + D-pad left", OY_LAND, 2) \
 X("bind_l_right", "L + D-pad right", OY_MAP, 2) \
 X("bind_l_cross", "L + X", OY_INTERACT, 2) \
 X("bind_l_circle", "L + Circle", OY_CAST, 2) \
 X("bind_l_square", "L + Square", OY_ATTACK, 2) \
 X("bind_l_triangle", "L + Triangle", OY_REST, 2) \
 X("bind_l_r", "L + R", OY_QUICK_REFERENCE, 2)
typedef struct OyVitaBindingDefinition { const char *key, *label; int action, group; } OyVitaBindingDefinition;
#define OY_BIND_DEF(key,label,action,group) {key,label,action,group},
static const OyVitaBindingDefinition oy_vita_binding_definitions[] = { OY_VITA_BINDINGS(OY_BIND_DEF) };
#undef OY_BIND_DEF
enum { OY_VITA_BINDING_COUNT = sizeof(oy_vita_binding_definitions) / sizeof(oy_vita_binding_definitions[0]) };
typedef struct OyVitaControls { int bindings[OY_VITA_BINDING_COUNT]; int modifierMode; } OyVitaControls;
static void oy_vita_controls_init(OyVitaControls *c)
{
    for (int i=0;i<OY_VITA_BINDING_COUNT;++i) c->bindings[i]=oy_vita_binding_definitions[i].action;
    c->modifierMode=1; // Real-time gameplay. Turn-based combat retains L inspection.
}
static int oy_vita_control_value(OyVitaControls *c, const char *key, const char *value)
{
    char *end; long n=strtol(value,&end,10);
    if (end==value || *end || n<0) return 0;
    if (strcmp(key,"l_modifier")==0) { if(n>2)return 0; c->modifierMode=(int)n; return 1; }
    if (n>=OY_ACTION_COUNT) return 0;
    for(int i=0;i<OY_VITA_BINDING_COUNT;++i) if(strcmp(key,oy_vita_binding_definitions[i].key)==0)
    { c->bindings[i]=(int)n; return 1; }
    return 0;
}
static int oy_vita_modifier_enabled(const OyVitaControls *c, int realTime)
{ return c->modifierMode==2 || (c->modifierMode==1 && realTime); }

typedef struct OyVitaChord { int used; } OyVitaChord;
static int oy_vita_chord_inspect(OyVitaChord *s, int lHeld, int combinationHeld)
{
    if (!lHeld) s->used=0;
    else if (combinationHeld) s->used=1;
    return lHeld && !s->used; // A chord never turns into inspection midway through release.
}
