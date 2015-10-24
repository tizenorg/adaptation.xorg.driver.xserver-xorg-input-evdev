/*
 * Copyright © 2004-2008 Red Hat, Inc.
 * Copyright © 2008 University of South Australia
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Authors:
 *	Kristian Høgsberg (krh@redhat.com)
 *	Adam Jackson (ajax@redhat.com)
 *	Peter Hutterer (peter@cs.unisa.edu.au)
 *	Oliver McFadden (oliver.mcfadden@nokia.com)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef EVDEV_H
#define EVDEV_H

#include <linux/input.h>
#include <linux/types.h>

#include <xorg-server.h>
#include <xf86Xinput.h>
#include <xf86_OSproc.h>
#include <xkbstr.h>

#ifdef MULTITOUCH
#include <mtdev.h>
#endif

#ifdef ENABLE_TTRACE
#include <ttrace.h>

#define TTRACE_BEGIN(NAME) traceBegin(TTRACE_TAG_INPUT, NAME)
#define TTRACE_END() traceEnd(TTRACE_TAG_INPUT)
#else //ENABLE_TTRACE
#define TTRACE_BEGIN(NAME)
#define TTRACE_END()
#endif //ENABLE_TTRACE

#ifdef _F_GESTURE_EXTENSION_
typedef enum _MTSyncType
{
    MTOUCH_FRAME_SYNC_END,
    MTOUCH_FRAME_SYNC_BEGIN,
    MTOUCH_FRAME_SYNC_UPDATE,
    ROTARY_FRAME_SYNC_END,
    ROTARY_FRAME_SYNC_BEGIN
} MTSyncType;

enum EventType
{
#ifdef _F_PROXY_DEVICE_ENABLED_
    ET_Motion = 6,
    ET_DeviceChanged = 17,
    ET_RawMotion = 24,
#endif /* #ifdef _F_PROXY_DEVICE_ENABLED_ */
    ET_MTSync = 0x7E,
    ET_Internal = 0xFF /* First byte */
};

typedef struct _AnyEvent AnyEvent;
struct _AnyEvent
{
    unsigned char header; /**< Always ET_Internal */
    enum EventType type;  /**< One of EventType */
    int length;           /**< Length in bytes */
    Time time;            /**< Time in ms */
    int deviceid;
    MTSyncType sync;
    int x;
    int y;
};

#ifdef _F_PROXY_DEVICE_ENABLED_

#define DEVCHANGE_SLAVE_SWITCH 0x2
#define DEVCHANGE_KEYBOARD_EVENT 0x8

struct _DeviceChangedEvent {
    unsigned char header; /**< Always ET_Internal */
    enum EventType type;  /**< ET_DeviceChanged */
    int length;           /**< Length in bytes */
    Time time;            /**< Time in ms */
    int deviceid;         /**< Device whose capabilities have changed */
    int flags;            /**< Mask of ::HAS_NEW_SLAVE,
                               ::POINTER_EVENT, ::KEYBOARD_EVENT */
    int masterid;         /**< MD when event was generated */
    int sourceid;         /**< The device that caused the change */

    struct {
        int num_buttons;        /**< Number of buttons */
        Atom names[MAX_BUTTONS];/**< Button names */
    } buttons;

    int num_valuators;          /**< Number of axes */
    struct {
        uint32_t min;           /**< Minimum value */
        uint32_t max;           /**< Maximum value */
        double value;           /**< Current value */
        /* FIXME: frac parts of min/max */
        uint32_t resolution;    /**< Resolution counts/m */
        uint8_t mode;           /**< Relative or Absolute */
        Atom name;              /**< Axis name */
        ScrollInfo scroll;      /**< Smooth scrolling info */
    } valuators[MAX_VALUATORS];

    struct {
        int min_keycode;
        int max_keycode;
    } keys;
};
#endif /* #ifdef _F_PROXY_DEVICE_ENABLED_ */

