/*
 *
 * xserver-xorg-input-evdev
 *
 * Contact: Sung-Jin Park <sj76.park@samsung.com>
 *          Sangjin LEE <lsj119@samsung.com>
 *          Jeonghyun Kang <jhyuni.kang@samsung.com>
 *
 * Copyright (c) 2000 - 2014 Samsung Electronics Co., Ltd. All rights reserved.
 * Copyright © 2004-2008 Red Hat, Inc.
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
 *	Peter Hutterer (peter.hutterer@redhat.com)
 *	Oliver McFadden (oliver.mcfadden@nokia.com)
 */

#ifdef _F_EVDEV_SUPPORT_SMARTRC_
 #ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <evdev.h>
#include <evdev-properties.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <exevents.h>
#include <xf86.h>
#include <xf86Xinput.h>
#include <X11/Xatom.h>

#define ABS(x) (((x) < 0) ? -(x) : (x))
#define XI_PROP_TOUCH_ON "Evdev SmartRC Moved"
#define XI_PROP_SMARTRC_DISCANCE_MOVED "Evdev SmartRC Distance Moved"
#define XI_PROP_START_CALCULATE_DISTANCE "Start Calcualting Distance"

#define IF_KEY_CODE_TOUCH (ev->code == BTN_EXTRA || ev->code == BTN_SIDE)
#define IF_NOT_KEY_CODE_TOUCH (ev->code != BTN_EXTRA && ev->code != BTN_SIDE)

static Atom atomDistanceMoved;
static Atom atomCalculateDistance;
static Atom prop_move_start;
static int calculateDistance=0;
#define INITIAL_CURSOR_DISTANCE_TO_MOVE 0

#ifdef _F_SMART_RC_CHG_KBD_SRC_DEV_
static InputInfoPtr pSmartRCKbdDeviceInfo = NULL;
#endif
extern int block_motion_device;

enum device_state
{
    STATE_NONE=0,
    STATE_NORMAL,
    RC_STATE_DR_KEY,
    RC_STATE_AIR_TOUCH
};

enum abs_move_state
{
    ABS_MOVE_END=0,
    ABS_MOVE_ON,
    ABS_MOVE_START
};

enum scroll_direction_state
{
    SCROLL_NONE=0,
    SCROLL_VERTICAL,
    SCROLL_HORIZEN
};

static void EvdevRCBlockHandler(pointer data, OSTimePtr pTimeout, pointer pRead);
static void EvdevRCBlockHandlerFor2014(pointer data, OSTimePtr pTimeout, pointer pRead);
#ifdef _F_SMART_RC_CHG_KBD_SRC_DEV_
static int EvdevRCProcessEvent(InputInfoPtr *ppInfo, struct input_event *ev);
#else
static int EvdevRCProcessEvent(InputInfoPtr pInfo, struct input_event *ev);
#endif
int EvdevRCProcessNormalState(InputInfoPtr pInfo, struct input_event *ev);
int EvdevRCProcessDRKeyState(InputInfoPtr pInfo, struct input_event *ev);
void EvdevRCWheelScroll(InputInfoPtr pInfo, struct input_event *ev);
int EvdevRCProcessAirTouchState(InputInfoPtr pInfo, struct input_event *ev, int check_boundary);
int EvdevRCIsSwipe(struct input_event *ev);
void EvdevRCSendEvent(InputInfoPtr pInfo, int type, int code, int value);
int EvdevRCIsBoundary(struct input_event *ev, int rc_range);
static int EvdevSetProperty(DeviceIntPtr dev, Atom atom,
                            XIPropertyValuePtr val, BOOL checkonly);

typedef struct _tagRCState
{
    int velocity;
    int velo_time;
    int move_state;
    BOOL is_touch_on;
    BOOL isSmartRC_15;
    Atom atomTouchOn;
}RCState, *RCStatePtr;

RCState rcstate;

