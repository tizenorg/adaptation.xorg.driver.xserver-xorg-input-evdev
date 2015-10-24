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

#ifdef _F_PROXY_DEVICE_ENABLED_
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "evdev.h"
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
#include <xorg/optionstr.h>

#include <xorg/mi.h>

struct _DeviceEvent {
    unsigned char header; /**< Always ET_Internal */
    enum EventType type;  /**< One of EventType */
    int length;           /**< Length in bytes */
    Time time;            /**< Time in ms */
    int deviceid;         /**< Device to post this event for */
    int sourceid;         /**< The physical source device */
    union {
        uint32_t button;  /**< Button number (also used in pointer emulating
                               touch events) */
        uint32_t key;     /**< Key code */
    } detail;
    uint32_t touchid;     /**< Touch ID (client_id) */
    int16_t root_x;       /**< Pos relative to root window in integral data */
    float root_x_frac;    /**< Pos relative to root window in frac part */
    int16_t root_y;       /**< Pos relative to root window in integral part */
    float root_y_frac;    /**< Pos relative to root window in frac part */
    uint8_t buttons[(MAX_BUTTONS + 7) / 8];  /**< Button mask */
    struct {
        uint8_t mask[(MAX_VALUATORS + 7) / 8];/**< Valuator mask */
        uint8_t mode[(MAX_VALUATORS + 7) / 8];/**< Valuator mode (Abs or Rel)*/
        double data[MAX_VALUATORS];           /**< Valuator data */
    } valuators;
    struct {
        uint32_t base;    /**< XKB base modifiers */
        uint32_t latched; /**< XKB latched modifiers */
        uint32_t locked;  /**< XKB locked modifiers */
        uint32_t effective;/**< XKB effective modifiers */
    } mods;
    struct {
        uint8_t base;    /**< XKB base group */
        uint8_t latched; /**< XKB latched group */
        uint8_t locked;  /**< XKB locked group */
        uint8_t effective;/**< XKB effective group */
    } group;
    Window root;      /**< Root window of the event */
    int corestate;    /**< Core key/button state BEFORE the event */
    int key_repeat;   /**< Internally-generated key repeat event */
    uint32_t flags;   /**< Flags to be copied into the generated event */
    uint32_t resource; /**< Touch event resource, only for TOUCH_REPLAYING */
};

typedef struct _barrierBoundary {
    unsigned int x1;
    unsigned int y1;
    unsigned int x2;
    unsigned int y2;
} barrierBoundary;

typedef enum _barrierState {
    BARRIER_NONE,
    BARRIER_INSIDE,
    BARRIER_OUTSIDE
} barrierState;

#define prop_MLS_Barrier_Boundary "MLS_Barrier_Boundary"
#define prop_MLS_Barrier_Hit "MLS_Barrier_Hit"

static void EvdevProxyBlockHandler(pointer data, OSTimePtr pTimeout, pointer pRead);
void EvdevProxySetBarrierBoundary(int num, unsigned int val[4], barrierBoundary *boundary);
barrierState EvdevProxyLocationManager(int x, int y, barrierBoundary boundary);
void EvdevProxyProcessMotion(int screen_num, InternalEvent *ev, DeviceIntPtr device);
void EvdevProxyProcessRawMotion(int screen_num, InternalEvent *ev, DeviceIntPtr device);
static int EvdevProxySetProperty(DeviceIntPtr dev, Atom atom, XIPropertyValuePtr val, BOOL checkonly);
static int EvdevProxyDeleteProperty(DeviceIntPtr dev, Atom atom);

DeviceIntPtr proxyDev;

/* MLS Barrier */
barrierBoundary BarrierBoundary;
barrierState BarrierState;
Atom atom_MLS_Barrier_Boundary;
Atom atom_MLS_Barrier_Hit;

/* Ungrab device */
static Atom prop_ungrab_device;

int last_id;
#define VCP_ID 2

static void
EvdevProxyBlockHandler(pointer data, OSTimePtr pTimeout, pointer pRead)
{
    InputInfoPtr pInfo = (InputInfoPtr)data;
    int rc = 0;

    RemoveBlockAndWakeupHandlers(EvdevProxyBlockHandler,
                                 (WakeupHandlerProcPtr)NoopDDA,
                                 data);

    if (BarrierState != BARRIER_NONE)
        rc = XIChangeDeviceProperty(pInfo->dev, atom_MLS_Barrier_Hit, XA_INTEGER, 32,
				PropModeReplace, 1, &BarrierState, TRUE);

    if (rc != Success)
    {
        xf86IDrvMsg(pInfo, X_ERROR, "[%s:%d] Failed to change device property (id:%d, prop=%d)\n", __FUNCTION__, __LINE__, pInfo->dev->id, atom_MLS_Barrier_Hit);
    }
}

void EvdevProxySetBarrierBoundary(int num, unsigned int val[4], barrierBoundary *boundary)
{
    if (num == 0) {
        if (BarrierState != BARRIER_NONE) ErrorF("[%s:%d] Unset MLS Barrier Boundary\n", __FUNCTION__, __LINE__);
        boundary->x1 = 0;
        boundary->x2 = 0;
        boundary->y1 = 0;
        boundary->y2 = 0;
        BarrierState = BARRIER_NONE;
    }
    else {
        ErrorF("[%s:%d] Set MLS Barrier Boundary (%d, %d) (%d, %d)\n", __FUNCTION__, __LINE__, val[0], val[1], val[2], val[3]);
        boundary->x1 = val[0];
        boundary->x2 = val[2];
        boundary->y1 = val[1];
        boundary->y2 = val[3];
    }
}