union _InternalEvent {
	struct {
	    unsigned char header; /**< Always ET_Internal */
	    enum EventType type;  /**< One of ET_* */
	    int length;           /**< Length in bytes */
	    Time time;            /**< Time in ms. */
	} any;
	AnyEvent any_event;
#ifdef _F_PROXY_DEVICE_ENABLED_
    DeviceChangedEvent changed_event;
#endif /* #ifdef _F_PROXY_DEVICE_ENABLED_ */
};

#define GESTURE_DEV_NAME "Gesture"
/* Gesture driver will query devices information
  * if a gesture driver's control function is called using DEVICE_READY*/
#define DEVICE_READY 11

#endif /* #ifdef _F_GESTURE_EXTENSION_ */

#ifndef EV_CNT /* linux 2.6.23 kernels and earlier lack _CNT defines */
#define EV_CNT (EV_MAX+1)
#endif
#ifndef KEY_CNT
#define KEY_CNT (KEY_MAX+1)
#endif
#ifndef REL_CNT
#define REL_CNT (REL_MAX+1)
#endif
#ifndef ABS_CNT
#define ABS_CNT (ABS_MAX+1)
#endif
#ifndef LED_CNT
#define LED_CNT (LED_MAX+1)
#endif

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 14
#define HAVE_SMOOTH_SCROLLING 1
#endif

#define EVDEV_MAXBUTTONS 32
#define EVDEV_MAXQUEUE 32

#define EVDEV_PRESS 1
#define EVDEV_RELEASE 0

#ifdef _ENV_TV_
#define EVDEV_RMS_TIMEOUT 5000
#else
#define EVDEV_RMS_TIMEOUT 15000
#endif //_ENV_TV_

/* evdev flags */
#define EVDEV_KEYBOARD_EVENTS	(1 << 0)
#define EVDEV_BUTTON_EVENTS	(1 << 1)
#define EVDEV_RELATIVE_EVENTS	(1 << 2)
#define EVDEV_ABSOLUTE_EVENTS	(1 << 3)
#define EVDEV_TOUCHPAD		(1 << 4)
#define EVDEV_INITIALIZED	(1 << 5) /* WheelInit etc. called already? */
#define EVDEV_TOUCHSCREEN	(1 << 6)
#define EVDEV_CALIBRATED	(1 << 7) /* run-time calibrated? */
#define EVDEV_TABLET		(1 << 8) /* device looks like a tablet? */
#define EVDEV_UNIGNORE_ABSOLUTE (1 << 9) /* explicitly unignore abs axes */
#define EVDEV_UNIGNORE_RELATIVE (1 << 10) /* explicitly unignore rel axes */
#define EVDEV_RELATIVE_MODE	(1 << 11) /* Force relative events for devices with absolute axes */
#ifdef _F_EVDEV_CONFINE_REGION_
#define EVDEV_CONFINE_REGION	(1 << 12)
#endif /* #ifdef _F_EVDEV_CONFINE_REGION_ */
#ifdef _F_EVDEV_SUPPORT_GAMEPAD
#define EVDEV_GAMEPAD   (1 << 13)

#define MAX_GAMEPAD_DEFINITION_ABS 3
#define MAX_GAMEPAD_DEFINITION_KEY 10
#endif //_F_EVDEV_SUPPORT_GAMEPAD

#ifdef _F_EVDEV_SUPPORT_ROTARY_
#define EVDEV_OFM  (1 << 14)
#define EVDEV_HALLIC (1 << 15)

#define DEFAULT_HW_ROTARY_MAX 4360
#endif //_F_EVDEV_SUPPORT_ROTARY_

#ifdef _F_EVDEV_SUPPORT_SMARTRC_
#define EVDEV_SMART_RC	(1 << 16) /* air touch mouse is special remote controller for smart tv */
#define AIR_TOUCH_MOUSE "Advanced Touch REMOTE"
#endif //_F_EVDEV_SUPPORT_SMARTRC_

#ifndef MAX_VALUATORS
#define MAX_VALUATORS 36
#endif