static void
EvdevRCBlockHandler(pointer data, OSTimePtr pTimeout, pointer pRead)
{
    InputInfoPtr pInfo = (InputInfoPtr)data;
    int rc;

    RemoveBlockAndWakeupHandlers(EvdevRCBlockHandler,
                                 (WakeupHandlerProcPtr)NoopDDA,
                                 data);

    ErrorF("15 RCBlock Handler Called, [%d] is_touch_on: %d\n", pInfo->dev->id, rcstate.is_touch_on);

    if (rcstate.is_touch_on == TRUE)
    {
        rc = XIChangeDeviceProperty(pInfo->dev, rcstate.atomTouchOn, XA_INTEGER, 8,
				PropModeReplace, 1, &rcstate.is_touch_on, TRUE);

        // In case of 2015 smartRC, cursor is enable when touch on device.  (prop_move_start is set to "1")
        if (block_motion_device == 0)
           rc = XIChangeDeviceProperty(pInfo->dev, prop_move_start, XA_INTEGER, 8,
                 PropModeReplace, 1, &rcstate.is_touch_on, TRUE);
    }
    else
        rc = XIDeleteDeviceProperty(pInfo->dev, rcstate.atomTouchOn, TRUE);

    if (rc != Success)
    {
        xf86IDrvMsg(pInfo, X_ERROR, "[%s] Failed to change device property (id:%d, prop=%d)\n", __FUNCTION__, pInfo->dev->id, (int)rcstate.atomTouchOn);
    }
}

static void
EvdevRCBlockHandlerFor2014(pointer data, OSTimePtr pTimeout, pointer pRead)
{
    InputInfoPtr pInfo = (InputInfoPtr)data;
    int rc;

    RemoveBlockAndWakeupHandlers(EvdevRCBlockHandlerFor2014,
                                 (WakeupHandlerProcPtr)NoopDDA,
                                 data);

    ErrorF("14 RCBlock Handler Called, [%d] is_touch_on: %d\n", pInfo->dev->id, rcstate.is_touch_on);

    if (rcstate.is_touch_on == TRUE)
    {
        rc = XIChangeDeviceProperty(pInfo->dev, rcstate.atomTouchOn, XA_INTEGER, 8,
                    PropModeReplace, 1, &rcstate.is_touch_on, TRUE);
    }
    else
        rc = XIDeleteDeviceProperty(pInfo->dev, rcstate.atomTouchOn, TRUE);

    if (rc != Success)
    {
        xf86IDrvMsg(pInfo, X_ERROR, "[%s] Failed to change device property (id:%d, prop=%d)\n", __FUNCTION__, pInfo->dev->id, (int)rcstate.atomTouchOn);
    }
}