barrierState EvdevProxyLocationManager(int x, int y, barrierBoundary boundary)
{
    if ((boundary.x1+boundary.y1+boundary.x2+boundary.y2) <= 0 ) {
        return BARRIER_NONE;
    }
    else if ( (boundary.x1 <= x && x <=boundary.x2)
        && (boundary.y1 <= y && y <=boundary.y2) ) {
        return BARRIER_INSIDE;
    }
    return BARRIER_OUTSIDE;
}

void EvdevProxyProcessRawMotion(int screen_num, InternalEvent * ev,DeviceIntPtr device)
{
    if (device->id != VCP_ID)
        last_id = device->id;
    device->public.processInputProc(ev, device);
}

void EvdevProxyProcessMotion(int screen_num, InternalEvent * ev,DeviceIntPtr device)
{
    DeviceEvent *dev = &(ev->changed_event);
    barrierState prevBarrierState = BarrierState;
    if (!dev) {
        ErrorF("[%s:%d] It is not a device event\n", __FUNCTION__, __LINE__);
        goto process_event;
    }
    if (last_id != device->id) {
        last_id = 0;
        goto process_event;
    }

    BarrierState = EvdevProxyLocationManager(dev->root_x, dev->root_y, BarrierBoundary);
    if (BarrierState == BARRIER_NONE)
        goto process_event;
    else if (prevBarrierState == BarrierState)
        goto process_event;
    else if (prevBarrierState == BARRIER_INSIDE) {
        ErrorF("[%s:%d] Pointer in to barrier from outside\n", __FUNCTION__, __LINE__);
        OsBlockSIGIO();
        RegisterBlockAndWakeupHandlers(EvdevProxyBlockHandler ,(WakeupHandlerProcPtr) NoopDDA, (InputInfoPtr)(proxyDev->public.devicePrivate));
        OsReleaseSIGIO();
    }
    else if (prevBarrierState == BARRIER_OUTSIDE) {
        ErrorF("[%s:%d] Pointer out to barrier from inside\n", __FUNCTION__, __LINE__);
        OsBlockSIGIO();
        RegisterBlockAndWakeupHandlers(EvdevProxyBlockHandler ,(WakeupHandlerProcPtr) NoopDDA, (InputInfoPtr)(proxyDev->public.devicePrivate));
        OsReleaseSIGIO();
    }
    else if (prevBarrierState == BARRIER_NONE && BarrierState == BARRIER_INSIDE) {
        ErrorF("[%s:%d] Pointer move start from inside\n", __FUNCTION__, __LINE__);
        OsBlockSIGIO();
        RegisterBlockAndWakeupHandlers(EvdevProxyBlockHandler ,(WakeupHandlerProcPtr) NoopDDA, (InputInfoPtr)(proxyDev->public.devicePrivate));
        OsReleaseSIGIO();
    }
    last_id = 0;
process_event:
    device->public.processInputProc(ev, device);
}

void EvdevProxyInit(DeviceIntPtr device)
{
    InputInfoPtr pInfo = device->public.devicePrivate;;
    EvdevPtr pEvdev = pInfo->private;

    XIRegisterPropertyHandler(device, EvdevProxySetProperty, NULL, EvdevProxyDeleteProperty);
    mieqSetHandler(ET_Motion, EvdevProxyProcessMotion);
    mieqSetHandler(ET_RawMotion, EvdevProxyProcessRawMotion);

    proxyDev = device;

    BarrierBoundary.x1 = 0;
    BarrierBoundary.y1 = 0;
    BarrierBoundary.x2 = 0;
    BarrierBoundary.y2 = 0;
    BarrierState = BARRIER_NONE;

    last_id = 0;

    atom_MLS_Barrier_Boundary = MakeAtom(prop_MLS_Barrier_Boundary, strlen(prop_MLS_Barrier_Boundary), TRUE);
    atom_MLS_Barrier_Hit = MakeAtom(prop_MLS_Barrier_Hit, strlen(prop_MLS_Barrier_Hit), TRUE);
    prop_ungrab_device = MakeAtom(XI_PROP_UNGRAB_DEVICE, strlen(XI_PROP_UNGRAB_DEVICE), TRUE);
}

static int
EvdevProxySetProperty(DeviceIntPtr dev, Atom atom, XIPropertyValuePtr val,
                 BOOL checkonly)
{
    if (atom == atom_MLS_Barrier_Boundary) {
        if (val->format != 32 || val->type != XA_INTEGER)
            return BadMatch;
        if (val->size != 4 && val->size != 0)
            return BadMatch;

        if (!checkonly) {
            EvdevProxySetBarrierBoundary(val->size, val->data, &BarrierBoundary);
        }
    }

    else if (atom == prop_ungrab_device )
    {
        int data;
        DeviceIntPtr master = inputInfo.pointer;

        if(!checkonly) {
            data = *((CARD8*)val->data);
            if (data == 1 && master->deviceGrab.grab) {
                ErrorF("[%s:%d] ungrab master pointer device(%d) in device (%d)\n", __FUNCTION__, __LINE__, master->id, dev->id);
                (*master->deviceGrab.DeactivateGrab) (master);
            }
        }
    }

    return Success;
}

static int
EvdevProxyDeleteProperty(DeviceIntPtr dev, Atom atom)
{
    if (atom == atom_MLS_Barrier_Boundary) {
        EvdevProxySetBarrierBoundary(0, NULL, &BarrierBoundary);
    }

    return Success;
}
#endif //_F_PROXY_DEVICE_ENABLED_