#ifndef XI_PROP_DEVICE_NODE
#define XI_PROP_DEVICE_NODE "Device Node"
#endif

#ifndef XI_PROP_DEVICE_TYPE
#define XI_PROP_DEVICE_TYPE "Device Type"
#endif

#ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_
#define XI_PROP_REL_MOVE_STATUS "Relative Move Status"
#define XI_PROP_REL_MOVE_ACK "Relative Move Acknowledge"
#endif /* #ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_ */
#ifdef _F_BLOCK_MOTION_DEVICE_
#define XI_PROP_BLOCK_MOTION_DEVICE "Block Motion Device" /*Atom to block motion device */
#endif //_F_BLOCK_MOTION_DEVICE_
#ifdef _F_PICTURE_OFF_MODE_ENABLE_
#define XI_PROP_PICTURE_OFF_MODE "PICTURE_OFF_MODE" /* Atom to check picture off mode */
#endif //_F_PICTURE_OFF_MODE_ENABLE_

#ifdef _F_PROXY_DEVICE_ENABLED_
#define XI_PROP_USE_PROXY_SLAVE_DEVICE "Use Proxy Slave Device"
#endif /* #ifdef _F_PROXY_DEVICE_ENABLED_ */

#define XI_PROP_UNGRAB_DEVICE "Ungrab Device"

#define LONG_BITS (sizeof(long) * 8)

/* Number of longs needed to hold the given number of bits */
#define NLONGS(x) (((x) + LONG_BITS - 1) / LONG_BITS)

/* Function key mode */
enum fkeymode {
    FKEYMODE_UNKNOWN = 0,
    FKEYMODE_FKEYS,       /* function keys send function keys */
    FKEYMODE_MMKEYS,      /* function keys send multimedia keys */
};

enum SlotState {
    SLOTSTATE_OPEN = 8,
    SLOTSTATE_CLOSE,
    SLOTSTATE_UPDATE,
    SLOTSTATE_EMPTY,
};

enum ButtonAction {
    BUTTON_RELEASE = 0,
    BUTTON_PRESS = 1
};

/* axis specific data for wheel emulation */
typedef struct {
    int up_button;
    int down_button;
    int traveled_distance;
} WheelAxis, *WheelAxisPtr;

#ifdef _F_MULTI_PASSIVE_GRAB_
typedef struct _ActivatedKeyList
{
    CARD8 keycode;
    struct xorg_list list;
} ActivatedKeyList;
#endif //_F_MULTI_PASSIVE_GRAB_

/* Event queue used to defer keyboard/button events until EV_SYN time. */
typedef struct {
    enum {
        EV_QUEUE_KEY,	/* xf86PostKeyboardEvent() */
        EV_QUEUE_BTN,	/* xf86PostButtonEvent() */
        EV_QUEUE_PROXIMITY, /* xf86PostProximityEvent() */
#ifdef MULTITOUCH
        EV_QUEUE_TOUCH,	/*xf86PostTouchEvent() */
#endif
    } type;
    union {
        int key;	/* May be either a key code or button number. */
#ifdef MULTITOUCH
        unsigned int touch; /* Touch ID */
#endif
    } detail;
    int val;	/* State of the key/button/touch; pressed or released. */
#ifdef MULTITOUCH
    ValuatorMask *touchMask;
#endif
} EvdevEventQueueRec, *EvdevEventQueuePtr;

#ifdef _F_REMAP_KEYS_
typedef struct {
    uint8_t cd[256];
} EvdevKeyRemapSlice;
typedef struct {
    EvdevKeyRemapSlice* sl[256];
} EvdevKeyRemap, *EvdevKeyRemapPtr;
#endif //_F_REMAP_KEYS_