#ifdef _F_SMART_RC_CHG_KBD_SRC_DEV_
static int EvdevRCProcessEvent(InputInfoPtr *ppInfo, struct input_event *ev)
#else
static int EvdevRCProcessEvent(InputInfoPtr pInfo, struct input_event *ev)
#endif
{
#ifdef _F_SMART_RC_CHG_KBD_SRC_DEV_
    InputInfoPtr pInfo = *ppInfo;
#endif
    EvdevPtr pEvdev = pInfo->private;
    int res = 0;
    // Start of Coord Cal
    static int mismatchX = 0, mismatchY = 0;

    if( rcstate.isSmartRC_15 == TRUE && ev->type == EV_ABS )
    {
       if(ev->code == ABS_X)
       {
          int calX = ev->value - pInfo->dev->spriteInfo->sprite->hot.x;
          if(calX == 0)
          {
             mismatchX = 0;
             return 0;
          }
          if(++mismatchX < 10)
          {
             return 0;
          }
          mismatchX = 0;
          ev->type = EV_REL;
          ev->code = REL_X;
          ev->value = calX;
       }
       if(ev->code == ABS_Y)
       {
          int calY = ev->value - pInfo->dev->spriteInfo->sprite->hot.y;
          if(calY == 0)
          {
             mismatchY = 0;
             return 0;
          }
          if(++mismatchY < 10)
          {
             return 0;
          }
          mismatchY = 0;
          ev->type = EV_REL;
          ev->code = REL_Y;
          ev->value = calY;
       }
    }
    // End of Coord Cal

    if (pEvdev->rel_move_ack == 3 || block_motion_device == 1)
    {
	    if (ev->type == EV_REL || ev->type == EV_ABS)
	    {
		    return res;
	    }
	    if (ev->type == EV_KEY && ev->code == BTN_LEFT)
	    {
		    ev->code = KEY_ENTER;
	    }
    }
    
    if (ev->type == EV_KEY && ev->code == BTN_EXTRA)
    {
        if (ev->value == 0)
        {
            rcstate.move_state = ABS_MOVE_END;
            rcstate.is_touch_on = FALSE;
            rcstate.isSmartRC_15 = FALSE;
        }
        else if (ev->value == 1)
        {
            rcstate.move_state = ABS_MOVE_ON;
            rcstate.is_touch_on = TRUE;
            rcstate.isSmartRC_15 = TRUE;
        }
		OsBlockSIGIO();
        RegisterBlockAndWakeupHandlers(EvdevRCBlockHandler ,(WakeupHandlerProcPtr) NoopDDA, pInfo);
		OsReleaseSIGIO(); 
    }
    else if (rcstate.move_state == ABS_MOVE_ON && ev->type == EV_SYN)
    {
        rcstate.move_state = ABS_MOVE_START;
    }


    if (ev->type == EV_KEY && ev->code == BTN_SIDE)
    {
        if (ev->value == 0)
        {
            rcstate.move_state = ABS_MOVE_END;
            rcstate.is_touch_on = FALSE;
        }
        else if (ev->value == 1)
        {
            rcstate.move_state = ABS_MOVE_ON;
            rcstate.is_touch_on = TRUE;
        }
	OsBlockSIGIO();
        RegisterBlockAndWakeupHandlers(EvdevRCBlockHandlerFor2014 ,(WakeupHandlerProcPtr) NoopDDA, pInfo);
    	OsReleaseSIGIO(); 
    }
    else if (rcstate.move_state == ABS_MOVE_ON && ev->type == EV_SYN)
    {
        rcstate.move_state = ABS_MOVE_START;
    }

    switch (pEvdev->rc_state)
    {
        case STATE_NORMAL:
            res = EvdevRCProcessNormalState(pInfo, ev);
            break;
        case RC_STATE_DR_KEY:
            res = EvdevRCProcessDRKeyState(pInfo, ev);
            break;
        case RC_STATE_AIR_TOUCH:
            res = EvdevRCProcessAirTouchState(pInfo, ev, 1);
            break;
        default:
            break;
    }

    if (ev->type == EV_KEY && IF_KEY_CODE_TOUCH)
        return 0;
    else 
#ifdef _F_SMART_RC_CHG_KBD_SRC_DEV_
    {
        if (pEvdev->rc_state == RC_STATE_DR_KEY)
        {
            if(pSmartRCKbdDeviceInfo) *ppInfo = pSmartRCKbdDeviceInfo;
        }
    }
#endif
    return res;
}

int EvdevRCProcessNormalState(InputInfoPtr pInfo, struct input_event *ev)
{
    EvdevPtr pEvdev = pInfo->private;
    int res = 0;

    if (ev->type == EV_REL || ev->type == EV_ABS)
        return res;

    if (ev->type == EV_KEY)
    {
        if (IF_KEY_CODE_TOUCH && ev->value == 1)
        {
            pEvdev->rc_state = RC_STATE_AIR_TOUCH;
            res = EvdevRCProcessAirTouchState(pInfo, ev, 1);
        }
        else if ( (ev->code == KEY_UP || ev->code == KEY_LEFT || ev->code == KEY_DOWN || ev->code == KEY_RIGHT || ev->code == KEY_ENTER)
            && (ev->value == 1))
        {
            pEvdev->rc_state = RC_STATE_DR_KEY;
            res = EvdevRCProcessDRKeyState(pInfo, ev);
        }
        else if (IF_KEY_CODE_TOUCH && ev->value == 0)
        {
            return res;
        }
        else
            res = 1;
    }
    else
        res = 1;

    return res;
}

