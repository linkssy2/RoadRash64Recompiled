#ifndef __RR64_RECOMPUI_EVENT_STRUCTS_H__
#define __RR64_RECOMPUI_EVENT_STRUCTS_H__

/*
 * Port-side ABI definitions consumed by RecompFrontend's ui_api_events.cpp.
 * Keep the enum ordering synchronized with RecompFrontend's
 * recompui/src/elements/ui_types.h.
 *
 * Road Rash does not expose mod UI callbacks yet, but RecompFrontend compiles
 * the callback bridge unconditionally.  These definitions provide the ABI the
 * frontend expects without introducing a MIPS patch build dependency.
 */

typedef enum {
    RECOMPUI_EVENT_NONE = 0,
    RECOMPUI_EVENT_CLICK,
    RECOMPUI_EVENT_FOCUS,
    RECOMPUI_EVENT_HOVER,
    RECOMPUI_EVENT_ENABLE,
    RECOMPUI_EVENT_DRAG,
    RECOMPUI_EVENT_TEXT,
    RECOMPUI_EVENT_UPDATE,
    RECOMPUI_EVENT_NAVIGATE,
    RECOMPUI_EVENT_MOUSE_BUTTON,
    RECOMPUI_EVENT_MENU_ACTION,
    RECOMPUI_EVENT_COUNT
} RecompuiEventType;

typedef enum {
    RECOMPUI_DRAG_PHASE_NONE = 0,
    RECOMPUI_DRAG_PHASE_START,
    RECOMPUI_DRAG_PHASE_MOVE,
    RECOMPUI_DRAG_PHASE_END
} RecompuiDragPhase;

typedef enum {
    RECOMPUI_MENU_ACTION_NONE = 0,
    RECOMPUI_MENU_ACTION_ACCEPT,
    RECOMPUI_MENU_ACTION_APPLY,
    RECOMPUI_MENU_ACTION_BACK,
    RECOMPUI_MENU_ACTION_TOGGLE,
    RECOMPUI_MENU_ACTION_TAB_LEFT,
    RECOMPUI_MENU_ACTION_TAB_RIGHT
} RecompuiMenuAction;

typedef enum {
    RECOMPUI_NAV_UP = 0,
    RECOMPUI_NAV_RIGHT,
    RECOMPUI_NAV_DOWN,
    RECOMPUI_NAV_LEFT
} RecompuiNavDirection;

typedef enum {
    RECOMPUI_MOUSE_LEFT = 0,
    RECOMPUI_MOUSE_RIGHT,
    RECOMPUI_MOUSE_MIDDLE,
    RECOMPUI_MOUSE_COUNT
} RecompuiMouseButton;

typedef struct {
    float x;
    float y;
} RecompuiEventClick;

typedef struct {
    unsigned int active;
} RecompuiEventFocus;

typedef struct {
    unsigned int active;
} RecompuiEventHover;

typedef struct {
    unsigned int active;
} RecompuiEventEnable;

typedef struct {
    RecompuiDragPhase phase;
    float x;
    float y;
} RecompuiEventDrag;

typedef struct {
    RecompuiNavDirection direction;
} RecompuiEventNavigate;

typedef struct {
    float x;
    float y;
    RecompuiMouseButton button;
    unsigned int pressed;
} RecompuiEventMouseButton;

typedef struct {
    RecompuiMenuAction action;
} RecompuiEventMenuAction;

typedef union {
    RecompuiEventClick click;
    RecompuiEventFocus focus;
    RecompuiEventHover hover;
    RecompuiEventEnable enable;
    RecompuiEventDrag drag;
    RecompuiEventNavigate navigate;
    RecompuiEventMouseButton mouse_button;
    RecompuiEventMenuAction menu_action;
} RecompuiEventPayload;

typedef struct {
    RecompuiEventType type;
    RecompuiEventPayload data;
} RecompuiEventData;

#endif