typedef struct {
#ifdef _F_MULTI_PASSIVE_GRAB_
    ActivatedKeyList *activatedkeylist;
#endif //_F_MULTI_PASSIVE_GRAB_

    unsigned short id_vendor;
    unsigned short id_product;

    char *device;
    int grabDevice;         /* grab the event device? */

    int num_vals;           /* number of valuators */
    int num_mt_vals;        /* number of multitouch valuators */
    int axis_map[max(ABS_CNT, REL_CNT)]; /* Map evdev <axis> to index */
    ValuatorMask *vals;     /* new values coming in */
    ValuatorMask *old_vals; /* old values for calculating relative motion */
    ValuatorMask *prox;     /* last values set while not in proximity */
    ValuatorMask *mt_mask;
    ValuatorMask **last_mt_vals;
    int cur_slot;
    enum SlotState slot_state;
#ifdef MULTITOUCH
    struct mtdev *mtdev;
#endif

    int flags;
    int in_proximity;           /* device in proximity */
    int use_proximity;          /* using the proximity bit? */
    int num_buttons;            /* number of buttons */
    BOOL swap_axes;
    BOOL invert_x;
    BOOL invert_y;

    int delta[REL_CNT];
    unsigned int abs_queued, rel_queued, prox_queued;

    /* XKB stuff has to be per-device rather than per-driver */
    XkbRMLVOSet rmlvo;

    /* Middle mouse button emulation */
    struct {
        BOOL                enabled;
        BOOL                pending;     /* timer waiting? */
        int                 buttonstate; /* phys. button state */
        int                 state;       /* state machine (see bt3emu.c) */
        Time                expires;     /* time of expiry */
        Time                timeout;
    } emulateMB;
    /* Third mouse button emulation */
    struct emulate3B {
        BOOL                enabled;
        BOOL                state;       /* current state */
        Time                timeout;     /* timeout until third button press */
        int                 buttonstate; /* phys. button state */
        int                 button;      /* phys button to emit */
        int                 threshold;   /* move threshold in dev coords */
        OsTimerPtr          timer;
        int                 delta[2];    /* delta x/y, accumulating */
        int                 startpos[2]; /* starting pos for abs devices */
        int                 flags;       /* remember if we had rel or abs movement */
    } emulate3B;
    struct {
	int                 meta;           /* meta key to lock any button */
	BOOL                meta_state;     /* meta_button state */
	unsigned int        lock_pair[EVDEV_MAXBUTTONS];  /* specify a meta/lock pair */
	BOOL                lock_state[EVDEV_MAXBUTTONS]; /* state of any locked buttons */
    } dragLock;
    struct {
        BOOL                enabled;
        int                 button;
        int                 button_state;
        int                 inertia;
        WheelAxis           X;
        WheelAxis           Y;
        Time                expires;     /* time of expiry */
        Time                timeout;
    } emulateWheel;
    /* run-time calibration */
    struct {
        int                 min_x;
        int                 max_x;
        int                 min_y;
        int                 max_y;
    } calibration;

    unsigned char btnmap[32];           /* config-file specified button mapping */

#ifdef _F_REMAP_KEYS_
    EvdevKeyRemapPtr keyremap;
#endif //_F_REMAP_KEYS_

    int reopen_attempts; /* max attempts to re-open after read failure */
    int reopen_left;     /* number of attempts left to re-open the device */
    OsTimerPtr reopen_timer;

#ifdef _F_EVDEV_CONFINE_REGION_
    //Backup pointer(s) for cursor
    CursorLimitsProcPtr pOrgCursorLimits;
    ConstrainCursorProcPtr pOrgConstrainCursor;

    //Confining information
    int confined_id;
    BoxPtr pointer_confine_region;
#endif /* #ifdef _F_EVDEV_CONFINE_REGION_ */
#ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_
    BOOL rel_move_status;
    BOOL rel_move_prop_set;
    int rel_move_ack;
    OsTimerPtr rel_move_timer;
#endif /* #ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_ */
    Bool block_handler_registered;
#ifdef _F_GESTURE_EXTENSION_
    int *mt_status;
#endif /* #ifdef _F_GESTURE_EXTENSION_ */

#ifdef _F_TOUCH_TRANSFORM_MATRIX_
    float transform[9];
    BOOL use_transform;
    struct pixman_transform inv_transform;
#endif /* #ifdef _F_TOUCH_TRANSFORM_MATRIX_ */

    /* Cached info from device. */
    char name[1024];
    unsigned long bitmask[NLONGS(EV_CNT)];
    unsigned long key_bitmask[NLONGS(KEY_CNT)];
    unsigned long rel_bitmask[NLONGS(REL_CNT)];
    unsigned long abs_bitmask[NLONGS(ABS_CNT)];
    unsigned long led_bitmask[NLONGS(LED_CNT)];
    struct input_absinfo absinfo[ABS_CNT];
#ifdef _F_EVDEV_SUPPORT_GAMEPAD
    int pre_hatx;
    int pre_haty;
    int pre_x;
    int pre_y;
    int keycode_btnA;
    int keycode_btnB;
    int keycode_btnX;
    int keycode_btnY;
    int keycode_btnTL;
    int keycode_btnTR;
    int keycode_btnStart;
    int keycode_btnSelect;
    int keycode_btnPlay;

    BOOL support_directional_key;

    int abs_gamepad_labels[MAX_GAMEPAD_DEFINITION_ABS];
    int key_gamepad_labels[MAX_GAMEPAD_DEFINITION_KEY];
#endif//_F_EVDEV_SUPPORT_GAMEPAD

#ifdef _F_USE_DEFAULT_XKB_RULES_
    BOOL use_default_xkb_rmlvo;
#endif//_F_USE_DEFAULT_XKB_RULES_

#ifdef _F_SUPPORT_ROTATION_ANGLE_
    int rotation_angle;
    int rotation_node;
#endif// _F_SUPPORT_ROTATION_ANGLE_

    /* minor/major number */
    dev_t min_maj;

    /* Event queue used to defer keyboard/button events until EV_SYN time. */
    int                     num_queue;
    EvdevEventQueueRec           queue[EVDEV_MAXQUEUE];

    enum fkeymode           fkeymode;

#ifdef _F_EVDEV_SUPPORT_ROTARY_
    void (*extra_rel_post_ofm) (InputInfoPtr pInfo, int num_v, int first_v, int v[MAX_VALUATORS]);
    void (*extra_rel_post_hallic) (InputInfoPtr pInfo, int num_v, int first_v, int v[MAX_VALUATORS]);
    int HW_Calibration;
#endif //_F_EVDEV_SUPPORT_ROTARY_
#ifdef _F_EVDEV_SUPPORT_SMARTRC_
    int rc_state;
#ifdef _F_SMART_RC_CHG_KBD_SRC_DEV_
    int (*extra_input_process) (InputInfoPtr *ppInfo, struct input_event *ev);
#else
    int (*extra_input_process) (InputInfoPtr pInfo, struct input_event *ev);
#endif
    void (*origin_input_process) (InputInfoPtr pInfo, struct input_event *ev);
#endif //_F_EVDEV_SUPPORT_SMARTRC_

#ifdef _F_PROXY_DEVICE_ENABLED_
    Bool proxy_enabled;
    Bool b_proxy_device;
    InputInfoPtr proxy_device;
#endif //_F_PROXY_DEVICE_ENABLED_
} EvdevRec, *EvdevPtr;