int EvdevRCProcessDRKeyState(InputInfoPtr pInfo, struct input_event *ev)
{
    EvdevPtr pEvdev = pInfo->private;
    int res = 0;
    static int pressed_flag = 0;
    static BOOL extra_pressed = FALSE;
    static int last_key = -1;

    if (ev->type == EV_KEY && ev->code == BTN_LEFT)
    {
        ev->code = KEY_ENTER;
    }

    if (ev->type == EV_KEY && IF_NOT_KEY_CODE_TOUCH)
    {
        if ( (last_key >= 0) && (ev->code != last_key))
        {
            return res;
        }
        if (ev->value == 1) {
            pressed_flag++;
            last_key = ev->code;
        }
        else {
            pressed_flag--;
            last_key = -1;
            if (pressed_flag<0)
                pressed_flag = 0;
        }
    }
    else if (ev->type == EV_KEY && IF_KEY_CODE_TOUCH)
    {
        if (pressed_flag == 0)
        {
            res = EvdevRCProcessAirTouchState(pInfo, ev, 0);
            extra_pressed = FALSE;
        }
        else if (ev->value == 1)
        {
            extra_pressed = TRUE;
        }
        else
        {
            extra_pressed = FALSE;
        }
    }
    if (extra_pressed == TRUE && (ev->type == EV_REL || ev->type == EV_ABS))
    {
       return res;
    }

    if ((pressed_flag==0) && block_motion_device == 0)
    {
       if (rcstate.isSmartRC_15 == TRUE)
       {
          pEvdev->rc_state = RC_STATE_AIR_TOUCH;
          res = EvdevRCProcessAirTouchState(pInfo, ev, 0);
          return res;
       }
       else
       {
          // calculate distance to change air touch mode(2014 smartRC)
          if(EvdevRCIsBoundary(ev, 150))
          {
             pEvdev->rc_state = RC_STATE_AIR_TOUCH;
             res = EvdevRCProcessAirTouchState(pInfo, ev, 0);
             return res;
          }
       }
    }

    if (ev->type == EV_REL || ev->type == EV_ABS)
    {
        return res;
    }
    else
        res = 1;

    return res;

}

int EvdevRCProcessAirTouchState(InputInfoPtr pInfo, struct input_event *ev, int check_boundary)
{
    EvdevPtr pEvdev = pInfo->private;
    int res = 0;
    int rc;
    int temp_res;

    if( calculateDistance && EvdevRCIsBoundary(ev, INITIAL_CURSOR_DISTANCE_TO_MOVE))
    {
        rc = XIChangeDeviceProperty(pInfo->dev, atomDistanceMoved, XA_INTEGER, 8,
            PropModeReplace, 1, &pEvdev->rel_move_status, TRUE);
        if (rc != Success)
            ErrorF("[EvdevRCProcessAirTouchState] Failed to change atomDistanceMoved\n");
        calculateDistance=0;
    }

    if (ev->type == EV_KEY && (IF_NOT_KEY_CODE_TOUCH && ev->code != BTN_LEFT))
        return res;

    if (ev->type == EV_REL && (ev->code == REL_HWHEEL || ev->code == REL_WHEEL))
    {
        res = 1;
        return res;
    }

    if (ev->type == EV_ABS || (ev->type == EV_KEY && IF_KEY_CODE_TOUCH) || ev->type == EV_SYN)
    {
        EvdevRCWheelScroll(pInfo, ev);
    }

    if (ev->type == EV_KEY && IF_KEY_CODE_TOUCH && ev->value == 0)
    {
        temp_res = EvdevRCIsBoundary(ev, 0);
        if (pEvdev->rc_state != RC_STATE_DR_KEY)
            pEvdev->rc_state = STATE_NORMAL;
        if (temp_res == 1)
            res = temp_res;
            return res;
    }
    else if (check_boundary && ev->type == EV_REL)
    {
        if (!EvdevRCIsBoundary(ev, 0))
            return res;
        else
        {
            if (block_motion_device == 0)
                pEvdev->rc_state = RC_STATE_AIR_TOUCH;
        }
    }

    res = 1;

    return res;
}

