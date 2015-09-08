/*
 *
 * xserver-xorg-input-evdev
 *
 * Contact: Sung-Jin Park <sj76.park@samsung.com>
 *          Sangjin LEE <lsj119@samsung.com>
 *          Jeonghyun Kang <jhyuni.kang@samsung.com>
 *          Hyung-Joon Jeon <helius.jeon@samsung.com>
 *          Minsu Han <minsu81.han@samsung.com>
 *          YoungHoon Song <ysens.song@samsung.com>
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

#ifdef _F_EVDEV_SUPPORT_ROTARY_

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

#define IDLE 0
#define MOVING 1

#define SW_ROTARY_MAX 3600 // Mathematical rotary angular position limit (i.e. 360) scaled by CALCULATOR_VALUE.

#define DETENT_TOLERANCE 20
#define DETENT_TIMEOUT_TOLERANCE 30

#define DETENT_TIMEOUT 500
#define START_TIMEOUT 2000
#define RETURN_TIMEOUT 200

#define NO_VALUE 0
#define CALCULATOR_VALUE 10	//for 3600-base angle
#define DETENT_INTERVAL 150

#define NULL_DETENT 255 // driver-side constant for null-detent event
#define RIGHT_DETENT 1 // driver-side constant for detent event moving right(clockwise)
#define LEFT_DETENT -1 // driver-side constant for detent event moving left(counter-clockwise)
#define ZERO_DETENT 0 // driver-side constant for detent event moving away from and then towards detent position

#define HALL_LEFT 1
#define HALL_CENTER 2
#define HALL_RIGHT 3

#define HALLIC_DETENT_RIGHT 2 // kernel-side constant for detent event moving right(clockwise)
#define HALLIC_DETENT_LEFT -2 // kernel-side constant for detent event moving left(counter-clockwise)
#define HALLIC_MOVE_OUT 1 // kernel-side constant for move event away from detent position
#define HALLIC_MOVE_IN -1 // kernel-side constant for move event towards detent position

#define NO_DEVICE 0
#define OFM_DEVICE 1
#define HALL_DEVICE 2

#define ENABLE_TIMER 0
#define DELETE_TIMER 1

//#define __DETAIL_DEBUG__

#ifdef __DETAIL_DEBUG__
#define DetailDebugPrint LogMessageVerbSigSafe
#else
#define DetailDebugPrint(...)
#endif

static int hasRotary = NO_DEVICE;
static int hasDetent = NO_DEVICE;

static int hw_rotary_max; // HW rotary angular position limit scaled by CALCULATOR_VALUE.
static double kernel_angle_step;
static int return_tolerance;

static OsTimerPtr detent_event_timer = NULL;
static OsTimerPtr start_event_timer = NULL;
static int start_event_timer_flag = ENABLE_TIMER;


static int angle_pos_raw = 0; //angular position within kernel-side range of about 0~hw_rotary_max. It resets to 0 upon timer event.
static int angle_pos = 0; // angular position within driver-side full range of 0~SW_ROTARY_MAX. It does not reset to 0 upon timer event.
static int angle_pos_sub_cal = 0; // angular position within driver-side sub-range of -ROTATE_DETENT_INTERVAL~ROTATE_DETENT_INTERVAL. It resets to 0 upon timer event.
static unsigned char is_detent = 0;
static int detent_sum = 0;
static int state = IDLE;
static int temp_sub_cal = 0;
static int event_time = 0;
static int angle_delta_dir=0; // rotation direction
static int return_state = 0;

static int ignore_state = 0;
static int ignore_angle = 0;
static int return_ignore_state = 0;
static int return_ignore_angle = 0;
static int return_ignore_reset_condition = 0;

static int detent_hall_ic = 0;
static int detent_hall_ic_prev = 0;
static int hall_ic_count = -1;

static int setRotaryFlag = 0;
static int setDetentFlag = 0;

#ifdef _F_GESTURE_EXTENSION_

#define ROTARY_FRAME_SYNC_TIMEOUT 200
#define ROTARY_FRAME_SYNC_TOLERANCE 30

static OsTimerPtr rotary_frame_sync_timer = NULL;
static unsigned char is_touch_blocked = 0;

extern void mieqEnqueue(DeviceIntPtr pDev, InternalEvent *e);
void EvdevRotarySync(InputInfoPtr pInfo, MTSyncType sync)
{
    AnyEvent event;

    memset(&event, 0, sizeof(event));
    event.header = ET_Internal;
    event.type = ET_MTSync;
    event.length = sizeof(event);
    event.time = GetTimeInMillis();
    event.deviceid = pInfo->dev->id;
    event.sync = sync;
    mieqEnqueue (pInfo->dev, (InternalEvent*)&event);
}
static CARD32 EvdevRotarySyncTimerFinish(OsTimerPtr timer, CARD32 time, pointer arg)
{
    InputInfoPtr pInfo = (InputInfoPtr)arg;

    if (is_touch_blocked == 0)
    {
        TimerCancel(rotary_frame_sync_timer);
        return 0;
    }

    DetailDebugPrint(X_INFO, 1, "[EvdevRotarySyncTimerFinish] Touch is freed from blocking.\n");
    EvdevRotarySync(pInfo, ROTARY_FRAME_SYNC_END);
    is_touch_blocked = 0;

    return 0;
}
#endif

static CARD32
ResetValues(OsTimerPtr timer, CARD32 time, pointer arg)
{
    angle_pos_sub_cal = 0;
    angle_pos_raw = 0;
    detent_sum = 0;
    temp_sub_cal = 0;
    ignore_state = 0;
    ignore_angle = 0;
    angle_delta_dir = 0;
    return_ignore_state = 0;
    return_ignore_angle = 0;
    detent_hall_ic = 0;
    return_ignore_reset_condition = 0;
    hall_ic_count = -1;
    setRotaryFlag = 0;
    setDetentFlag = 0;

    state = IDLE;
    DetailDebugPrint(X_INFO, 1, "[ResetValues] Reset all values!\n");

    return 0;
}

static void
DeliverDetentEvent(InputInfoPtr pInfo, int type, int value)
{
    EvdevPtr pEvdev = pInfo->private;
    int detent_type;

    event_time = GetTimeInMillis();

    valuator_mask_set(pEvdev->vals, REL_X, value);
    valuator_mask_set(pEvdev->vals, REL_Y, event_time);

    if (type == 0)
        detent_type = 0;
    else
        detent_type = 1;

    valuator_mask_set(pEvdev->vals, REL_Z, detent_type);
    xf86PostMotionEventM(pInfo->dev, Relative, pEvdev->vals);

    detent_sum++;
    DetailDebugPrint(X_INFO, 1, "[Deliver PreDetent] X=%d, angle_pos_sub_cal=%d, detent=%d, time=%d\n", value, angle_pos_sub_cal, detent_type, event_time);
}

static CARD32
RotaryReset(OsTimerPtr timer, CARD32 time, pointer arg)
{
    InputInfoPtr pInfo = (InputInfoPtr)arg;

    EvdevPtr pEvdev = pInfo->private;

    if (start_event_timer_flag && (timer == start_event_timer))
    {
        start_event_timer_flag = ENABLE_TIMER;
        TimerCancel(start_event_timer);
        return 0;
    }

    if ((detent_sum != 0) && (return_ignore_reset_condition == 0) &&
            ((angle_pos_sub_cal <= DETENT_TIMEOUT_TOLERANCE && angle_pos_sub_cal >= -DETENT_TIMEOUT_TOLERANCE)))
    {
        DetailDebugPrint(X_INFO, 1, "[RotaryReset] angle_pos_sub_cal=%d, detent_sum=%d, time=%d. All values set to 0.\n", angle_pos_sub_cal, detent_sum, event_time);

        ResetValues(NULL, 1, NULL);
    }
    else
    {
        DetailDebugPrint(X_INFO, 1, "[No Reset] angle_pos_sub_cal=%d.\n", angle_pos_sub_cal);
        temp_sub_cal = angle_pos_sub_cal;

        if (return_ignore_state == 1)
        {
            ResetValues(NULL, 1, NULL);
            DetailDebugPrint(X_INFO, 1, "[Reset return ignore state] return_ignore_state=%d.\n", return_ignore_state);
            return;
        }

        {
            DetailDebugPrint(X_INFO, 1, "[Reset] Stop between detents !\n");

            if (detent_hall_ic > 0)
            {
                DetailDebugPrint(X_INFO, 1, "[Reset] Find hall ic event!Deliver detent event\n");
                ResetValues(NULL, 1, NULL);
                return;
            }
        }
    }

    if (detent_event_timer)
    {
        TimerCancel(detent_event_timer);
    }

    return 0;
}

static void EvdevRotaryProcessHallIcEvent(InputInfoPtr pInfo, int num_v, int first_v,
        int v[MAX_VALUATORS])
{
    EvdevPtr pEvdev = pInfo->private;
    int detent_count = 0;
    int move_count = 0;

    event_time = GetTimeInMillis();

    detent_hall_ic = valuator_mask_get(pEvdev->vals, REL_Z);
    switch(detent_hall_ic)
    {
        case HALLIC_DETENT_RIGHT:
            detent_count = RIGHT_DETENT;
            break;
        case HALLIC_DETENT_LEFT:
            detent_count = LEFT_DETENT;
            break;
        case HALLIC_MOVE_OUT:
            detent_count = ZERO_DETENT;
            move_count = 1;
            break;
        case HALLIC_MOVE_IN:
            detent_count = ZERO_DETENT;
            break;
        default:
            LogMessageVerbSigSafe(X_WARNING, 1, "[EvdevRotaryProcessHallIcEvent] Unknown Hall IC event! assuming HALLIC_MOVE_IN.\n");
            detent_count = ZERO_DETENT;
            break;
    }
    DetailDebugPrint(X_INFO, 1, "[HALL Only] REL_Z=%d, detent_count=%d, time=%d\n", detent_hall_ic, detent_count, event_time);

#ifdef _F_GESTURE_EXTENSION_
    if(move_count)
    {
        DetailDebugPrint(X_INFO, 1, "[EvdevRotaryProcessHallIcEvent] Rotary is moving...touch is blocked!\n");
        if(is_touch_blocked == 0)
        {
            EvdevRotarySync(pInfo, ROTARY_FRAME_SYNC_BEGIN);
            is_touch_blocked = 1;
        }
        rotary_frame_sync_timer = TimerSet(rotary_frame_sync_timer, 0, ROTARY_FRAME_SYNC_TIMEOUT, EvdevRotarySyncTimerFinish, pInfo);
        return;
    }
#endif

    if(!(DPMSPowerLevel == DPMSModeOff || DPMSPowerLevel == DPMSModeSuspend))
    {
#ifdef _F_GESTURE_EXTENSION_
        if(is_touch_blocked == 1)
        {
            EvdevRotarySync(pInfo, ROTARY_FRAME_SYNC_END);
            is_touch_blocked = 0;
        }
#endif

        valuator_mask_set(pEvdev->vals, REL_X, NO_VALUE);
        valuator_mask_set(pEvdev->vals, REL_Y, event_time);
        valuator_mask_set(pEvdev->vals, REL_Z, detent_count);
        xf86PostMotionEventM(pInfo->dev, Relative, pEvdev->vals);
        LogMessageVerbSigSafe(X_INFO, 1, "[Detent] Hall ic detent has been sent. raw_data=%d, time=%d, detent=%d \n", detent_hall_ic, event_time, detent_count);
    }
    else
        DetailDebugPrint(X_INFO, 1, "[No detent] DPMS off or suspend.\n");

    detent_hall_ic = 0;

}

static void EvdevRotaryProcessLegacyHallIcEvent(InputInfoPtr pInfo, int num_v, int first_v,
        int v[MAX_VALUATORS])
{
    EvdevPtr pEvdev = pInfo->private;
    int detent_count = 0;

    event_time = GetTimeInMillis();

    detent_hall_ic = valuator_mask_get(pEvdev->vals, REL_Z);

    valuator_mask_set(pEvdev->vals, REL_X, NO_VALUE);
    valuator_mask_set(pEvdev->vals, REL_Y, event_time);

    if (!detent_hall_ic_prev)
    {
        if (angle_pos_raw > 0)
        {
            detent_count = RIGHT_DETENT;
            LogMessageVerbSigSafe(X_INFO, 1, "[OFM+HALL] REL_Z=%d, angle_pos_raw=%d, detent_count=%d, time=%d\n", detent_hall_ic, angle_pos_raw, detent_count, event_time);
        }
        else if (angle_pos_raw < 0)
        {
            detent_count = LEFT_DETENT;
            LogMessageVerbSigSafe(X_INFO, 1, "[OFM+HALL] REL_Z=%d, angle_pos_raw=%d, detent_count=%d, time=%d\n", detent_hall_ic, angle_pos_raw, detent_count, event_time);
        }
    }

    // Only hall ic
    else
    {
        switch(detent_hall_ic)
        {
            case HALL_LEFT :
                if (detent_hall_ic_prev == HALL_CENTER)
                    detent_count = LEFT_DETENT;
                else if (detent_hall_ic_prev == HALL_RIGHT)
                    detent_count = RIGHT_DETENT;
                break;
            case HALL_CENTER :
                if (detent_hall_ic_prev == HALL_RIGHT)
                	detent_count = LEFT_DETENT;
                else if (detent_hall_ic_prev == HALL_LEFT)
                	detent_count = RIGHT_DETENT;
                break;
            case HALL_RIGHT :
                if (detent_hall_ic_prev == HALL_LEFT)
                	detent_count = LEFT_DETENT;
                else if (detent_hall_ic_prev == HALL_CENTER)
                	detent_count = RIGHT_DETENT;
                break;
            default :
                break;
        }

        DetailDebugPrint(X_INFO, 1, "[HALL Only] REL_Z_Prev=%d, REL_Z=%d, detent_count=%d, time=%d\n", detent_hall_ic_prev, detent_hall_ic, detent_count, event_time);
    }

    valuator_mask_set(pEvdev->vals, REL_Z, detent_count);

    if((!(DPMSPowerLevel == DPMSModeOff || DPMSPowerLevel == DPMSModeSuspend)) && (setRotaryFlag == 0))
    {
        xf86PostMotionEventM(pInfo->dev, Relative, pEvdev->vals);
#ifdef _F_GESTURE_EXTENSION_
        if(is_touch_blocked == 0)
        {
            EvdevRotarySync(pInfo, ROTARY_FRAME_SYNC_BEGIN);
            is_touch_blocked = 1;
        }
        rotary_frame_sync_timer = TimerSet(rotary_frame_sync_timer, 0, ROTARY_FRAME_SYNC_TIMEOUT, EvdevRotarySyncTimerFinish, pInfo);
#endif
        setDetentFlag = 1;
        LogMessageVerbSigSafe(X_INFO, 1, "[Rotary] Hall ic detent has been sent. setRotaryFlag=%d, setDetentFlag=%d, raw_data=%d, time=%d, detent=%d \n", setRotaryFlag, setDetentFlag, detent_hall_ic, event_time, detent_count);
    }
    else
        DetailDebugPrint(X_INFO, 1, "[Rotary] DPMS off or suspend or rotary detent has already been sent. setRotaryFlag=%d, setDetentFlag=%d \n", setRotaryFlag, setDetentFlag);

    detent_hall_ic_prev = detent_hall_ic;
    detent_hall_ic = 0;
    angle_pos_raw = 0;

    return;
}

static void EvdevRotaryProcessOfmEvent(InputInfoPtr pInfo, int num_v, int first_v,
        int v[MAX_VALUATORS])
{
    int angle_delta_raw=0; // kernel-side rotation angle scaled by CALCULATOR_VALUE
    int angle_delta_cal=0; // actual rotation angle scaled by CALCULATOR_VALUE, in integer format

    int detent_count=0;
    int i;

    EvdevPtr pEvdev = pInfo->private;

    DeviceIntPtr dev;
    InputInfoPtr pInfoDetent;
    EvdevPtr pEvdevDetent;

    event_time = GetTimeInMillis();

    angle_delta_raw = valuator_mask_get(pEvdev->vals, REL_X) * CALCULATOR_VALUE;
    angle_delta_cal = (int)((angle_pos_raw + angle_delta_raw) * (double)kernel_angle_step) - (int)(angle_pos_raw * (double)kernel_angle_step);

    angle_pos_raw += angle_delta_raw;
    if (angle_pos_raw >= hw_rotary_max || angle_pos_raw <= -hw_rotary_max)
        angle_pos_raw = angle_pos_raw % hw_rotary_max;

    angle_pos_sub_cal += angle_delta_cal;

    angle_pos += angle_delta_cal;
    if( angle_pos > SW_ROTARY_MAX-1 )
        angle_pos = angle_pos % SW_ROTARY_MAX;
    else if( angle_pos < 0 )
        angle_pos = SW_ROTARY_MAX + angle_pos % SW_ROTARY_MAX;

    if (hall_ic_count >= 0)
    {
        hall_ic_count++;
        if (hall_ic_count > 2)
        {
            DetailDebugPrint(X_INFO, 1, "[Detent hall ic count up] hall_ic_count=%d\n", hall_ic_count);
            start_event_timer = TimerSet(start_event_timer, 0, 1, RotaryReset, pInfo);
            hall_ic_count = -1;
            return;
        }
    }

    if (ignore_state == 1)
    {
        ignore_angle += angle_delta_cal;

        if (ignore_angle * angle_delta_cal < 0)
        {
            if ((ignore_angle < DETENT_INTERVAL - DETENT_TOLERANCE) && (ignore_angle > -DETENT_INTERVAL + DETENT_TOLERANCE))
            {
                DetailDebugPrint(X_INFO, 1, "[Opposite direction] ignore_angle=%d, angle_delta_cal=%d, Reset!!\n", ignore_angle, angle_delta_cal);
                ignore_state = 0;
                ignore_angle = 0;

                detent_event_timer = TimerSet(detent_event_timer, 0, 1, ResetValues, NULL);
            }
            else
            {
                DetailDebugPrint(X_INFO, 1, "[Opposite direction, inside threshold] ignore_angle=%d, angle_delta_cal=%d, No Reset.\n", ignore_angle, angle_delta_cal);
            }
            return;
        }
        else
        {
            if ((ignore_angle >= DETENT_INTERVAL + DETENT_TIMEOUT_TOLERANCE) || (ignore_angle <= -DETENT_INTERVAL - DETENT_TIMEOUT_TOLERANCE))
            {
                DetailDebugPrint(X_INFO, 1, "[No Deliver] ignore_angle=%d, Reset!!\n", ignore_angle);
                ignore_state = 0;
                ignore_angle = 0;

                detent_event_timer = TimerSet(detent_event_timer, 0, 1, ResetValues, NULL);
            }

            else
                DetailDebugPrint(X_INFO, 1, "[No Deliver] raw_X=%d, kernel_angle=%d, X=%d, ignore_angle=%d, ignore_state=%d, detent=%d, move_sum=%d\n", angle_delta_raw, angle_pos_raw, angle_delta_cal, ignore_angle, ignore_state, NULL_DETENT, angle_pos_sub_cal);
            return;
        }
    }

    if (return_ignore_state  == 1)
    {
        return_ignore_angle += angle_delta_cal;

        if ((return_ignore_angle < DETENT_TIMEOUT_TOLERANCE) && (return_ignore_angle > -DETENT_TIMEOUT_TOLERANCE))
        {
            DetailDebugPrint(X_INFO, 1, "[No Deliver] return direction, inside threshold, return_ignore_angle=%d, angle_delta_cal=%d\n", return_ignore_angle, angle_delta_cal);
            return;
        }
        else
            return_ignore_reset_condition = 1;
    }

    if( angle_delta_cal == 0 )
    {
        DetailDebugPrint(X_INFO, 1, "[No Detent0] raw_X=%d, kernel_angle=%d, X=0, angle=%d\n", angle_delta_raw, angle_pos_raw, angle_pos);
    }
    else // angle_delta_cal != 0
    {
        // angle_pos_sub_cal 150 ~ -150
        if( angle_pos_sub_cal >= (DETENT_INTERVAL - DETENT_TIMEOUT_TOLERANCE) ||
                angle_pos_sub_cal <= (-DETENT_INTERVAL + DETENT_TIMEOUT_TOLERANCE))
        {
            is_detent = 1; // Set detent flag

            if( angle_pos_sub_cal >= DETENT_INTERVAL ) // right direction, NO need to consider tolerance
            {
                detent_count = angle_pos_sub_cal / DETENT_INTERVAL;
            }
            else if( angle_pos_sub_cal <= -DETENT_INTERVAL ) // left direction, NO need to consider tolerance
            {
                detent_count = -angle_pos_sub_cal / DETENT_INTERVAL;
            }
            else // right or left direction, NEED to consider tolerance!
            {
                detent_count = 1;
            }
            detent_sum += detent_count;

        }

        valuator_mask_set(pEvdev->vals, REL_X, angle_delta_cal);
        valuator_mask_set(pEvdev->vals, REL_Y, event_time);

        if( is_detent == 1 ) // detent
        {
            ignore_state = 1;
            ignore_angle = angle_pos_sub_cal % DETENT_INTERVAL;
            detent_hall_ic = 0;
            hall_ic_count = -1;

            if (angle_pos_sub_cal > 0)
                detent_count = RIGHT_DETENT;
            else
                detent_count = LEFT_DETENT;

            if (!setDetentFlag)
            {
                for (dev = inputInfo.keyboard; dev; dev = dev->next)
                {
                    if (!strncmp(dev->name, "tizen_detent", strlen(dev->name)))
                    {
                        pInfoDetent = dev->public.devicePrivate;
                        pEvdevDetent = pInfoDetent->private;
                        break;
                    }
                }

                valuator_mask_set(pEvdevDetent->vals, REL_X, NO_VALUE);
                valuator_mask_set(pEvdevDetent->vals, REL_Y, event_time);
                valuator_mask_set(pEvdevDetent->vals, REL_Z, detent_count);
                xf86PostMotionEventM(pInfoDetent->dev, Relative, pEvdevDetent->vals);
#ifdef _F_GESTURE_EXTENSION_
                if(is_touch_blocked == 0)
                {
                    EvdevRotarySync(pInfo, ROTARY_FRAME_SYNC_BEGIN);
                    is_touch_blocked = 1;
                }
                rotary_frame_sync_timer = TimerSet(rotary_frame_sync_timer, 0, ROTARY_FRAME_SYNC_TIMEOUT, EvdevRotarySyncTimerFinish, pInfo);
#endif
                setRotaryFlag = 1;
                LogMessageVerbSigSafe(X_INFO, 1, "[Rotary] OFM detent has been sent. setRotaryFlag=%d, setDetentFlag=%d, device name=%s \n", setRotaryFlag, setDetentFlag, dev->name);
            }
            else
                DetailDebugPrint(X_INFO, 1, "[Rotary] Hall ic detent has already been sent. setRotaryFlag=%d, setDetentFlag=%d \n", setRotaryFlag, setDetentFlag);

            if (state == IDLE)
            {
                if (angle_delta_cal > 0)
                {
                    angle_delta_dir = 1;
                }
                else if (angle_delta_cal < 0)
                {
                    angle_delta_dir = -1;
                }
                start_event_timer = TimerSet(start_event_timer, 0, START_TIMEOUT, RotaryReset, pInfo);
                DetailDebugPrint(X_INFO, 1, "[Detent] START! REL_Y = %d\n", event_time);
                state = MOVING;
            }

            if (angle_delta_dir == 1)
                angle_pos_sub_cal -= (DETENT_INTERVAL * detent_count);
            else
                angle_pos_sub_cal += (DETENT_INTERVAL * detent_count);

            DetailDebugPrint(X_INFO, 1, "[Detent] raw_X=%d, kernel_angle=%d, X=%d, angle=%d, time=%d, detent=%d, move_sum=%d\n",
                    angle_delta_raw, angle_pos_raw, angle_delta_cal, angle_pos, event_time, detent_count, angle_pos_sub_cal);

            if (angle_pos_sub_cal > DETENT_TIMEOUT_TOLERANCE)
            {
                DetailDebugPrint(X_INFO, 1, "[Compensation] move_sum = %d -> 10\n", angle_pos_sub_cal);
                angle_pos_sub_cal = DETENT_TOLERANCE;
            }
            else if (angle_pos_sub_cal < -DETENT_TIMEOUT_TOLERANCE)
            {
                DetailDebugPrint(X_INFO, 1, "[Compensation] move_sum = %d -> -10\n", angle_pos_sub_cal);
                angle_pos_sub_cal = -DETENT_TOLERANCE;
            }

            //detent timer
            detent_event_timer = TimerSet(detent_event_timer, 0, DETENT_TIMEOUT, RotaryReset, pInfo);
            if (start_event_timer)
            {
                DetailDebugPrint(X_INFO, 1, "[Start_Timer_Delete]\n");
                start_event_timer_flag = DELETE_TIMER;
            }
            is_detent = 0; // reset detent flag
        }
        else // no detent
        {
            if (state == IDLE)
            {
                valuator_mask_set(pEvdev->vals, REL_Z, NULL_DETENT);
                if (angle_pos_sub_cal > DETENT_TOLERANCE || angle_pos_sub_cal < -DETENT_TOLERANCE)
                {
                    if (angle_delta_cal > 0)
                    {
                        angle_delta_dir = 1;
                    }
                    else if (angle_delta_cal < 0)
                    {
                        angle_delta_dir = -1;
                    }
                    start_event_timer = TimerSet(start_event_timer, 0, START_TIMEOUT, RotaryReset, pInfo);
                    DetailDebugPrint(X_INFO, 1, "[Start] start tolerance = %d\n", angle_pos_sub_cal);
                    state = MOVING;
                }
                else
                {
                    if (angle_pos_sub_cal * angle_delta_cal <= 0)
                    {
                        DetailDebugPrint(X_INFO, 1, "[No Start] Direction changed. before=%d, current=%d\n", angle_pos_sub_cal, angle_delta_cal);
                        angle_pos_sub_cal = angle_delta_cal;
                    }
                    else
                        DetailDebugPrint(X_INFO, 1, "[No Start] No start tolerance = %d\n", angle_pos_sub_cal);
                }
            }

            if (state == MOVING)
            {
                if ( (angle_delta_dir == 1 && angle_pos_sub_cal <= (return_tolerance*2)) ||
                        (angle_delta_dir == -1 && angle_pos_sub_cal >= -(return_tolerance*2)) )
                {
                    valuator_mask_set(pEvdev->vals, REL_X, 0);
                    valuator_mask_set(pEvdev->vals, REL_Z, ZERO_DETENT);
                    DetailDebugPrint(X_INFO, 1, "[Detent2] angle_delta_dir=%d, angle_pos_sub_cal=%d, return case%d\n",
                            angle_delta_dir, angle_pos_sub_cal, (angle_delta_dir==1)?1:2);
                    angle_delta_dir = 0;
                    detent_sum++;
                    return_state = 1;
                }
                else
                {
                    valuator_mask_set(pEvdev->vals, REL_Z, NULL_DETENT);
                    DetailDebugPrint(X_INFO, 1, "[No Detent1] raw_X=%d, kernel_angle=%d, X=%d, angle=%d, time=%d, detent=%d, move_sum=%d\n",
                            angle_delta_raw, angle_pos_raw, angle_delta_cal, angle_pos, event_time, NULL_DETENT, angle_pos_sub_cal);
                }

                if (return_state)
                {
                    if (start_event_timer)
                    {
                        DetailDebugPrint(X_INFO, 1, "[start_event_timer Cancel]\n");
                        start_event_timer_flag = DELETE_TIMER;
                    }

                    detent_event_timer = TimerSet(detent_event_timer, 0, RETURN_TIMEOUT, RotaryReset, pInfo);

                    return_state = 0;
                    return_ignore_state = 1;
                    return_ignore_angle = angle_pos_sub_cal;
                }
            }
        }
    }
}

void EvdevRotaryInit(DeviceIntPtr device)
{
    InputInfoPtr pInfo;
    EvdevPtr pEvdev;

    InputInfoPtr pInfoDetent = NULL;
    EvdevPtr pEvdevDetent = NULL;
    DeviceIntPtr dev;

    pInfo = device->public.devicePrivate;
    pEvdev = pInfo->private;
    if (pEvdev->flags & EVDEV_OFM) {
        pEvdev->extra_rel_post_ofm= EvdevRotaryProcessOfmEvent;

        hw_rotary_max = pEvdev->HW_Calibration;
        kernel_angle_step = ((double)SW_ROTARY_MAX/hw_rotary_max);
        return_tolerance = ((10 * SW_ROTARY_MAX) / hw_rotary_max);

        if (hasDetent)
        {
            for (dev = inputInfo.keyboard; dev; dev = dev->next)
            {
                if (!strncmp(dev->name, "tizen_detent", strlen(dev->name)))
                {
                    pInfoDetent = dev->public.devicePrivate;
                    pEvdevDetent = pInfoDetent->private;
                    break;
                }
            }
            pEvdevDetent->extra_rel_post_hallic = EvdevRotaryProcessLegacyHallIcEvent;
            LogMessageVerbSigSafe(X_INFO, 1, "[EvdevRotaryInit][OFM+HALL] Detent device detected before rotary device. So, detent device call has been changed.\n");
        }

        hasRotary = OFM_DEVICE;

        LogMessageVerbSigSafe(X_INFO, 1, "[Rotary Init] hw_rotary_max=%d, return_tolerance=%d, kernel_angle_step=%d\n", hw_rotary_max, return_tolerance, kernel_angle_step);
    }

    if (pEvdev->flags & EVDEV_HALLIC) {
        if (hasRotary)
        {
            pEvdev->extra_rel_post_hallic = EvdevRotaryProcessLegacyHallIcEvent;
            LogMessageVerbSigSafe(X_INFO, 1, "[EvdevRotaryInit][OFM+HALL] Rotary device detetced before detent device\n");
        }
        else
        {
            pEvdev->extra_rel_post_hallic = EvdevRotaryProcessHallIcEvent;
            LogMessageVerbSigSafe(X_INFO, 1, "[EvdevRotaryInit][HALL] Detent device without rotary device.\n");
        }
        hasDetent = HALL_DEVICE;
    }
}

void EvdevRotaryUnInit(EvdevPtr pEvdev)
{
    if(!pEvdev)
    {
        LogMessageVerbSigSafe(X_ERROR, 1, "[EvdevRotaryUnInit] pEvdev not yet initialized.\n");
        return;
    }

    if (pEvdev->flags & EVDEV_OFM)
    {
        TimerFree(rotary_frame_sync_timer);
        rotary_frame_sync_timer = NULL;
    }
}

#endif //_F_EVDEV_SUPPORT_ROTARY_