/* Event posting functions */
void EvdevQueueKbdEvent(InputInfoPtr pInfo, struct input_event *ev, int value);
void EvdevQueueButtonEvent(InputInfoPtr pInfo, int button, int value);
void EvdevQueueProximityEvent(InputInfoPtr pInfo, int value);
#ifdef MULTITOUCH
void EvdevQueueTouchEvent(InputInfoPtr pInfo, unsigned int touch,
                          ValuatorMask *mask, uint16_t type);
#endif
void EvdevPostButtonEvent(InputInfoPtr pInfo, int button, enum ButtonAction act);
void EvdevQueueButtonClicks(InputInfoPtr pInfo, int button, int count);
void EvdevPostRelativeMotionEvents(InputInfoPtr pInfo, int num_v, int first_v,
				   int v[MAX_VALUATORS]);
void EvdevPostAbsoluteMotionEvents(InputInfoPtr pInfo, int num_v, int first_v,
				   int v[MAX_VALUATORS]);
unsigned int EvdevUtilButtonEventToButtonNumber(EvdevPtr pEvdev, int code);

/* Middle Button emulation */
int  EvdevMBEmuTimer(InputInfoPtr);
BOOL EvdevMBEmuFilterEvent(InputInfoPtr, int, BOOL);
void EvdevMBEmuWakeupHandler(pointer, int, pointer);
void EvdevMBEmuBlockHandler(pointer, struct timeval**, pointer);
void EvdevMBEmuPreInit(InputInfoPtr);
void EvdevMBEmuOn(InputInfoPtr);
void EvdevMBEmuFinalize(InputInfoPtr);