void EvdevRCWheelScroll(InputInfoPtr pInfo, struct input_event *ev)
{
    static int before_x=-1, before_y=-1;
    static int before_x_utime = -1, before_y_utime = -1;
    static int before_x_time = -1, before_y_time = -1;

    int time_diff = 0;
    float velo_x=0;

    float velo_y = 0;
    static int velo_y_count=0, velo_x_count = 0;

    static int wheel_direction_flag = SCROLL_NONE;

    if (ev->type == EV_REL || ev->type == EV_SYN)
        return;
    if (ev->type == EV_KEY && IF_NOT_KEY_CODE_TOUCH)
        return;

    if (ev->type == EV_KEY)
        wheel_direction_flag = SCROLL_NONE;

    if (rcstate.move_state == ABS_MOVE_ON && ev->type == EV_ABS)
    {
        velo_y_count=0;
        if (ev->code == ABS_X)
        {
            before_x = ev->value;
            before_x_time = ev->time.tv_sec;
            before_x_utime = ev->time.tv_usec;

            if ((before_x < 150) || (350 < before_x))
            {
                wheel_direction_flag = SCROLL_HORIZEN;
            }
            return;
        }
        else if (ev->code == ABS_Y)
        {
            before_y = ev->value;
            before_y_time = ev->time.tv_sec;
            before_y_utime = ev->time.tv_usec;

            if ((before_y < 150) || (350 < before_y))
            {
                wheel_direction_flag = SCROLL_VERTICAL;
            }
            return;
        }
    }

    if (ev->type == EV_ABS && ev->code == ABS_X && wheel_direction_flag != SCROLL_VERTICAL)
    {
        time_diff = (ev->time.tv_sec - before_x_time) * 1000 + (ev->time.tv_usec - before_x_utime)/1000;
        if (time_diff == 0)
        {
            ErrorF("before and current events had same event time...\n");
            return;
        }

        velo_x = (float)((ABS(before_x - ev->value)) / time_diff);

        if (velo_x > 1)
        {
            velo_x_count = velo_x_count + (int)velo_x;
        }

        if (velo_x_count > 2)
        {
            wheel_direction_flag = SCROLL_HORIZEN;
            velo_x_count = (velo_x_count / 4);
            if (velo_x_count > 2)
                velo_x_count = velo_x_count * 2;

            EvdevRCSendEvent(pInfo, EV_REL, REL_HWHEEL, ((before_x - ev->value) < 0)? -velo_x_count: velo_x_count);
            velo_x_count = 0;
        }

        before_x = ev->value;
        before_x_time = ev->time.tv_sec;
        before_x_utime = ev->time.tv_usec;
    }
    else if (ev->type == EV_ABS && ev->code == ABS_Y && wheel_direction_flag != SCROLL_HORIZEN)
    {
        time_diff = (ev->time.tv_sec - before_y_time) * 1000 + (ev->time.tv_usec - before_y_utime)/1000;
        if (time_diff == 0)
        {
            ErrorF("before and current events had same event time...\n");
            return;
        }
        velo_y = (float)((ABS(before_y - ev->value)) / time_diff);

        if (velo_y > 1)
        {
            velo_y_count = velo_y_count + (int)velo_y;
        }

        if (velo_y_count > 2)
        {
            wheel_direction_flag = SCROLL_VERTICAL;
            velo_y_count = (velo_y_count / 4);
            if (velo_y_count > 2)
                velo_y_count = velo_y_count * 2;

            EvdevRCSendEvent(pInfo, EV_REL, REL_WHEEL, ((before_y - ev->value) < 0)? -velo_y_count: velo_y_count);
            velo_y_count = 0;
        }

        before_y = ev->value;
        before_y_time = ev->time.tv_sec;
        before_y_utime = ev->time.tv_usec;
    }
}