/* Third button emulation */
CARD32 Evdev3BEmuTimer(OsTimerPtr timer, CARD32 time, pointer arg);
BOOL Evdev3BEmuFilterEvent(InputInfoPtr, int, BOOL);
void Evdev3BEmuPreInit(InputInfoPtr pInfo);
void Evdev3BEmuOn(InputInfoPtr);
void Evdev3BEmuFinalize(InputInfoPtr);
void Evdev3BEmuProcessRelMotion(InputInfoPtr pInfo, int dx, int dy);
void Evdev3BEmuProcessAbsMotion(InputInfoPtr pInfo, ValuatorMask *vals);

/* Mouse Wheel emulation */
void EvdevWheelEmuPreInit(InputInfoPtr pInfo);
BOOL EvdevWheelEmuFilterButton(InputInfoPtr pInfo, unsigned int button, int value);
BOOL EvdevWheelEmuFilterMotion(InputInfoPtr pInfo, struct input_event *pEv);

/* Draglock code */
void EvdevDragLockPreInit(InputInfoPtr pInfo);
BOOL EvdevDragLockFilterEvent(InputInfoPtr pInfo, unsigned int button, int value);

void EvdevMBEmuInitProperty(DeviceIntPtr);
void Evdev3BEmuInitProperty(DeviceIntPtr);
void EvdevWheelEmuInitProperty(DeviceIntPtr);
void EvdevDragLockInitProperty(DeviceIntPtr);
void EvdevAppleInitProperty(DeviceIntPtr);
#ifdef _F_EVDEV_SUPPORT_ROTARY_
void EvdevRotaryInit(DeviceIntPtr device);
void EvdevRotaryUnInit(EvdevPtr pEvdev);
#endif //_F_EVDEV_SUPPORT_ROTARY_
#ifdef _F_EVDEV_SUPPORT_GAMEPAD
static void EvdevMappingGamepadAbsToKey(InputInfoPtr pInfo,  struct input_event *ev);
static void EvdevMappingGamepadKeyToKey(InputInfoPtr pInfo,  struct input_event *ev);
static int EvdevIsGamePad(InputInfoPtr pInfo);
#endif//_F_EVDEV_SUPPORT_GAMEPAD
static void EvdevProcessEvent(InputInfoPtr pInfo, struct input_event *ev);
#ifdef _F_USE_DEFAULT_XKB_RULES_
void EvdevGetXkbRules(DeviceIntPtr device, XkbRMLVOSet * rmlvo);
#endif //_F_USE_DEFAULT_XKB_RULES_
#ifdef _F_EVDEV_SUPPORT_SMARTRC_
void EvdevRCInit(DeviceIntPtr device);
#endif //_F_EVDEV_SUPPORT_SMARTRC_
#ifdef _F_PROXY_DEVICE_ENABLED_
void EvdevProxyInit(DeviceIntPtr device);
#endif //_F_PROXY_DEVICE_ENABLED_
#endif