int EvdevRCIsSwipe(struct input_event *ev)
{
    int res = 0;
    static int base_point_x = -1;
    static int base_point_y = -1;

    if (ev->type == EV_KEY && IF_KEY_CODE_TOUCH)
    {
            base_point_x = -1;
            base_point_y = -1;
    }

    if (ev->type == EV_ABS)
    {
        if (ev->code == ABS_X)
        {
            if (base_point_x == -1)
            {
                base_point_x = ev->value;
            }
            else if (ABS(base_point_x - ev->value) > 300)
            {
                res = 1;
            }
        }
        else if (ev->code == ABS_Y)
        {
            if (base_point_y == -1)
            {
                base_point_y = ev->value;
            }
            else if (ABS(base_point_y - ev->value) > 300)
            {
                res = 1;
            }
        }
    }

    return res;
}

void EvdevRCSendEvent(InputInfoPtr pInfo, int type, int code, int value)
{
    EvdevPtr pEvdev = pInfo->private;
    struct input_event ev;

    ev.type = type;
    ev.code = code;
    ev.value = value;

    pEvdev->origin_input_process(pInfo, &ev);
}

static int
EvdevSetProperty(DeviceIntPtr dev, Atom atom, XIPropertyValuePtr val,
                 BOOL checkonly)
{
    InputInfoPtr pInfo  = dev->public.devicePrivate;
    EvdevPtr     pEvdev = pInfo->private;
 
    if (atom == atomCalculateDistance )
    {
        calculateDistance=1;
    }
    return 0;
}
int EvdevRCIsBoundary(struct input_event *ev, int rc_range)
{
    static int x_diff=0, y_diff = 0;
    int res = 0;

    if (ev->type == EV_KEY && IF_KEY_CODE_TOUCH)
    {
        x_diff = y_diff = 0;
    }

    if (ev->type == EV_REL)
    {
        if (ev->code == REL_X)
            x_diff = x_diff + ev->value;
        if (ev->code == REL_Y)
            y_diff = y_diff + ev->value;
    }

    if ((ABS(x_diff) > rc_range) || (ABS(y_diff) > rc_range))
    {
        res = 1;
    }
    return res;
}

void EvdevRCInit(DeviceIntPtr device)
{
    InputInfoPtr pInfo;
    EvdevPtr pEvdev;

    pInfo = device->public.devicePrivate;
    pEvdev = pInfo->private;

    if (pEvdev->flags & EVDEV_SMART_RC)
    {
        ErrorF("[%s] id: %d, smart rc... hook \n", __FUNCTION__, device->id);
        pEvdev->extra_input_process = EvdevRCProcessEvent;
        pEvdev->rc_state = STATE_NORMAL;
        rcstate.velocity = 3;
        rcstate.atomTouchOn = MakeAtom(XI_PROP_TOUCH_ON, strlen(XI_PROP_TOUCH_ON), TRUE);
        atomDistanceMoved = MakeAtom(XI_PROP_SMARTRC_DISCANCE_MOVED,strlen(XI_PROP_SMARTRC_DISCANCE_MOVED), TRUE);
        atomCalculateDistance= MakeAtom(XI_PROP_START_CALCULATE_DISTANCE,strlen(XI_PROP_START_CALCULATE_DISTANCE), TRUE);
        prop_move_start = MakeAtom(XI_PROP_REL_MOVE_STATUS, strlen(XI_PROP_REL_MOVE_STATUS), TRUE);
    }
#ifdef _F_SMART_RC_CHG_KBD_SRC_DEV_
    else
    {
        if (!strncmp(pEvdev->name, AIR_TOUCH_MOUSE, strlen(AIR_TOUCH_MOUSE))) pSmartRCKbdDeviceInfo = pInfo;
    }
#endif
    XIRegisterPropertyHandler(device, EvdevSetProperty, NULL, NULL);
    return;
}
#endif //_F_EVDEV_SUPPORT_SMARTRC_