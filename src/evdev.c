/*
 *
 * xserver-xorg-input-evdev
 *
 * Contact: Sung-Jin Park <sj76.park@samsung.com>
 *          Sangjin LEE <lsj119@samsung.com>
 *
 * Copyright (c) 2000 - 2011 Samsung Electronics Co., Ltd. All rights reserved.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "evdev.h"

#include <X11/keysym.h>
#include <X11/extensions/XI.h>

#include <linux/version.h>
#include <sys/stat.h>
#include <libudev.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <xf86.h>
#include <xf86Xinput.h>
#include <exevents.h>
#include <xorgVersion.h>
#include <xkbsrv.h>

#include <X11/Xatom.h>
#include <evdev-properties.h>
#include <xserver-properties.h>

#ifdef _F_EVDEV_CONFINE_REGION_
#include <xorg/mipointrst.h>

#define MIPOINTER(dev) \
    ((!IsMaster(dev) && !dev->master) ? \
        (miPointerPtr)dixLookupPrivate(&(dev)->devPrivates, miPointerPrivKey): \
        (miPointerPtr)dixLookupPrivate(&(GetMaster(dev, MASTER_POINTER))->devPrivates, miPointerPrivKey))

#endif /* _F_EVDEV_CONFINE_REGION_ */

#ifndef XI_PROP_PRODUCT_ID
#define XI_PROP_PRODUCT_ID "Device Product ID"
#endif

#ifndef XI_PROP_VIRTUAL_DEVICE
#define XI_PROP_VIRTUAL_DEVICE "Virtual Device"
#endif

#ifndef XI_PROP_DEVICE_TYPE
#define XI_PROP_DEVICE_TYPE "Device Type"
#endif

/* removed from server, purge when dropping support for server 1.10 */
#define XI86_SEND_DRAG_EVENTS   0x08

#ifndef MAXDEVICES
#include <inputstr.h> /* for MAX_DEVICES */
#define MAXDEVICES MAX_DEVICES
#endif

#define ArrayLength(a) (sizeof(a) / (sizeof((a)[0])))

#define MIN_KEYCODE 8
#define GLYPHS_PER_KEY 2
#define AltMask		Mod1Mask
#define NumLockMask	Mod2Mask
#define AltLangMask	Mod3Mask
#define KanaMask	Mod4Mask
#define ScrollLockMask	Mod5Mask

#define CAPSFLAG	1
#define NUMFLAG		2
#define SCROLLFLAG	4
#define MODEFLAG	8
#define COMPOSEFLAG	16

#ifndef ABS_MT_SLOT
#define ABS_MT_SLOT 0x2f
#endif

#ifndef ABS_MT_TRACKING_ID
#define ABS_MT_TRACKING_ID 0x39
#endif

static char *evdevDefaults[] = {
    "XkbRules",     "evdev",
    "XkbModel",     "evdev",
    "XkbLayout",    "us",
    NULL
};

/* Any of those triggers a proximity event */
static int proximity_bits[] = {
        BTN_TOOL_PEN,
        BTN_TOOL_RUBBER,
        BTN_TOOL_BRUSH,
        BTN_TOOL_PENCIL,
        BTN_TOOL_AIRBRUSH,
        BTN_TOOL_FINGER,
        BTN_TOOL_MOUSE,
        BTN_TOOL_LENS,
};

static int EvdevOn(DeviceIntPtr);
static int EvdevCache(InputInfoPtr pInfo);
static void EvdevKbdCtrl(DeviceIntPtr device, KeybdCtrl *ctrl);
static int EvdevSwitchMode(ClientPtr client, DeviceIntPtr device, int mode);
static BOOL EvdevGrabDevice(InputInfoPtr pInfo, int grab, int ungrab);
static void EvdevSetCalibration(InputInfoPtr pInfo, int num_calibration, int calibration[4]);
static int EvdevOpenDevice(InputInfoPtr pInfo);

static void EvdevInitAxesLabels(EvdevPtr pEvdev, int mode, int natoms, Atom *atoms);
static void EvdevInitButtonLabels(EvdevPtr pEvdev, int natoms, Atom *atoms);
static void EvdevInitProperty(DeviceIntPtr dev);
static int EvdevSetProperty(DeviceIntPtr dev, Atom atom,
                            XIPropertyValuePtr val, BOOL checkonly);
#ifdef _F_GESTURE_EXTENSION_
extern void mieqEnqueue(DeviceIntPtr pDev, InternalEvent *e);
static void EvdevMTSync(InputInfoPtr pInfo, MTSyncType sync);
static BOOL EvdevMTStatusGet(InputInfoPtr pInfo, MTSyncType sync);
#endif /* #ifdef _F_GESTURE_EXTENSION_ */
#ifdef _F_TOUCH_TRANSFORM_MATRIX_
static void EvdevSetTransformMatrix(InputInfoPtr pInfo, int num_transform, float *tmatrix);
#endif /* #ifdef _F_TOUCH_TRANSFORM_MATRIX_ */
#ifdef _F_EVDEV_CONFINE_REGION_
Bool IsMaster(DeviceIntPtr dev);
DeviceIntPtr GetPairedDevice(DeviceIntPtr dev);
DeviceIntPtr GetMaster(DeviceIntPtr dev, int which);
DeviceIntPtr GetMasterPointerFromId(int deviceid);
static void EvdevHookPointerCursorLimits(DeviceIntPtr pDev, ScreenPtr pScreen, CursorPtr pCursor, BoxPtr pHotBox, BoxPtr pTopLeftBox);
static void EvdevHookPointerConstrainCursor (DeviceIntPtr pDev, ScreenPtr pScreen, BoxPtr pBox);
static void EvdevSetCursorLimits(InputInfoPtr pInfo, int region[6], int isSet);
static void EvdevSetConfineRegion(InputInfoPtr pInfo, int num_item, int region[6]);
#ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_
static CARD32 EvdevRelativeMoveTimer(OsTimerPtr timer, CARD32 time, pointer arg);
#endif /* #ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_ */

static Atom prop_confine_region = 0;
#endif /* _F_EVDEV_CONFINE_REGION_ */
static Atom prop_product_id;
static Atom prop_invert;
static Atom prop_calibration;
static Atom prop_swap;
static Atom prop_axis_label;
static Atom prop_btn_label;
static Atom prop_device;
static Atom prop_virtual;
#ifdef _F_ENABLE_DEVICE_TYPE_PROP_
static Atom prop_device_type;
#endif /* #ifdef _F_ENABLE_DEVICE_TYPE_PROP_ */
#ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_
static Atom prop_relative_move_status;
static Atom prop_relative_move_ack;
#endif /* #ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_ */
#ifdef _F_TOUCH_TRANSFORM_MATRIX_
static Atom prop_transform;
#endif /* #ifdef _F_TOUCH_TRANSFORM_MATRIX_ */

#ifdef _F_USE_DEFAULT_XKB_RULES_
static Atom prop_xkb_rules = None;
#endif //_F_USE_DEFAULT_XKB_RULES_

/* All devices the evdev driver has allocated and knows about.
 * MAXDEVICES is safe as null-terminated array, as two devices (VCP and VCK)
 * cannot be used by evdev, leaving us with a space of 2 at the end. */
static EvdevPtr evdev_devices[MAXDEVICES] = {NULL};

#ifdef _F_REMAP_KEYS_
static uint16_t
remapKey(EvdevPtr ev, uint16_t code)
{
    uint8_t slice=code/256;
    uint8_t offs=code%256;

    if (!ev->keyremap) return code;
    if (!(ev->keyremap->sl[slice])) return code;
    if (!(ev->keyremap->sl[slice]->cd[offs])) return code;
    return ev->keyremap->sl[slice]->cd[offs];
}

static void
addRemap(EvdevPtr ev,uint16_t code,uint8_t value)
{
    uint8_t slice=code/256;
    uint8_t offs=code%256;

    if (!ev->keyremap) {
        ev->keyremap=(EvdevKeyRemapPtr)calloc(sizeof(EvdevKeyRemap),1);
    }
    if (!ev->keyremap->sl[slice]) {
        ev->keyremap->sl[slice]=(EvdevKeyRemapSlice*)calloc(sizeof(EvdevKeyRemapSlice),1);
     }
     ev->keyremap->sl[slice]->cd[offs]=value;
}

static void
freeRemap(EvdevPtr ev)
{
    uint16_t slice;
    if (!ev->keyremap) return;
    for (slice=0;slice<256;++slice) {
        if (!ev->keyremap->sl[slice]) continue;
        free(ev->keyremap->sl[slice]);
    }
    free(ev->keyremap);
    ev->keyremap=0;
}
#endif //_F_REMAP_KEYS_

static int EvdevSwitchMode(ClientPtr client, DeviceIntPtr device, int mode)
{
    InputInfoPtr pInfo;
    EvdevPtr pEvdev;

    pInfo = device->public.devicePrivate;
    pEvdev = pInfo->private;

    if (pEvdev->flags & EVDEV_RELATIVE_EVENTS)
    {
        if (mode == Relative)
            return Success;
        else
            return XI_BadMode;
    }

    switch (mode) {
        case Absolute:
            pEvdev->flags &= ~EVDEV_RELATIVE_MODE;
            break;

        case Relative:
            pEvdev->flags |= EVDEV_RELATIVE_MODE;
            break;

        default:
            return XI_BadMode;
    }

    return Success;
}

static size_t EvdevCountBits(unsigned long *array, size_t nlongs)
{
    unsigned int i;
    size_t count = 0;

    for (i = 0; i < nlongs; i++) {
        unsigned long x = array[i];

        while (x > 0)
        {
            count += (x & 0x1);
            x >>= 1;
        }
    }
    return count;
}

static inline int EvdevBitIsSet(const unsigned long *array, int bit)
{
    return !!(array[bit / LONG_BITS] & (1LL << (bit % LONG_BITS)));
}

static inline void EvdevSetBit(unsigned long *array, int bit)
{
    array[bit / LONG_BITS] |= (1LL << (bit % LONG_BITS));
}

static int
EvdevGetMajorMinor(InputInfoPtr pInfo)
{
    struct stat st;

    if (fstat(pInfo->fd, &st) == -1)
    {
        xf86IDrvMsg(pInfo, X_ERROR, "stat failed (%s). cannot check for duplicates.\n",
                    strerror(errno));
        return 0;
    }

    return st.st_rdev;
}

/**
 * Return TRUE if one of the devices we know about has the same min/maj
 * number.
 */
static BOOL
EvdevIsDuplicate(InputInfoPtr pInfo)
{
    EvdevPtr pEvdev = pInfo->private;
    EvdevPtr* dev   = evdev_devices;

    if (pEvdev->min_maj)
    {
        while(*dev)
        {
            if ((*dev) != pEvdev &&
                (*dev)->min_maj &&
                (*dev)->min_maj == pEvdev->min_maj)
                return TRUE;
            dev++;
        }
    }
    return FALSE;
}

/**
 * Add to internal device list.
 */
static void
EvdevAddDevice(InputInfoPtr pInfo)
{
    EvdevPtr pEvdev = pInfo->private;
    EvdevPtr* dev = evdev_devices;

    while(*dev)
        dev++;

    *dev = pEvdev;
}

/**
 * Remove from internal device list.
 */
static void
EvdevRemoveDevice(InputInfoPtr pInfo)
{
    EvdevPtr pEvdev = pInfo->private;
    EvdevPtr *dev   = evdev_devices;
    int count       = 0;

    while(*dev)
    {
        count++;
        if (*dev == pEvdev)
        {
            memmove(dev, dev + 1,
                    sizeof(evdev_devices) - (count * sizeof(EvdevPtr)));
            break;
        }
        dev++;
    }
}


static void
SetXkbOption(InputInfoPtr pInfo, char *name, char **option)
{
    char *s;

    if ((s = xf86SetStrOption(pInfo->options, name, NULL))) {
        if (!s[0]) {
            free(s);
            *option = NULL;
        } else {
            *option = s;
        }
    }
}

static BOOL
EvdevDeviceIsVirtual(const char* devicenode)
{
    struct udev *udev = NULL;
    struct udev_device *device = NULL;
    struct stat st;
    int rc = FALSE;
    const char *devpath;

    udev = udev_new();
    if (!udev)
        goto out;

    if (stat(devicenode, &st) < 0) {
        ErrorF("Failed to get (%s)'s status (stat)\n", devicenode);
        goto out;
    }
    device = udev_device_new_from_devnum(udev, 'c', st.st_rdev);

    if (!device)
        goto out;


    devpath = udev_device_get_devpath(device);
    if (!devpath)
        goto out;

    if (strstr(devpath, "LNXSYSTM"))
        rc = TRUE;

out:
    udev_device_unref(device);
    udev_unref(udev);
    return rc;
}

#ifndef HAVE_SMOOTH_SCROLLING
static int wheel_up_button = 4;
static int wheel_down_button = 5;
static int wheel_left_button = 6;
static int wheel_right_button = 7;
#endif

#ifdef _F_REMAP_KEYS_
static void
SetRemapOption(InputInfoPtr pInfo,const char* name)
{
    char *s,*c;
    unsigned long int code,value;
    int consumed;
    EvdevPtr ev = pInfo->private;

    s = xf86SetStrOption(pInfo->options, name, NULL);
    if (!s) return;
    if (!s[0]) {
        free(s);
        return;
    }

    c=s;
    while (sscanf(c," %li = %li %n",&code,&value,&consumed) > 1) {
        c+=consumed;
        if (code < 0 || code > 65535L) {
            xf86Msg(X_ERROR,"%s: input code %ld out of range for option \"event_key_remap\", ignoring.\n",pInfo->name,code);
            continue;
        }
        if (value < MIN_KEYCODE || value > 255) {
            xf86Msg(X_ERROR,"%s: output value %ld out of range for option \"event_key_remap\", ignoring.\n",pInfo->name,value);
            continue;
        }
        xf86Msg(X_INFO,"%s: remapping %ld into %ld.\n",pInfo->name,code,value);
        addRemap(ev,code,value-MIN_KEYCODE);
    }

    if (*c!='\0') {
        xf86Msg(X_ERROR, "%s: invalid input for option \"event_key_remap\" starting at '%s', ignoring.\n",
                pInfo->name, c);
    }
}
#endif //_F_REMAP_KEYS_

static EventQueuePtr
EvdevNextInQueue(InputInfoPtr pInfo)
{
    EvdevPtr pEvdev = pInfo->private;

    if (pEvdev->num_queue >= EVDEV_MAXQUEUE)
    {
        xf86IDrvMsg(pInfo, X_NONE, "dropping event due to full queue!\n");
        return NULL;
    }

    pEvdev->num_queue++;
    return &pEvdev->queue[pEvdev->num_queue - 1];
}

void
EvdevQueueKbdEvent(InputInfoPtr pInfo, struct input_event *ev, int value)
{
#ifdef _F_REMAP_KEYS_
    int code = remapKey((EvdevPtr)(pInfo->private),ev->code) + MIN_KEYCODE;
#else //_F_REMAP_KEYS_
    int code = ev->code + MIN_KEYCODE;
#endif //_F_REMAP_KEYS_
    EventQueuePtr pQueue;

    /* Filter all repeated events from device.
       We'll do softrepeat in the server, but only since 1.6 */
    if (value == 2)
        return;

    if ((pQueue = EvdevNextInQueue(pInfo)))
    {
        pQueue->type = EV_QUEUE_KEY;
        pQueue->detail.key = code;
        pQueue->val = value;
    }
}

void
EvdevQueueButtonEvent(InputInfoPtr pInfo, int button, int value)
{
    EventQueuePtr pQueue;

    if ((pQueue = EvdevNextInQueue(pInfo)))
    {
        pQueue->type = EV_QUEUE_BTN;
        pQueue->detail.key = button;
        pQueue->val = value;
    }
}

void
EvdevQueueProximityEvent(InputInfoPtr pInfo, int value)
{
    EventQueuePtr pQueue;
    if ((pQueue = EvdevNextInQueue(pInfo)))
    {
        pQueue->type = EV_QUEUE_PROXIMITY;
        pQueue->detail.key = 0;
        pQueue->val = value;
    }
}

#ifdef MULTITOUCH
void
EvdevQueueTouchEvent(InputInfoPtr pInfo, unsigned int touch, ValuatorMask *mask,
                     uint16_t evtype)
{
    EventQueuePtr pQueue;
    if ((pQueue = EvdevNextInQueue(pInfo)))
    {
        pQueue->type = EV_QUEUE_TOUCH;
        pQueue->detail.touch = touch;
        valuator_mask_copy(pQueue->touchMask, mask);
        pQueue->val = evtype;
    }
}
#endif

/**
 * Post button event right here, right now.
 * Interface for MB emulation since these need to post immediately.
 */
void
EvdevPostButtonEvent(InputInfoPtr pInfo, int button, enum ButtonAction act)
{
    xf86PostButtonEvent(pInfo->dev, Relative, button,
                        (act == BUTTON_PRESS) ? 1 : 0, 0, 0);
}

void
EvdevQueueButtonClicks(InputInfoPtr pInfo, int button, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        EvdevQueueButtonEvent(pInfo, button, 1);
        EvdevQueueButtonEvent(pInfo, button, 0);
    }
}

#ifdef _F_EVDEV_SUPPORT_GAMEPAD
static void
EvdevMappingGamepadAbsToKey(InputInfoPtr pInfo,  struct input_event *ev)
{
    EvdevPtr pEvdev = pInfo->private;

    if (pEvdev->support_directional_key == FALSE)
        return;

    if (ev->type != EV_ABS)
    {
        ErrorF("[EvdevMappingGamepadAbsToKey] Invalid evtype(%d)\n", ev->type);
        return;
    }

    if (ev->code == ABS_HAT0X)
    {
        switch(ev->value)
        {
            case 1:
                ev->type = EV_KEY;
                ev->code = KEY_RIGHT;
                ev->value = EVDEV_PRESS;
                pEvdev->pre_hatx = 1;
                EvdevProcessEvent(pInfo, ev);
                break;
            case -1:
                ev->type = EV_KEY;
                ev->code = KEY_LEFT;
                ev->value = EVDEV_PRESS;
                pEvdev->pre_hatx = -1;
                EvdevProcessEvent(pInfo, ev);
                break;
            case 0:
                ev->type = EV_KEY;
                ev->code = ( (pEvdev->pre_hatx == 1)? KEY_RIGHT : KEY_LEFT);
                ev->value = EVDEV_RELEASE;
                pEvdev->pre_hatx = 0;
                EvdevProcessEvent(pInfo, ev);
                break;
            default:
                ErrorF("[EvdevMappingGamepadAbsToKey] Invalid value\n");
                return;
                break;
        }
    }
    else if(ev->code == ABS_HAT0Y)
    {
        switch(ev->value)
        {
            case 1:
                ev->type = EV_KEY;
                ev->code = KEY_DOWN;
                ev->value = EVDEV_PRESS;
                pEvdev->pre_haty = 1;
                EvdevProcessEvent(pInfo, ev);
                break;
            case -1:
                ev->type = EV_KEY;
                ev->code = KEY_UP;
                ev->value = EVDEV_PRESS;
                pEvdev->pre_haty = -1;
                EvdevProcessEvent(pInfo, ev);
                break;
            case 0:
                ev->type = EV_KEY;
                ev->code = ( (pEvdev->pre_haty == 1)? KEY_DOWN : KEY_UP);
                ev->value = EVDEV_RELEASE;
                pEvdev->pre_haty = 0;
                EvdevProcessEvent(pInfo, ev);
                break;
            default:
                ErrorF("[EvdevMappingGamepadAbsToKey] Invalid value\n");
                return;
                break;
        }
    }
    else if(ev->code == ABS_X)
    {
        switch(ev->value)
        {
            case 0:
                ev->type = EV_KEY;
                ev->code = KEY_LEFT;
                ev->value = EVDEV_PRESS;
                pEvdev->pre_x = 0;
                EvdevProcessEvent(pInfo, ev);
                break;
            case 1 ... 254:
                if( pEvdev->pre_x == 255 || pEvdev->pre_x == 0 )
                {
                    ev->type = EV_KEY;
                    ev->code = ( (pEvdev->pre_x == 255)? KEY_RIGHT : KEY_LEFT);
                    ev->value = EVDEV_RELEASE;
                    pEvdev->pre_x = 128;
                    EvdevProcessEvent(pInfo, ev);
                }
                break;
            case 255:
                ev->type = EV_KEY;
                ev->code = KEY_RIGHT;
                ev->value = EVDEV_PRESS;
                pEvdev->pre_x = 255;
                EvdevProcessEvent(pInfo, ev);
                break;
            default:
                ErrorF("[EvdevMappingGamepadAbsToKey] Invalid value\n");
                return;
                break;
        }
    }
    else if(ev->code == ABS_Y)
    {
        switch(ev->value)
        {
            case 0:
                ev->type = EV_KEY;
                ev->code = KEY_UP;
                ev->value = EVDEV_PRESS;
                pEvdev->pre_y = 0;
                EvdevProcessEvent(pInfo, ev);
                break;
            case 1 ... 254:
                if( pEvdev->pre_y == 255 || pEvdev->pre_y == 0 )
                {
                    ev->type = EV_KEY;
                    ev->code = ( (pEvdev->pre_y == 255)? KEY_DOWN : KEY_UP);
                    ev->value = EVDEV_RELEASE;
                    pEvdev->pre_y = 128;
                    EvdevProcessEvent(pInfo, ev);
                }
                break;
            case 255:
                ev->type = EV_KEY;
                ev->code = KEY_DOWN;
                ev->value = EVDEV_PRESS;
                pEvdev->pre_y = 255;
                EvdevProcessEvent(pInfo, ev);
                break;
            default:
                ErrorF("[EvdevMappingGamepadAbsToKey] Invalid value\n");
                return;
                break;
        }
    }
}

static void
EvdevMappingGamepadKeyToKey(InputInfoPtr pInfo,  struct input_event *ev)
{
    EvdevPtr pEvdev = pInfo->private;
    if(ev->type != EV_KEY)
    {
        ErrorF("[EvdevMappingGamepadKeyToKey] Invalid type (%d)\n", ev->type);
        return;
    }
    if(ev->code == BTN_A)
    {
        if (pEvdev->keycode_btnA == 0)
        {
            ev->code = pEvdev->keycode_btnA;
            return;
        }
        switch(ev->value)
        {
            case 1:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnA;
                ev->value = EVDEV_PRESS;
                break;
            case 0:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnA;
                ev->value = EVDEV_RELEASE;
                break;
            default:
                ErrorF("[EvdevMappingGamepadKeyToKey] Invalid value\n");
                return;
                break;
        }
    }
    else if(ev->code == BTN_B)
    {
        if (pEvdev->keycode_btnB == 0)
        {
            ev->code = pEvdev->keycode_btnB;
            return;
        }
        switch(ev->value)
        {
            case 1:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnB;
                ev->value = EVDEV_PRESS;
                break;
            case 0:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnB;
                ev->value = EVDEV_RELEASE;
                break;
            default:
                ErrorF("[EvdevMappingGamepadKeyToKey] Invalid value\n");
                return;
                break;
        }
    }
    else if(ev->code == BTN_SELECT)
    {
        if (pEvdev->keycode_btnSelect == 0)
        {
            ev->code = pEvdev->keycode_btnSelect;
            return;
        }
        switch(ev->value)
        {
            case 1:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnSelect;
                ev->value = EVDEV_PRESS;
                break;
            case 0:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnSelect;
                ev->value = EVDEV_RELEASE;
                break;
            default:
                ErrorF("[EvdevMappingGamepadKeyToKey] Invalid value\n");
                return;
                break;
        }
    }
    else if(ev->code == BTN_START)
    {
        if (pEvdev->keycode_btnStart == 0)
        {
            ev->code = pEvdev->keycode_btnStart;
            return;
        }
        switch(ev->value)
        {
            case 1:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnStart;
                ev->value = EVDEV_PRESS;
                break;
            case 0:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnStart;
                ev->value = EVDEV_RELEASE;
                break;
            default:
                ErrorF("[EvdevMappingGamepadKeyToKey] Invalid value\n");
                return;
                break;
        }
    }
    else if(ev->code == 319)
    {
        if (pEvdev->keycode_btnPlay== 0)
        {
            ev->code = pEvdev->keycode_btnPlay;
            return;
        }
        switch(ev->value)
        {
            case 1:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnPlay;
                ev->value = EVDEV_PRESS;
                break;
            case 0:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnPlay;
                ev->value = EVDEV_RELEASE;
                break;
            default:
                ErrorF("[EvdevMappingGamepadKeyToKey] Invalid value\n");
                return;
                break;
        }
    }
    else if(ev->code == BTN_X)
    {
       if (pEvdev->keycode_btnX == 0)
      {
          ev->code = pEvdev->keycode_btnX;
          return;
      }
        switch(ev->value)
        {
            case 1:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnX;
                ev->value = EVDEV_PRESS;
                break;
            case 0:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnX;
                ev->value = EVDEV_RELEASE;
                break;
            default:
                ErrorF("[EvdevMappingGamepadKeyToKey] Invalid value\n");
                return;
                break;
        }
    }
    else if(ev->code == BTN_Y)
    {
        if (pEvdev->keycode_btnY == 0)
       {
          ev->code = pEvdev->keycode_btnY;
          return;
       }
        switch(ev->value)
        {
            case 1:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnY;
                ev->value = EVDEV_PRESS;
                break;
            case 0:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnY;
                ev->value = EVDEV_RELEASE;
                break;
            default:
                ErrorF("[EvdevMappingGamepadKeyToKey] Invalid value\n");
                return;
                break;
        }
    }
    else if(ev->code == BTN_TL)
    {
        if (pEvdev->keycode_btnTL== 0)
       {
          ev->code = pEvdev->keycode_btnTL;
          return;
       }
        switch(ev->value)
        {
            case 1:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnTL;
                ev->value = EVDEV_PRESS;
                break;
            case 0:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnTL;
                ev->value = EVDEV_RELEASE;
                break;
            default:
                ErrorF("[EvdevMappingGamepadKeyToKey] Invalid value\n");
                return;
                break;
        }
    }
    else if(ev->code == BTN_TR)
    {
        if (pEvdev->keycode_btnTR == 0)
        {
            ev->code = pEvdev->keycode_btnTR;
            return;
        }
        switch(ev->value)
        {
            case 1:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnTR;
                ev->value = EVDEV_PRESS;
                break;
            case 0:
                ev->type = EV_KEY;
                ev->code = pEvdev->keycode_btnTR;
                ev->value = EVDEV_RELEASE;
                break;
            default:
                ErrorF("[EvdevMappingGamepadKeyToKey] Invalid value\n");
                return;
                break;
        }
    }
}
#endif//_F_EVDEV_SUPPORT_GAMEPAD

/**
 * Take the valuators and process them accordingly.
 */
static void
EvdevProcessValuators(InputInfoPtr pInfo)
{
    int tmp;
    EvdevPtr pEvdev = pInfo->private;
    int *delta = pEvdev->delta;

    /* convert to relative motion for touchpads */
    if (pEvdev->abs_queued && (pEvdev->flags & EVDEV_RELATIVE_MODE)) {
        if (pEvdev->in_proximity) {
            if (valuator_mask_isset(pEvdev->vals, 0))
            {
                if (valuator_mask_isset(pEvdev->old_vals, 0))
                    delta[REL_X] = valuator_mask_get(pEvdev->vals, 0) -
                                   valuator_mask_get(pEvdev->old_vals, 0);
                valuator_mask_set(pEvdev->old_vals, 0,
                                  valuator_mask_get(pEvdev->vals, 0));
            }
            if (valuator_mask_isset(pEvdev->vals, 1))
            {
                if (valuator_mask_isset(pEvdev->old_vals, 1))
                    delta[REL_Y] = valuator_mask_get(pEvdev->vals, 1) -
                                   valuator_mask_get(pEvdev->old_vals, 1);
                valuator_mask_set(pEvdev->old_vals, 1,
                                  valuator_mask_get(pEvdev->vals, 1));
            }
        } else {
            valuator_mask_zero(pEvdev->old_vals);
        }
        valuator_mask_zero(pEvdev->vals);
        pEvdev->abs_queued = 0;
        pEvdev->rel_queued = 1;
    }

    if (pEvdev->rel_queued) {
        int i;

        if (pEvdev->swap_axes) {
            tmp = pEvdev->delta[REL_X];
            pEvdev->delta[REL_X] = pEvdev->delta[REL_Y];
            pEvdev->delta[REL_Y] = tmp;
            if (pEvdev->delta[REL_X] == 0)
                valuator_mask_unset(pEvdev->vals, REL_X);
            if (pEvdev->delta[REL_Y] == 0)
                valuator_mask_unset(pEvdev->vals, REL_Y);
        }
        if (pEvdev->invert_x)
            pEvdev->delta[REL_X] *= -1;
        if (pEvdev->invert_y)
            pEvdev->delta[REL_Y] *= -1;


        Evdev3BEmuProcessRelMotion(pInfo,
                                   pEvdev->delta[REL_X],
                                   pEvdev->delta[REL_Y]);

        for (i = 0; i < REL_CNT; i++)
        {
            int map = pEvdev->axis_map[i];
            if (pEvdev->delta[i] && map != -1)
                valuator_mask_set(pEvdev->vals, map, pEvdev->delta[i]);
        }
    }
    /*
     * Some devices only generate valid abs coords when BTN_TOOL_PEN is
     * pressed.  On wacom tablets, this means that the pen is in
     * proximity of the tablet.  After the pen is removed, BTN_TOOL_PEN is
     * released, and a (0, 0) absolute event is generated.  Checking
     * pEvdev->in_proximity here lets us ignore that event.  pEvdev is
     * initialized to 1 so devices that don't use this scheme still
     * just works.
     */
    else if (pEvdev->abs_queued && pEvdev->in_proximity) {
        int i;

        if (pEvdev->swap_axes) {
            int swapped_isset[2] = {0, 0};
            int swapped_values[2];

            for(i = 0; i <= 1; i++)
                if (valuator_mask_isset(pEvdev->vals, i)) {
                    swapped_isset[1 - i] = 1;
                    swapped_values[1 - i] =
                        xf86ScaleAxis(valuator_mask_get(pEvdev->vals, i),
                                      pEvdev->absinfo[1 - i].maximum,
                                      pEvdev->absinfo[1 - i].minimum,
                                      pEvdev->absinfo[i].maximum,
                                      pEvdev->absinfo[i].minimum);
                }

            for (i = 0; i <= 1; i++)
                if (swapped_isset[i])
                    valuator_mask_set(pEvdev->vals, i, swapped_values[i]);
                else
                    valuator_mask_unset(pEvdev->vals, i);
        }

        for (i = 0; i <= 1; i++) {
            int val;
            int calib_min;
            int calib_max;

            if (!valuator_mask_isset(pEvdev->vals, i))
                continue;

            val = valuator_mask_get(pEvdev->vals, i);

            if (i == 0) {
                calib_min = pEvdev->calibration.min_x;
                calib_max = pEvdev->calibration.max_x;
            } else {
                calib_min = pEvdev->calibration.min_y;
                calib_max = pEvdev->calibration.max_y;
            }

            if (pEvdev->flags & EVDEV_CALIBRATED)
                val = xf86ScaleAxis(val, pEvdev->absinfo[i].maximum,
                                    pEvdev->absinfo[i].minimum, calib_max,
                                    calib_min);

            if ((i == 0 && pEvdev->invert_x) || (i == 1 && pEvdev->invert_y))
                val = (pEvdev->absinfo[i].maximum - val +
                       pEvdev->absinfo[i].minimum);

            valuator_mask_set(pEvdev->vals, i, val);
        }
        Evdev3BEmuProcessAbsMotion(pInfo, pEvdev->vals);
    }
}

static void
EvdevProcessProximityEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    EvdevPtr pEvdev = pInfo->private;

    if (!pEvdev->use_proximity)
        return;

    pEvdev->prox_queued = 1;

    EvdevQueueProximityEvent(pInfo, ev->value);
}

/**
 * Proximity handling is rather weird because of tablet-specific issues.
 * Some tablets, notably Wacoms, send a 0/0 coordinate in the same EV_SYN as
 * the out-of-proximity notify. We need to ignore those, hence we only
 * actually post valuator events when we're in proximity.
 *
 * Other tablets send the x/y coordinates, then EV_SYN, then the proximity
 * event. For those, we need to remember x/y to post it when the proximity
 * comes.
 *
 * If we're not in proximity and we get valuator events, remember that, they
 * won't be posted though. If we move into proximity without valuators, use
 * the last ones we got and let the rest of the code post them.
 */
static int
EvdevProcessProximityState(InputInfoPtr pInfo)
{
    EvdevPtr pEvdev = pInfo->private;
    int prox_state = 0;
    int i;

    /* Does this device have any proximity axes? */
    if (!pEvdev->prox)
        return 0;

    /* no proximity change in the queue */
    if (!pEvdev->prox_queued)
    {
        if (pEvdev->abs_queued && !pEvdev->in_proximity)
            for (i = 0; i < valuator_mask_size(pEvdev->vals); i++)
                if (valuator_mask_isset(pEvdev->vals, i))
                    valuator_mask_set(pEvdev->prox, i,
                                      valuator_mask_get(pEvdev->vals, i));
        return 0;
    }

    for (i = 0; i < pEvdev->num_queue; i++)
    {
        if (pEvdev->queue[i].type == EV_QUEUE_PROXIMITY)
        {
            prox_state = pEvdev->queue[i].val;
            break;
        }
    }

    if ((prox_state && !pEvdev->in_proximity) ||
        (!prox_state && pEvdev->in_proximity))
    {
        /* We're about to go into/out of proximity but have no abs events
         * within the EV_SYN. Use the last coordinates we have. */
        for (i = 0; i < valuator_mask_size(pEvdev->prox); i++)
            if (!valuator_mask_isset(pEvdev->vals, i) &&
                valuator_mask_isset(pEvdev->prox, i))
                valuator_mask_set(pEvdev->vals, i,
                                  valuator_mask_get(pEvdev->prox, i));
        valuator_mask_zero(pEvdev->prox);

        pEvdev->abs_queued = valuator_mask_size(pEvdev->vals);
    }

    pEvdev->in_proximity = prox_state;
    return 1;
}

/**
 * Take a button input event and process it accordingly.
 */
static void
EvdevProcessButtonEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    unsigned int button;
    int value;
    EvdevPtr pEvdev = pInfo->private;

    button = EvdevUtilButtonEventToButtonNumber(pEvdev, ev->code);

    /* Get the signed value, earlier kernels had this as unsigned */
    value = ev->value;

    /* Handle drag lock */
    if (EvdevDragLockFilterEvent(pInfo, button, value))
        return;

    if (EvdevWheelEmuFilterButton(pInfo, button, value))
        return;

    if (EvdevMBEmuFilterEvent(pInfo, button, value))
        return;

    if (button)
        EvdevQueueButtonEvent(pInfo, button, value);
    else
        EvdevQueueKbdEvent(pInfo, ev, value);
}

/**
 * Take the relative motion input event and process it accordingly.
 */
static void
EvdevProcessRelativeMotionEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    int value;
    EvdevPtr pEvdev = pInfo->private;
    int map;

    /* Get the signed value, earlier kernels had this as unsigned */
    value = ev->value;

    switch (ev->code) {
#ifndef HAVE_SMOOTH_SCROLLING
        case REL_WHEEL:
            if (value > 0)
                EvdevQueueButtonClicks(pInfo, wheel_up_button, value);
            else if (value < 0)
                EvdevQueueButtonClicks(pInfo, wheel_down_button, -value);
            break;

        case REL_DIAL:
        case REL_HWHEEL:
            if (value > 0)
                EvdevQueueButtonClicks(pInfo, wheel_right_button, value);
            else if (value < 0)
                EvdevQueueButtonClicks(pInfo, wheel_left_button, -value);
            break;
        /* We don't post wheel events as axis motion. */
#endif
        default:
            /* Ignore EV_REL events if we never set up for them. */
            if (!(pEvdev->flags & EVDEV_RELATIVE_EVENTS))
                return;

            /* Handle mouse wheel emulation */
            if (EvdevWheelEmuFilterMotion(pInfo, ev))
                return;

            pEvdev->rel_queued = 1;
            pEvdev->delta[ev->code] += value;
            map = pEvdev->axis_map[ev->code];
            valuator_mask_set(pEvdev->vals, map, value);
            break;
    }
}

#ifdef MULTITOUCH
static void
EvdevProcessTouch(InputInfoPtr pInfo)
{
    EvdevPtr pEvdev = pInfo->private;
    int type;

    if (pEvdev->cur_slot < 0 || !pEvdev->mt_mask)
        return;

    /* If the ABS_MT_SLOT is the first event we get after EV_SYN, skip this */
    if (pEvdev->slot_state == SLOTSTATE_EMPTY)
        return;

    if (pEvdev->slot_state == SLOTSTATE_CLOSE)
        type = XI_TouchEnd;
    else if (pEvdev->slot_state == SLOTSTATE_OPEN)
        type = XI_TouchBegin;
    else
        type = XI_TouchUpdate;


    EvdevQueueTouchEvent(pInfo, pEvdev->cur_slot, pEvdev->mt_mask, type);

    pEvdev->slot_state = SLOTSTATE_EMPTY;

    valuator_mask_zero(pEvdev->mt_mask);
}

static int
num_slots(EvdevPtr pEvdev)
{
    int value = pEvdev->absinfo[ABS_MT_SLOT].maximum -
                pEvdev->absinfo[ABS_MT_SLOT].minimum + 1;

    /* If we don't know how many slots there are, assume at least 10 */
    return value > 1 ? value : 10;
}

static int
last_mt_vals_slot(EvdevPtr pEvdev)
{
    int value = pEvdev->cur_slot - pEvdev->absinfo[ABS_MT_SLOT].minimum;

    return value < num_slots(pEvdev) ? value : -1;
}

static void
EvdevProcessTouchEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    EvdevPtr pEvdev = pInfo->private;
    int map;

    if (ev->code == ABS_MT_SLOT) {
        EvdevProcessTouch(pInfo);
        pEvdev->cur_slot = ev->value;
    } else
    {
        int slot_index = last_mt_vals_slot(pEvdev);

        if (pEvdev->slot_state == SLOTSTATE_EMPTY)
            pEvdev->slot_state = SLOTSTATE_UPDATE;
        if (ev->code == ABS_MT_TRACKING_ID) {
            if (ev->value >= 0) {
                pEvdev->slot_state = SLOTSTATE_OPEN;

                if (slot_index >= 0)
                    valuator_mask_copy(pEvdev->mt_mask,
                                       pEvdev->last_mt_vals[slot_index]);
                else
                    xf86IDrvMsg(pInfo, X_WARNING,
                                "Attempted to copy values from out-of-range "
                                "slot, touch events may be incorrect.\n");
            } else
                pEvdev->slot_state = SLOTSTATE_CLOSE;
        } else {
            map = pEvdev->axis_map[ev->code];
            valuator_mask_set(pEvdev->mt_mask, map, ev->value);
            if (slot_index >= 0)
                valuator_mask_set(pEvdev->last_mt_vals[slot_index], map,
                                  ev->value);
        }
    }
}
#else
#define EvdevProcessTouch(pInfo)
#define EvdevProcessTouchEvent(pInfo, ev)
#endif /* MULTITOUCH */

/**
 * Take the absolute motion input event and process it accordingly.
 */
static void
EvdevProcessAbsoluteMotionEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    int value;
    EvdevPtr pEvdev = pInfo->private;
    int map;

    /* Get the signed value, earlier kernels had this as unsigned */
    value = ev->value;
#ifdef _F_EVDEV_SUPPORT_GAMEPAD
    if(pEvdev->flags & EVDEV_GAMEPAD)
    {
      EvdevMappingGamepadAbsToKey(pInfo, ev);
      return;
    }
#endif//_F_EVDEV_SUPPORT_GAMEPAD

    /* Ignore EV_ABS events if we never set up for them. */
    if (!(pEvdev->flags & EVDEV_ABSOLUTE_EVENTS))
        return;

    if (ev->code > ABS_MAX)
        return;

    if (EvdevWheelEmuFilterMotion(pInfo, ev))
        return;

    if (ev->code >= ABS_MT_SLOT) {
        EvdevProcessTouchEvent(pInfo, ev);
        pEvdev->abs_queued = 1;
    } else if (!pEvdev->mt_mask) {
        map = pEvdev->axis_map[ev->code];

	if(map < 0)
	{
		xf86IDrvMsg(pInfo, X_INFO, "[EvdevProcessAbsoluteMotionEvent] Invalid valuator (=%d), value=%d\nThis is going to be skipped.", map, value);
		return;
	}

        valuator_mask_set(pEvdev->vals, map, value);
        pEvdev->abs_queued = 1;
    }
}

/**
 * Take the key press/release input event and process it accordingly.
 */
static void
EvdevProcessKeyEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    int value, i;
    EvdevPtr pEvdev = pInfo->private;

    /* Get the signed value, earlier kernels had this as unsigned */
    value = ev->value;

    /* don't repeat mouse buttons */
    if (ev->code >= BTN_MOUSE && ev->code < KEY_OK)
        if (value == 2)
            return;

#ifdef _F_EVDEV_SUPPORT_GAMEPAD
    if(pEvdev->flags & EVDEV_GAMEPAD)
    {
        EvdevMappingGamepadKeyToKey(pInfo, ev);
        if (ev->code == 0)
            return;
    }
#endif//_F_EVDEV_SUPPORT_GAMEPAD


    for (i = 0; i < ArrayLength(proximity_bits); i++)
    {
        if (ev->code == proximity_bits[i])
        {
            EvdevProcessProximityEvent(pInfo, ev);
            return;
        }
    }

    switch (ev->code) {
        case BTN_TOUCH:
            /* For devices that have but don't use proximity, use
             * BTN_TOUCH as the proximity notifier */
            if (!pEvdev->use_proximity)
                pEvdev->in_proximity = value ? ev->code : 0;
            if (!(pEvdev->flags & (EVDEV_TOUCHSCREEN | EVDEV_TABLET)) ||
                pEvdev->mt_mask)
                break;
            /* Treat BTN_TOUCH from devices that only have BTN_TOUCH as
             * BTN_LEFT. */
            ev->code = BTN_LEFT;
            /* Intentional fallthrough! */

        default:
            EvdevProcessButtonEvent(pInfo, ev);
            break;
    }
}

#ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_
static CARD32
EvdevRelativeMoveTimer(OsTimerPtr timer, CARD32 time, pointer arg)
{
    InputInfoPtr pInfo = (InputInfoPtr)arg;
    EvdevPtr pEvdev;

    if(pInfo) pEvdev = pInfo->private;
    else return 0;

    if(!pEvdev) return 0;
    if(pEvdev->rel_move_timer)
        TimerCancel(pEvdev->rel_move_timer);
    pEvdev->rel_move_timer = NULL;

    pEvdev->rel_move_status = 0;
    pEvdev->rel_move_ack = 0;
    int rc = XIDeleteDeviceProperty(pInfo->dev, prop_relative_move_status, TRUE);

    if (rc != Success)
    {
        xf86IDrvMsg(pInfo, X_ERROR, "[%s] Failed to delete device property (id:%d, prop=%d)\n", __FUNCTION__, pInfo->dev->id, prop_relative_move_status);
    }

    ErrorF("[%s] pEvdev->rel_move_status=%d\n", __FUNCTION__, pEvdev->rel_move_status);

    return 0;
}
#endif /* #ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_ */

#ifdef _F_GESTURE_EXTENSION_
static BOOL EvdevMTStatusGet(InputInfoPtr pInfo, MTSyncType sync)
{
    int i;
    static int nslots;
    int pressed = 0;
    const int first_press = XI_TouchEnd - XI_TouchBegin;
    EvdevPtr pEvdev = pInfo->private;

    if (!pEvdev || !pEvdev->mt_status )
    	 return FALSE;

    nslots = num_slots(pEvdev);
    for(i = 0; i < nslots; i++)
    {
        pressed += pEvdev->mt_status[i];

	 if (pressed > first_press)
	 	return FALSE;
    }

    if ((sync == MTOUCH_FRAME_SYNC_BEGIN) && (pressed == first_press))
        return TRUE;

    if ((sync == MTOUCH_FRAME_SYNC_END) && !pressed)
        return TRUE;

    return FALSE;
}

static void EvdevMTSync(InputInfoPtr pInfo, MTSyncType sync)
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

#ifdef __MTSYNC_DEBUG__
	xf86IDrvMsg(pInfo, X_INFO, "[EvdevMTSync] %s has been sent !\n",
		(sync==MTOUCH_FRAME_SYNC_BEGIN) ? "MTOUCH_FRAME_SYNC_BEGIN" : "MTOUCH_FRAME_SYNC_END");
#endif /* #ifdef __MTSYNC_DEBUG__ */
}
#endif /* #ifdef _F_GESTURE_EXTENSION_ */

static void
EvdevBlockHandler(pointer data, OSTimePtr pTimeout, pointer pRead)
{
    InputInfoPtr pInfo = (InputInfoPtr)data;
    EvdevPtr pEvdev = pInfo->private;

    RemoveBlockAndWakeupHandlers(EvdevBlockHandler,
                                 (WakeupHandlerProcPtr)NoopDDA,
                                 data);
    pEvdev->block_handler_registered = FALSE;
    ErrorF("Block Handler Called, [%d]pEvdev->rel_move_status %d, pEvdev->rel_move_status_ack %d\n", pInfo->dev->id, pEvdev->rel_move_status, pEvdev->rel_move_ack);


    int rc = XIChangeDeviceProperty(pInfo->dev, prop_relative_move_status, XA_INTEGER, 8,
				PropModeReplace, 1, &pEvdev->rel_move_status, TRUE);

    if (rc != Success)
    {
        xf86IDrvMsg(pInfo, X_ERROR, "[%s] Failed to change device property (id:%d, prop=%d)\n", __FUNCTION__, pInfo->dev->id, prop_relative_move_status);
    }
}

/**
 * Post the relative motion events.
 */
void
EvdevPostRelativeMotionEvents(InputInfoPtr pInfo, int num_v, int first_v,
                              int v[MAX_VALUATORS])
{
    EvdevPtr pEvdev = pInfo->private;

    if (pEvdev->rel_queued) {
#ifdef _F_EVDEV_SUPPORT_ROTARY_
        if (pEvdev->flags & EVDEV_OFM) {
            pEvdev->extra_rel_post_ofm(pInfo, num_v, first_v, v);
            return;
        }

        if (pEvdev->flags & EVDEV_HALLIC) {
            pEvdev->extra_rel_post_hallic(pInfo, num_v, first_v, v);
            return;
        }
#endif //_F_EVDEV_SUPPORT_ROTARY_
#ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_
	if(!pEvdev->rel_move_prop_set)
		pEvdev->rel_move_prop_set = 1;

	if((!pEvdev->block_handler_registered) && (!pEvdev->rel_move_status || !pEvdev->rel_move_ack))
	{
		pEvdev->rel_move_status = 1;
		pEvdev->block_handler_registered = TRUE;
		RegisterBlockAndWakeupHandlers(EvdevBlockHandler ,(WakeupHandlerProcPtr) NoopDDA, pInfo);
	}

	TimerCancel(pEvdev->rel_move_timer);
	pEvdev->rel_move_timer = NULL;
	pEvdev->rel_move_timer = TimerSet(pEvdev->rel_move_timer, 0, 15000, EvdevRelativeMoveTimer, pInfo);
#endif /* #ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_ */
        xf86PostMotionEventM(pInfo->dev, Relative, pEvdev->vals);
    }
}

/**
 * Post the absolute motion events.
 */
void
EvdevPostAbsoluteMotionEvents(InputInfoPtr pInfo, int num_v, int first_v,
                              int v[MAX_VALUATORS])
{
    EvdevPtr pEvdev = pInfo->private;

    /*
     * Some devices only generate valid abs coords when BTN_TOOL_PEN is
     * pressed.  On wacom tablets, this means that the pen is in
     * proximity of the tablet.  After the pen is removed, BTN_TOOL_PEN is
     * released, and a (0, 0) absolute event is generated.  Checking
     * pEvdev->in_proximity here lets us ignore that event.
     * pEvdev->in_proximity is initialized to 1 so devices that don't use
     * this scheme still just work.
     */
    if (pEvdev->abs_queued && pEvdev->in_proximity) {
        xf86PostMotionEventM(pInfo->dev, Absolute, pEvdev->vals);
    }
}

static void
EvdevPostProximityEvents(InputInfoPtr pInfo, int which, int num_v, int first_v,
                                  int v[MAX_VALUATORS])
{
    int i;
    EvdevPtr pEvdev = pInfo->private;

    for (i = 0; pEvdev->prox_queued && i < pEvdev->num_queue; i++) {
        switch (pEvdev->queue[i].type) {
            case EV_QUEUE_KEY:
            case EV_QUEUE_BTN:
#ifdef MULTITOUCH
            case EV_QUEUE_TOUCH:
#endif
                break;
            case EV_QUEUE_PROXIMITY:
                if (pEvdev->queue[i].val == which)
                    xf86PostProximityEventP(pInfo->dev, which, first_v, num_v,
                            v + first_v);
                break;
        }
    }
}

/**
 * Post the queued key/button events.
 */
static void EvdevPostQueuedEvents(InputInfoPtr pInfo, int num_v, int first_v,
                                  int v[MAX_VALUATORS])
{
    int i;
#ifdef _F_GESTURE_EXTENSION_
    int sync_value;
    int event_type;
    int slot_idx;
    int pressed = 0;
#endif /* #ifdef _F_GESTURE_EXTENSION_ */
#ifdef _F_TOUCH_TRANSFORM_MATRIX_
    pixman_vector_t p;
    static int lastx;
    static int lasty;
#endif /* #ifdef _F_TOUCH_TRANSFORM_MATRIX_ */

    EvdevPtr pEvdev = pInfo->private;

    for (i = 0; i < pEvdev->num_queue; i++) {
        switch (pEvdev->queue[i].type) {
        case EV_QUEUE_KEY:
            xf86PostKeyboardEvent(pInfo->dev, pEvdev->queue[i].detail.key,
                                  pEvdev->queue[i].val);
            break;
        case EV_QUEUE_BTN:
            if (Evdev3BEmuFilterEvent(pInfo,
                                      pEvdev->queue[i].detail.key,
                                      pEvdev->queue[i].val))
                break;

            if (pEvdev->abs_queued && pEvdev->in_proximity) {
                xf86PostButtonEventP(pInfo->dev, Absolute, pEvdev->queue[i].detail.key,
                                     pEvdev->queue[i].val, first_v, num_v,
                                     v + first_v);

            } else
                xf86PostButtonEvent(pInfo->dev, Relative, pEvdev->queue[i].detail.key,
                                    pEvdev->queue[i].val, 0, 0);
            break;
        case EV_QUEUE_PROXIMITY:
            break;
#ifdef MULTITOUCH
        case EV_QUEUE_TOUCH:
#ifdef _F_TOUCH_TRANSFORM_MATRIX_
            if( pEvdev->use_transform )
            {
                int x, y, slot;

                x = valuator_mask_get(pEvdev->queue[i].touchMask, 0);
                y = valuator_mask_get(pEvdev->queue[i].touchMask, 1);

                if(x || y)
                {
			slot = pEvdev->queue[i].detail.touch;
			x = valuator_mask_get(pEvdev->last_mt_vals[slot], 0);
			y = valuator_mask_get(pEvdev->last_mt_vals[slot], 1);

			p.vector[0] = pixman_int_to_fixed(x);
			p.vector[1] = pixman_int_to_fixed(y);
			p.vector[2] = pixman_int_to_fixed(1);

			pixman_transform_point(&pEvdev->inv_transform, &p);

			valuator_mask_set(pEvdev->queue[i].touchMask, 0, pixman_fixed_to_int(p.vector[0]));
			valuator_mask_set(pEvdev->queue[i].touchMask, 1, pixman_fixed_to_int(p.vector[1]));
                }
            }
#endif /* #ifdef _F_TOUCH_TRANSFORM_MATRIX_ */
#ifdef _F_GESTURE_EXTENSION_
            sync_value = -1;
            event_type = pEvdev->queue[i].val;
            slot_idx = pEvdev->queue[i].detail.touch;

            if (pEvdev->mt_status)
            {
                pEvdev->mt_status[slot_idx] = XI_TouchEnd - event_type;

                if ((XI_TouchBegin == event_type) && (slot_idx == 0))
                {
	                if (EvdevMTStatusGet(pInfo, MTOUCH_FRAME_SYNC_BEGIN))
	                {
				sync_value = MTOUCH_FRAME_SYNC_BEGIN;
	                	EvdevMTSync(pInfo, MTOUCH_FRAME_SYNC_BEGIN);
	                }
                }

                xf86PostTouchEvent(pInfo->dev, slot_idx,
                               event_type, 0,
                               pEvdev->queue[i].touchMask);

                if ((sync_value < 0) && (XI_TouchEnd == event_type))
                {
	                if (EvdevMTStatusGet(pInfo, MTOUCH_FRAME_SYNC_END))
	                {
	                	EvdevMTSync(pInfo, MTOUCH_FRAME_SYNC_END);
	                }
                }
            }
#else /* #ifdef _F_GESTURE_EXTENSION_ */
            xf86PostTouchEvent(pInfo->dev, pEvdev->queue[i].detail.touch,
                               pEvdev->queue[i].val, 0,
                               pEvdev->queue[i].touchMask);
#endif /* #ifdef _F_GESTURE_EXTENSION_ */
            break;
#endif
        }
    }
}

/**
 * Take the synchronization input event and process it accordingly; the motion
 * notify events are sent first, then any button/key press/release events.
 */
static void
EvdevProcessSyncEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    int i;
    int num_v = 0, first_v = 0;
    int v[MAX_VALUATORS] = {};
    EvdevPtr pEvdev = pInfo->private;

    EvdevProcessProximityState(pInfo);

    EvdevProcessValuators(pInfo);
    EvdevProcessTouch(pInfo);

    EvdevPostProximityEvents(pInfo, TRUE, num_v, first_v, v);
    EvdevPostRelativeMotionEvents(pInfo, num_v, first_v, v);
    EvdevPostAbsoluteMotionEvents(pInfo, num_v, first_v, v);
    EvdevPostQueuedEvents(pInfo, num_v, first_v, v);
    EvdevPostProximityEvents(pInfo, FALSE, num_v, first_v, v);

    memset(pEvdev->delta, 0, sizeof(pEvdev->delta));
    for (i = 0; i < ArrayLength(pEvdev->queue); i++)
    {
        EventQueuePtr queue = &pEvdev->queue[i];
        queue->detail.key = 0;
        queue->type = 0;
        queue->val = 0;
        /* don't reset the touchMask */
    }

    if (pEvdev->vals)
        valuator_mask_zero(pEvdev->vals);
    pEvdev->num_queue = 0;
    pEvdev->abs_queued = 0;
    pEvdev->rel_queued = 0;
    pEvdev->prox_queued = 0;

}

/**
 * Process the events from the device; nothing is actually posted to the server
 * until an EV_SYN event is received.
 */
static void
EvdevProcessEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    switch (ev->type) {
        case EV_REL:
            EvdevProcessRelativeMotionEvent(pInfo, ev);
            break;
        case EV_ABS:
            EvdevProcessAbsoluteMotionEvent(pInfo, ev);
            break;
        case EV_KEY:
            EvdevProcessKeyEvent(pInfo, ev);
            break;
        case EV_SYN:
            EvdevProcessSyncEvent(pInfo, ev);
            break;
    }
}

#undef ABS_X_VALUE
#undef ABS_Y_VALUE
#undef ABS_VALUE

static void
EvdevFreeMasks(EvdevPtr pEvdev)
{
    int i;

    valuator_mask_free(&pEvdev->vals);
    valuator_mask_free(&pEvdev->old_vals);
    valuator_mask_free(&pEvdev->prox);
#ifdef MULTITOUCH
    valuator_mask_free(&pEvdev->mt_mask);
    if (pEvdev->last_mt_vals)
    {
        for (i = 0; i < num_slots(pEvdev); i++)
            valuator_mask_free(&pEvdev->last_mt_vals[i]);
        free(pEvdev->last_mt_vals);
        pEvdev->last_mt_vals = NULL;
    }
    for (i = 0; i < EVDEV_MAXQUEUE; i++)
        valuator_mask_free(&pEvdev->queue[i].touchMask);
#endif
}

/* just a magic number to reduce the number of reads */
#define NUM_EVENTS 16

static void
EvdevReadInput(InputInfoPtr pInfo)
{
    struct input_event ev[NUM_EVENTS];
    int i, len = sizeof(ev);

    while (len == sizeof(ev))
    {
#ifdef MULTITOUCH
        EvdevPtr pEvdev = pInfo->private;

        if (pEvdev->mtdev)
            len = mtdev_get(pEvdev->mtdev, pInfo->fd, ev, NUM_EVENTS) *
                sizeof(struct input_event);
        else
#endif
            len = read(pInfo->fd, &ev, sizeof(ev));

        if (len <= 0)
        {
            if (errno == ENODEV) /* May happen after resume */
            {
                EvdevMBEmuFinalize(pInfo);
                Evdev3BEmuFinalize(pInfo);
                xf86RemoveEnabledDevice(pInfo);
                close(pInfo->fd);
                pInfo->fd = -1;
            } else if (errno != EAGAIN)
            {
                /* We use X_NONE here because it doesn't alloc */
                xf86MsgVerb(X_NONE, 0, "%s: Read error: %s\n", pInfo->name,
                        strerror(errno));
            }
            break;
        }

        /* The kernel promises that we always only read a complete
         * event, so len != sizeof ev is an error. */
        if (len % sizeof(ev[0])) {
            /* We use X_NONE here because it doesn't alloc */
            xf86MsgVerb(X_NONE, 0, "%s: Read error: %s\n", pInfo->name, strerror(errno));
            break;
        }

        for (i = 0; i < len/sizeof(ev[0]); i++)
            EvdevProcessEvent(pInfo, &ev[i]);
    }
}

static void
EvdevPtrCtrlProc(DeviceIntPtr device, PtrCtrl *ctrl)
{
    /* Nothing to do, dix handles all settings */
}

static void
EvdevKbdCtrl(DeviceIntPtr device, KeybdCtrl *ctrl)
{
    static struct { int xbit, code; } bits[] = {
        { CAPSFLAG,	LED_CAPSL },
        { NUMFLAG,	LED_NUML },
        { SCROLLFLAG,	LED_SCROLLL },
        { MODEFLAG,	LED_KANA },
        { COMPOSEFLAG,	LED_COMPOSE }
    };

    InputInfoPtr pInfo;
    struct input_event ev[ArrayLength(bits)];
    int i;

    memset(ev, 0, sizeof(ev));

    pInfo = device->public.devicePrivate;
    for (i = 0; i < ArrayLength(bits); i++) {
        ev[i].type = EV_LED;
        ev[i].code = bits[i].code;
        ev[i].value = (ctrl->leds & bits[i].xbit) > 0;
    }

    write(pInfo->fd, ev, sizeof ev);
}

#ifdef _F_USE_DEFAULT_XKB_RULES_
void
EvdevGetXkbRules(DeviceIntPtr device, XkbRMLVOSet * rmlvo)
{
    WindowPtr root=NULL;
    PropertyPtr pProp;
    int rc=0;
    char * keymap;
    if(screenInfo.numScreens > 0 && screenInfo.screens[0])
    {
        root = screenInfo.screens[0]->root;
    }
    else
        return;

    if( prop_xkb_rules == None )
        prop_xkb_rules = MakeAtom("_XKB_RULES_NAMES", strlen("_XKB_RULES_NAMES"), TRUE);

    rc = dixLookupProperty (&pProp, root, prop_xkb_rules, serverClient, DixReadAccess);
    if (rc == Success && pProp->data){
        keymap = (char *)pProp->data;
        rmlvo->rules = keymap;
        keymap = keymap+strlen(keymap)+1;
        rmlvo->model = keymap;
        keymap = keymap+strlen(keymap)+1;
        rmlvo->layout = keymap;
        keymap = keymap+strlen(keymap)+1;
        rmlvo->variant = keymap;
        keymap = keymap+strlen(keymap)+1;
        rmlvo->options = keymap;
    }
    else
    {
        XkbGetRulesDflts(rmlvo);
    }
}
#endif //_F_USE_DEFAULT_XKB_RULES_

static int
EvdevAddKeyClass(DeviceIntPtr device)
{
    InputInfoPtr pInfo;
    EvdevPtr pEvdev;

    pInfo = device->public.devicePrivate;
    pEvdev = pInfo->private;

#ifdef _F_USE_DEFAULT_XKB_RULES_
    XkbRMLVOSet dflts = { NULL };

    if (pEvdev->use_default_xkb_rmlvo)
    {
        EvdevGetXkbRules(device, &dflts);

        pEvdev->rmlvo.rules = (dflts.rules) ? strdup(dflts.rules) : NULL;
        pEvdev->rmlvo.model = (dflts.model) ? strdup(dflts.model) : NULL;
        pEvdev->rmlvo.layout = (dflts.layout) ? strdup(dflts.layout) : NULL;
        pEvdev->rmlvo.variant = (dflts.variant) ? strdup(dflts.variant) : NULL;
        pEvdev->rmlvo.options = (dflts.options) ? strdup(dflts.options) : NULL;

        ErrorF("[%s] Set default XKB RMLVO !\n", __FUNCTION__);

        ErrorF("[%s] pEvdev->rmlvo.rules=%s\n", __FUNCTION__, pEvdev->rmlvo.rules ? pEvdev->rmlvo.rules : "NULL");
        ErrorF("[%s] pEvdev->rmlvo.model=%s\n", __FUNCTION__, pEvdev->rmlvo.model ? pEvdev->rmlvo.model : "NULL");
        ErrorF("[%s] pEvdev->rmlvo.layout=%s\n", __FUNCTION__, pEvdev->rmlvo.layout ? pEvdev->rmlvo.layout : "NULL");
        ErrorF("[%s] pEvdev->rmlvo.variant=%s\n", __FUNCTION__, pEvdev->rmlvo.variant ? pEvdev->rmlvo.variant : "NULL");
        ErrorF("[%s] pEvdev->rmlvo.options=%s\n", __FUNCTION__, pEvdev->rmlvo.options ? pEvdev->rmlvo.options : "NULL");
    }
    else
    {
#endif
    /* sorry, no rules change allowed for you */
    xf86ReplaceStrOption(pInfo->options, "xkb_rules", "evdev");
    SetXkbOption(pInfo, "xkb_rules", &pEvdev->rmlvo.rules);
    SetXkbOption(pInfo, "xkb_model", &pEvdev->rmlvo.model);
    if (!pEvdev->rmlvo.model)
        SetXkbOption(pInfo, "XkbModel", &pEvdev->rmlvo.model);
    SetXkbOption(pInfo, "xkb_layout", &pEvdev->rmlvo.layout);
    if (!pEvdev->rmlvo.layout)
        SetXkbOption(pInfo, "XkbLayout", &pEvdev->rmlvo.layout);
    SetXkbOption(pInfo, "xkb_variant", &pEvdev->rmlvo.variant);
    if (!pEvdev->rmlvo.variant)
        SetXkbOption(pInfo, "XkbVariant", &pEvdev->rmlvo.variant);
    SetXkbOption(pInfo, "xkb_options", &pEvdev->rmlvo.options);
    if (!pEvdev->rmlvo.options)
        SetXkbOption(pInfo, "XkbOptions", &pEvdev->rmlvo.options);
#ifdef _F_USE_DEFAULT_XKB_RULES_
    }
#endif

    if (!InitKeyboardDeviceStruct(device, &pEvdev->rmlvo, NULL, EvdevKbdCtrl))
        return !Success;

#ifdef _F_REMAP_KEYS_
    SetRemapOption(pInfo,"event_key_remap");
#endif //_F_REMAP_KEYS_

    return Success;
}

#ifdef MULTITOUCH
/* MT axes are counted twice - once as ABS_X (which the kernel keeps for
 * backwards compatibility), once as ABS_MT_POSITION_X. So we need to keep a
 * mapping of those axes to make sure we only count them once
 */
struct mt_axis_mappings {
    int mt_code;
    int code;
    Bool needs_mapping; /* TRUE if both code and mt_code are present */
    int mapping;        /* Logical mapping of 'code' axis */
};

static struct mt_axis_mappings mt_axis_mappings[] = {
    {ABS_MT_POSITION_X, ABS_X},
    {ABS_MT_POSITION_Y, ABS_Y},
    {ABS_MT_PRESSURE, ABS_PRESSURE},
    {ABS_MT_DISTANCE, ABS_DISTANCE},
};
#endif

/**
 * return TRUE if the axis is not one we should count as true axis
 */
static int
is_blacklisted_axis(int axis)
{
    switch(axis)
    {
        case ABS_MT_SLOT:
        case ABS_MT_TRACKING_ID:
            return TRUE;
        default:
            return FALSE;
    }
}


static int
EvdevAddAbsValuatorClass(DeviceIntPtr device)
{
    InputInfoPtr pInfo;
    EvdevPtr pEvdev;
    int num_axes, axis, i = 0;
    int num_mt_axes = 0, /* number of MT-only axes */
        num_mt_axes_total = 0; /* total number of MT axes, including
                                  double-counted ones, excluding blacklisted */
    Atom *atoms = NULL;

    pInfo = device->public.devicePrivate;
    pEvdev = pInfo->private;

    if (!EvdevBitIsSet(pEvdev->bitmask, EV_ABS))
        goto out;

    num_axes = EvdevCountBits(pEvdev->abs_bitmask, NLONGS(ABS_MAX));
    if (num_axes < 1)
        goto out;

#ifdef MULTITOUCH
    for (axis = ABS_MT_SLOT; axis < ABS_MAX; axis++)
    {
        if (EvdevBitIsSet(pEvdev->abs_bitmask, axis))
        {
            int j;
            Bool skip = FALSE;

            for (j = 0; j < ArrayLength(mt_axis_mappings); j++)
            {
                if (mt_axis_mappings[j].mt_code == axis &&
                    BitIsOn(pEvdev->abs_bitmask, mt_axis_mappings[j].code))
                {
                    mt_axis_mappings[j].needs_mapping = TRUE;
                    skip = TRUE;
                }
            }

            if (!is_blacklisted_axis(axis))
            {
                num_mt_axes_total++;
                if (!skip)
                    num_mt_axes++;
            }
            num_axes--;
        }
    }
#endif
    if (num_axes + num_mt_axes > MAX_VALUATORS) {
        xf86IDrvMsg(pInfo, X_WARNING, "found %d axes, limiting to %d.\n", num_axes, MAX_VALUATORS);
        num_axes = MAX_VALUATORS;
    }

    if (num_axes < 1 && num_mt_axes_total < 1) {
        xf86Msg(X_WARNING, "%s: no absolute or touch axes found.\n",
                device->name);
        return !Success;
    }

    pEvdev->num_vals = num_axes;
    if (num_axes > 0) {
        pEvdev->vals = valuator_mask_new(num_axes);
        pEvdev->old_vals = valuator_mask_new(num_axes);
        if (!pEvdev->vals || !pEvdev->old_vals) {
            xf86IDrvMsg(pInfo, X_ERROR, "failed to allocate valuator masks.\n");
            goto out;
        }
    }
#ifdef MULTITOUCH
    if (num_mt_axes_total > 0) {
        pEvdev->mt_mask = valuator_mask_new(num_mt_axes_total);
        if (!pEvdev->mt_mask) {
            xf86Msg(X_ERROR, "%s: failed to allocate MT valuator mask.\n",
                    device->name);
            goto out;
        }

        pEvdev->last_mt_vals = calloc(num_slots(pEvdev), sizeof(ValuatorMask *));
        if (!pEvdev->last_mt_vals) {
            xf86IDrvMsg(pInfo, X_ERROR,
                        "%s: failed to allocate MT last values mask array.\n",
                        device->name);
            goto out;
        }

        for (i = 0; i < num_slots(pEvdev); i++) {
            pEvdev->last_mt_vals[i] = valuator_mask_new(num_mt_axes_total);
            if (!pEvdev->last_mt_vals[i]) {
                xf86IDrvMsg(pInfo, X_ERROR,
                            "%s: failed to allocate MT last values mask.\n",
                            device->name);
                goto out;
            }
        }

        for (i = 0; i < EVDEV_MAXQUEUE; i++) {
            pEvdev->queue[i].touchMask =
                valuator_mask_new(num_mt_axes_total);
            if (!pEvdev->queue[i].touchMask) {
                xf86Msg(X_ERROR, "%s: failed to allocate MT valuator masks for "
                        "evdev event queue.\n", device->name);
                goto out;
            }
        }
    }
#endif
    atoms = malloc((pEvdev->num_vals + num_mt_axes) * sizeof(Atom));

    i = 0;
    for (axis = ABS_X; i < MAX_VALUATORS && axis <= ABS_MAX; axis++) {
        int j;
        int mapping;
        pEvdev->axis_map[axis] = -1;
        if (!EvdevBitIsSet(pEvdev->abs_bitmask, axis) ||
            is_blacklisted_axis(axis))
            continue;

        mapping = i;

#ifdef MULTITOUCH
        for (j = 0; j < ArrayLength(mt_axis_mappings); j++)
        {
            if (mt_axis_mappings[j].code == axis)
                mt_axis_mappings[j].mapping = mapping;
            else if (mt_axis_mappings[j].mt_code == axis &&
                    mt_axis_mappings[j].needs_mapping)
                mapping = mt_axis_mappings[j].mapping;
        }
#endif
        pEvdev->axis_map[axis] = mapping;
        if (mapping == i)
            i++;
    }

    EvdevInitAxesLabels(pEvdev, Absolute, pEvdev->num_vals + num_mt_axes, atoms);

    if (!InitValuatorClassDeviceStruct(device, num_axes + num_mt_axes, atoms,
                                       GetMotionHistorySize(), Absolute)) {
        xf86IDrvMsg(pInfo, X_ERROR, "failed to initialize valuator class device.\n");
        goto out;
    }

#ifdef MULTITOUCH
    if (num_mt_axes_total > 0)
    {
        int num_touches = 0;
        int mode = pEvdev->flags & EVDEV_TOUCHPAD ?
            XIDependentTouch : XIDirectTouch;

        if (pEvdev->mtdev && pEvdev->mtdev->caps.slot.maximum > 0)
            num_touches = pEvdev->mtdev->caps.slot.maximum;

        if (!InitTouchClassDeviceStruct(device, num_touches, mode,
                                        num_mt_axes_total)) {
            xf86Msg(X_ERROR, "%s: failed to initialize touch class device.\n",
                    device->name);
            goto out;
        }

        for (i = 0; i < num_slots(pEvdev); i++) {
            for (axis = ABS_MT_TOUCH_MAJOR; axis < ABS_MAX; axis++) {
                if (pEvdev->axis_map[axis] >= 0) {
                    /* XXX: read initial values from mtdev when it adds support
                     *      for doing so. */
                    valuator_mask_set(pEvdev->last_mt_vals[i],
                                      pEvdev->axis_map[axis], 0);
                }
            }
        }
    }
#endif

    for (axis = ABS_X; axis < ABS_MT_SLOT; axis++) {
        int axnum = pEvdev->axis_map[axis];
        int resolution = 0;

        if (axnum == -1)
            continue;

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 30)
        /* Kernel provides units/mm, X wants units/m */
        if (pEvdev->absinfo[axis].resolution)
            resolution = pEvdev->absinfo[axis].resolution * 1000;
#endif

        xf86InitValuatorAxisStruct(device, axnum,
                                   atoms[axnum],
                                   pEvdev->absinfo[axis].minimum,
                                   pEvdev->absinfo[axis].maximum,
                                   resolution, 0, resolution, Absolute);
        xf86InitValuatorDefaults(device, axnum);
    }

#ifdef MULTITOUCH
    for (axis = ABS_MT_TOUCH_MAJOR; axis <= ABS_MAX; axis++) {
        int axnum = pEvdev->axis_map[axis];
        int resolution = 0;
        int j;
        BOOL skip = FALSE;

        if (axnum < 0)
            continue;

        for (j = 0; j < ArrayLength(mt_axis_mappings); j++)
            if (mt_axis_mappings[j].mt_code == axis &&
                    mt_axis_mappings[j].needs_mapping)
            {
                skip = TRUE;
                break;
            }

        /* MT axis is mapped, don't set up twice */
        if (skip)
            continue;

        if (pEvdev->absinfo[axis].resolution)
            resolution = pEvdev->absinfo[axis].resolution * 1000;

        xf86InitValuatorAxisStruct(device, axnum,
                                   atoms[axnum],
                                   pEvdev->absinfo[axis].minimum,
                                   pEvdev->absinfo[axis].maximum,
                                   resolution, 0, resolution,
                                   Absolute);
    }
#endif

    free(atoms);
    atoms = NULL;

    for (i = 0; i < ArrayLength(proximity_bits); i++)
    {
        if (!pEvdev->use_proximity)
            break;

        if (EvdevBitIsSet(pEvdev->key_bitmask, proximity_bits[i]))
        {
            InitProximityClassDeviceStruct(device);
            pEvdev->prox = valuator_mask_new(num_axes);
            if (!pEvdev->prox) {
                xf86IDrvMsg(pInfo, X_ERROR,
                            "failed to allocate proximity valuator " "mask.\n");
                goto out;
            }
            break;
        }
    }

    if (!InitPtrFeedbackClassDeviceStruct(device, EvdevPtrCtrlProc)) {
        xf86IDrvMsg(pInfo, X_ERROR,
                    "failed to initialize pointer feedback class device.\n");
        goto out;
    }

    if (pEvdev->flags & EVDEV_TOUCHPAD)
        pEvdev->flags |= EVDEV_RELATIVE_MODE;
    else
        pEvdev->flags &= ~EVDEV_RELATIVE_MODE;

    if (xf86FindOption(pInfo->options, "Mode"))
    {
        char *mode;
        mode = xf86SetStrOption(pInfo->options, "Mode", NULL);
        if (!strcasecmp("absolute", mode))
            pEvdev->flags &= ~EVDEV_RELATIVE_MODE;
        else if (!strcasecmp("relative", mode))
            pEvdev->flags |= EVDEV_RELATIVE_MODE;
        else
            xf86IDrvMsg(pInfo, X_INFO, "unknown mode, use default\n");
        free(mode);
    }

    return Success;

out:
    EvdevFreeMasks(pEvdev);
    if (atoms)
        free(atoms);
    return !Success;
}

static int
EvdevAddRelValuatorClass(DeviceIntPtr device)
{
    InputInfoPtr pInfo;
    EvdevPtr pEvdev;
    int num_axes, axis, i = 0;
    Atom *atoms = NULL;

    pInfo = device->public.devicePrivate;
    pEvdev = pInfo->private;

    if (!EvdevBitIsSet(pEvdev->bitmask, EV_REL))
        goto out;

    num_axes = EvdevCountBits(pEvdev->rel_bitmask, NLONGS(REL_MAX));
    if (num_axes < 1)
        goto out;

#ifndef HAVE_SMOOTH_SCROLLING
    /* Wheels are special, we post them as button events. So let's ignore them
     * in the axes list too */
    if (EvdevBitIsSet(pEvdev->rel_bitmask, REL_WHEEL))
        num_axes--;
    if (EvdevBitIsSet(pEvdev->rel_bitmask, REL_HWHEEL))
        num_axes--;
    if (EvdevBitIsSet(pEvdev->rel_bitmask, REL_DIAL))
        num_axes--;

    if (num_axes <= 0)
        goto out;
#endif

    if (num_axes > MAX_VALUATORS) {
        xf86IDrvMsg(pInfo, X_WARNING, "found %d axes, limiting to %d.\n", num_axes, MAX_VALUATORS);
        num_axes = MAX_VALUATORS;
    }

    pEvdev->num_vals = num_axes;
    if (num_axes > 0) {
        pEvdev->vals = valuator_mask_new(num_axes);
        if (!pEvdev->vals)
            goto out;
    }
    atoms = malloc(pEvdev->num_vals * sizeof(Atom));

    for (axis = REL_X; i < MAX_VALUATORS && axis <= REL_MAX; axis++)
    {
        pEvdev->axis_map[axis] = -1;
#ifndef HAVE_SMOOTH_SCROLLING
        /* We don't post wheel events, so ignore them here too */
        if (axis == REL_WHEEL || axis == REL_HWHEEL || axis == REL_DIAL)
            continue;
#endif
        if (!EvdevBitIsSet(pEvdev->rel_bitmask, axis))
            continue;
        pEvdev->axis_map[axis] = i;
        i++;
    }

    EvdevInitAxesLabels(pEvdev, Relative, pEvdev->num_vals, atoms);

    if (!InitValuatorClassDeviceStruct(device, num_axes, atoms,
                                       GetMotionHistorySize(), Relative)) {
        xf86IDrvMsg(pInfo, X_ERROR, "failed to initialize valuator class device.\n");
        goto out;
    }

    if (!InitPtrFeedbackClassDeviceStruct(device, EvdevPtrCtrlProc)) {
        xf86IDrvMsg(pInfo, X_ERROR, "failed to initialize pointer feedback class "
                "device.\n");
        goto out;
    }

    for (axis = REL_X; axis <= REL_MAX; axis++)
    {
        int axnum = pEvdev->axis_map[axis];

        if (axnum == -1)
            continue;
        xf86InitValuatorAxisStruct(device, axnum, atoms[axnum], -1, -1, 1, 0, 1,
                                   Relative);
        xf86InitValuatorDefaults(device, axnum);
#ifdef HAVE_SMOOTH_SCROLLING
        if (axis == REL_WHEEL)
            SetScrollValuator(device, axnum, SCROLL_TYPE_VERTICAL, -1.0, SCROLL_FLAG_PREFERRED);
        else if (axis == REL_DIAL)
            SetScrollValuator(device, axnum, SCROLL_TYPE_VERTICAL, -1.0, SCROLL_FLAG_NONE);
        else if (axis == REL_HWHEEL)
            SetScrollValuator(device, axnum, SCROLL_TYPE_HORIZONTAL, -1.0, SCROLL_FLAG_NONE);
#endif
    }

    free(atoms);

    return Success;

out:
    valuator_mask_free(&pEvdev->vals);
    if (atoms)
        free(atoms);
    return !Success;
}

static int
EvdevAddButtonClass(DeviceIntPtr device)
{
    InputInfoPtr pInfo;
    EvdevPtr pEvdev;
    Atom *labels = NULL;

    pInfo = device->public.devicePrivate;
    pEvdev = pInfo->private;

    labels = malloc(pEvdev->num_buttons * sizeof(Atom));
    if (!labels) return BadAlloc;
    EvdevInitButtonLabels(pEvdev, pEvdev->num_buttons, labels);

    if (!InitButtonClassDeviceStruct(device, pEvdev->num_buttons, labels,
                                     pEvdev->btnmap))
    {
	 free(labels);
        return !Success;
    }

    free(labels);
    return Success;
}

/**
 * Init the button mapping for the device. By default, this is a 1:1 mapping,
 * i.e. Button 1 maps to Button 1, Button 2 to 2, etc.
 *
 * If a mapping has been specified, the mapping is the default, with the
 * user-defined ones overwriting the defaults.
 * i.e. a user-defined mapping of "3 2 1" results in a mapping of 3 2 1 4 5 6 ...
 *
 * Invalid button mappings revert to the default.
 *
 * Note that index 0 is unused, button 0 does not exist.
 * This mapping is initialised for all devices, but only applied if the device
 * has buttons (in EvdevAddButtonClass).
 */
static void
EvdevInitButtonMapping(InputInfoPtr pInfo)
{
    int         i, nbuttons     = 1;
    char       *mapping         = NULL;
    EvdevPtr    pEvdev          = pInfo->private;

    /* Check for user-defined button mapping */
    if ((mapping = xf86CheckStrOption(pInfo->options, "ButtonMapping", NULL)))
    {
        char    *map, *s = " ";
        int     btn = 0;

        xf86IDrvMsg(pInfo, X_CONFIG, "ButtonMapping '%s'\n", mapping);
        map = mapping;
        while (s && *s != '\0' && nbuttons < EVDEV_MAXBUTTONS)
        {
            btn = strtol(map, &s, 10);

            if (s == map || btn < 0 || btn > EVDEV_MAXBUTTONS)
            {
                xf86IDrvMsg(pInfo, X_ERROR,
                            "... Invalid button mapping. Using defaults\n");
                nbuttons = 1; /* ensure defaults start at 1 */
                break;
            }

            pEvdev->btnmap[nbuttons++] = btn;
            map = s;
        }
        free(mapping);
    }

    for (i = nbuttons; i < ArrayLength(pEvdev->btnmap); i++)
        pEvdev->btnmap[i] = i;

}

static void
EvdevInitAnyValuators(DeviceIntPtr device, EvdevPtr pEvdev)
{
    InputInfoPtr pInfo = device->public.devicePrivate;

    if (pEvdev->flags & EVDEV_RELATIVE_EVENTS &&
        EvdevAddRelValuatorClass(device) == Success)
        xf86IDrvMsg(pInfo, X_INFO, "initialized for relative axes.\n");
    if (pEvdev->flags & EVDEV_ABSOLUTE_EVENTS &&
        EvdevAddAbsValuatorClass(device) == Success)
        xf86IDrvMsg(pInfo, X_INFO, "initialized for absolute axes.\n");
}

static void
EvdevInitAbsValuators(DeviceIntPtr device, EvdevPtr pEvdev)
{
    InputInfoPtr pInfo = device->public.devicePrivate;

    if (EvdevAddAbsValuatorClass(device) == Success) {
        xf86IDrvMsg(pInfo, X_INFO,"initialized for absolute axes.\n");
    } else {
        xf86IDrvMsg(pInfo, X_ERROR,"failed to initialize for absolute axes.\n");
        pEvdev->flags &= ~EVDEV_ABSOLUTE_EVENTS;
    }
}

static void
EvdevInitRelValuators(DeviceIntPtr device, EvdevPtr pEvdev)
{
    InputInfoPtr pInfo = device->public.devicePrivate;
    int has_abs_axes = pEvdev->flags & EVDEV_ABSOLUTE_EVENTS;
#ifdef _F_EVDEV_SUPPORT_GAMEPAD
    if ( (pEvdev->flags & EVDEV_GAMEPAD) && (has_abs_axes) ) {
        xf86IDrvMsg(pInfo, X_INFO,"initialized for game pad axes. Ignore relative axes.\n");

        pEvdev->flags &= ~EVDEV_RELATIVE_EVENTS;

        EvdevInitAbsValuators(device, pEvdev);
    } else if (EvdevAddRelValuatorClass(device) == Success) {
#else
    if (EvdevAddRelValuatorClass(device) == Success) {
#endif//_F_EVDEV_SUPPORT_GAMEPAD
        xf86IDrvMsg(pInfo, X_INFO,"initialized for relative axes.\n");

        if (has_abs_axes) {
            xf86IDrvMsg(pInfo, X_WARNING,"ignoring absolute axes.\n");
            pEvdev->flags &= ~EVDEV_ABSOLUTE_EVENTS;
        }

    } else {
        xf86IDrvMsg(pInfo, X_ERROR,"failed to initialize for relative axes.\n");

        pEvdev->flags &= ~EVDEV_RELATIVE_EVENTS;

        if (has_abs_axes)
            EvdevInitAbsValuators(device, pEvdev);
    }
}

static void
EvdevInitTouchDevice(DeviceIntPtr device, EvdevPtr pEvdev)
{
    InputInfoPtr pInfo = device->public.devicePrivate;

    if (pEvdev->flags & EVDEV_RELATIVE_EVENTS) {
        xf86IDrvMsg(pInfo, X_WARNING, "touchpads, tablets and touchscreens "
                    "ignore relative axes.\n");
        pEvdev->flags &= ~EVDEV_RELATIVE_EVENTS;
    }

    EvdevInitAbsValuators(device, pEvdev);
}

#ifdef _F_EVDEV_SUPPORT_GAMEPAD
static int
EvdevIsGamePad(InputInfoPtr pInfo)
{
    int i;
    EvdevPtr pEvdev = pInfo->private;
    int result = 1;

    for(i=0; i<MAX_GAMEPAD_DEFINITION_ABS; i++)
    {
        if(pEvdev->abs_gamepad_labels[i] == 0)
            break;

        if(!EvdevBitIsSet(pEvdev->abs_bitmask, pEvdev->abs_gamepad_labels[i]))
        {
            ErrorF("[EvdevIsGamePad] %s device doesn't support abs code(%d)\n", pInfo->name, pEvdev->abs_gamepad_labels[i]);
            result = 0;
            return result;
        }
    }

    for(i=0; i<MAX_GAMEPAD_DEFINITION_KEY; i++)
    {
        if(pEvdev->key_gamepad_labels[i] == 0)
            break;

        if(!EvdevBitIsSet(pEvdev->key_bitmask, pEvdev->key_gamepad_labels[i]))
        {
            ErrorF("[EvdevIsGamePad] %s device doesn't support key code(%d)\n", pInfo->name, pEvdev->key_gamepad_labels[i]);
            result = 0;
            return result;
        }
    }
    return result;
}
#endif//_F_EVDEV_SUPPORT_GAMEPAD

static int
EvdevInit(DeviceIntPtr device)
{
    int i;
    InputInfoPtr pInfo;
    EvdevPtr pEvdev;

    pInfo = device->public.devicePrivate;
    pEvdev = pInfo->private;

    /* clear all axis_map entries */
    for(i = 0; i < max(ABS_CNT,REL_CNT); i++)
      pEvdev->axis_map[i]=-1;

    if (pEvdev->flags & EVDEV_KEYBOARD_EVENTS)
	EvdevAddKeyClass(device);
    if (pEvdev->flags & EVDEV_BUTTON_EVENTS)
	EvdevAddButtonClass(device);

    /* We don't allow relative and absolute axes on the same device. The
     * reason is that some devices (MS Optical Desktop 2000) register both
     * rel and abs axes for x/y.
     *
     * The abs axes register min/max; this min/max then also applies to the
     * relative device (the mouse) and caps it at 0..255 for both axes.
     * So, unless you have a small screen, you won't be enjoying it much;
     * consequently, absolute axes are generally ignored.
     *
     * However, currenly only a device with absolute axes can be registered
     * as a touch{pad,screen}. Thus, given such a device, absolute axes are
     * used and relative axes are ignored.
     */

    if (pEvdev->flags & (EVDEV_UNIGNORE_RELATIVE | EVDEV_UNIGNORE_ABSOLUTE))
        EvdevInitAnyValuators(device, pEvdev);
    else if (pEvdev->flags & (EVDEV_TOUCHPAD | EVDEV_TOUCHSCREEN | EVDEV_TABLET))
        EvdevInitTouchDevice(device, pEvdev);
    else if (pEvdev->flags & EVDEV_RELATIVE_EVENTS)
        EvdevInitRelValuators(device, pEvdev);

#ifdef _F_INIT_ABS_ONLY_FOR_POINTER_
    else if ( !(pEvdev->flags & EVDEV_KEYBOARD_EVENTS) && (pEvdev->flags & EVDEV_ABSOLUTE_EVENTS) )
#else /* #ifdef _F_INIT_ABS_ONLY_FOR_POINTER_ */
    else if (pEvdev->flags & EVDEV_ABSOLUTE_EVENTS)
#endif /* #ifdef _F_INIT_ABS_ONLY_FOR_POINTER_ */
        EvdevInitAbsValuators(device, pEvdev);

    /* We drop the return value, the only time we ever want the handlers to
     * unregister is when the device dies. In which case we don't have to
     * unregister anyway */
    EvdevInitProperty(device);
    XIRegisterPropertyHandler(device, EvdevSetProperty, NULL, NULL);
    EvdevMBEmuInitProperty(device);
    Evdev3BEmuInitProperty(device);
    EvdevWheelEmuInitProperty(device);
    EvdevDragLockInitProperty(device);
    EvdevAppleInitProperty(device);
#ifdef _F_EVDEV_SUPPORT_ROTARY_
    EvdevRotaryInit(device);
#endif //_F_EVDEV_SUPPORT_ROTARY_

    return Success;
}

/**
 * Init all extras (wheel emulation, etc.) and grab the device.
 */
static int
EvdevOn(DeviceIntPtr device)
{
    InputInfoPtr pInfo;
    EvdevPtr pEvdev;
    int rc = Success;

    pInfo = device->public.devicePrivate;
    pEvdev = pInfo->private;
    /* after PreInit fd is still open */
    rc = EvdevOpenDevice(pInfo);
    if (rc != Success)
        return rc;

    EvdevGrabDevice(pInfo, 1, 0);

    xf86FlushInput(pInfo->fd);
    xf86AddEnabledDevice(pInfo);
    EvdevMBEmuOn(pInfo);
    Evdev3BEmuOn(pInfo);
    pEvdev->flags |= EVDEV_INITIALIZED;
    device->public.on = TRUE;

    return Success;
}


static int
EvdevProc(DeviceIntPtr device, int what)
{
    InputInfoPtr pInfo;
    EvdevPtr pEvdev;
#ifdef _F_EVDEV_CONFINE_REGION_
    int region[6] = { 0, };
#endif /* #ifdef _F_EVDEV_CONFINE_REGION_ */

    pInfo = device->public.devicePrivate;
    pEvdev = pInfo->private;

    switch (what)
    {
    case DEVICE_INIT:
	return EvdevInit(device);

    case DEVICE_ON:
        return EvdevOn(device);

    case DEVICE_OFF:
        if (pEvdev->flags & EVDEV_INITIALIZED)
        {
            EvdevMBEmuFinalize(pInfo);
            Evdev3BEmuFinalize(pInfo);
        }
        if (pInfo->fd != -1)
        {
            EvdevGrabDevice(pInfo, 0, 1);
            xf86RemoveEnabledDevice(pInfo);
            close(pInfo->fd);
            pInfo->fd = -1;
        }

#ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_
        if (pEvdev->rel_move_timer)
        {
            TimerCancel(pEvdev->rel_move_timer);
            pEvdev->rel_move_timer = NULL;
            pEvdev->rel_move_status = 0;
            pEvdev->rel_move_ack = 0;
            ErrorF("[%s][dev:%d] DEVICE_OFF (rel_move_status=%d, rel_move_prop_set=%d)\n", __FUNCTION__, pInfo->dev->id, pEvdev->rel_move_status,
                    pEvdev->rel_move_prop_set);
        }
#endif /* #ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_ */

        pEvdev->min_maj = 0;
        pEvdev->flags &= ~EVDEV_INITIALIZED;
	device->public.on = FALSE;
	break;

    case DEVICE_CLOSE:
#ifdef _F_EVDEV_CONFINE_REGION_
	if(pEvdev->pointer_confine_region && pEvdev->confined_id)
		EvdevSetConfineRegion(pInfo, 1, &region[0]);
#endif /* _F_EVDEV_CONFINE_REGION_ */
	xf86IDrvMsg(pInfo, X_INFO, "Close\n");
        if (pInfo->fd != -1) {
            close(pInfo->fd);
            pInfo->fd = -1;
        }
        EvdevFreeMasks(pEvdev);
        EvdevRemoveDevice(pInfo);
#ifdef _F_REMAP_KEYS_
        freeRemap(pEvdev);
#endif //_F_REMAP_KEYS_
        pEvdev->min_maj = 0;
	break;

    default:
        return BadValue;
    }

    return Success;
}

/**
 * Get as much information as we can from the fd and cache it.
 *
 * @return Success if the information was cached, or !Success otherwise.
 */
static int
EvdevCache(InputInfoPtr pInfo)
{
    EvdevPtr pEvdev = pInfo->private;
    int i, len;
    struct input_id id;

    char name[1024]                  = {0};
    unsigned long bitmask[NLONGS(EV_CNT)]      = {0};
    unsigned long key_bitmask[NLONGS(KEY_CNT)] = {0};
    unsigned long rel_bitmask[NLONGS(REL_CNT)] = {0};
    unsigned long abs_bitmask[NLONGS(ABS_CNT)] = {0};
    unsigned long led_bitmask[NLONGS(LED_CNT)] = {0};


    if (ioctl(pInfo->fd, EVIOCGID, &id) < 0)
    {
        xf86IDrvMsg(pInfo, X_ERROR, "ioctl EVIOCGID failed: %s\n", strerror(errno));
        goto error;
    }

    pEvdev->id_vendor = id.vendor;
    pEvdev->id_product = id.product;

    if (ioctl(pInfo->fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
        xf86IDrvMsg(pInfo, X_ERROR, "ioctl EVIOCGNAME failed: %s\n", strerror(errno));
        goto error;
    }

    strcpy(pEvdev->name, name);

    len = ioctl(pInfo->fd, EVIOCGBIT(0, sizeof(bitmask)), bitmask);
    if (len < 0) {
        xf86IDrvMsg(pInfo, X_ERROR, "ioctl EVIOCGBIT failed: %s\n",
                    strerror(errno));
        goto error;
    }

    memcpy(pEvdev->bitmask, bitmask, len);

    len = ioctl(pInfo->fd, EVIOCGBIT(EV_REL, sizeof(rel_bitmask)), rel_bitmask);
    if (len < 0) {
        xf86IDrvMsg(pInfo, X_ERROR, "ioctl EVIOCGBIT failed: %s\n",
                    strerror(errno));
        goto error;
    }

    memcpy(pEvdev->rel_bitmask, rel_bitmask, len);

    len = ioctl(pInfo->fd, EVIOCGBIT(EV_ABS, sizeof(abs_bitmask)), abs_bitmask);
    if (len < 0) {
        xf86IDrvMsg(pInfo, X_ERROR, "ioctl EVIOCGBIT failed: %s\n",
                    strerror(errno));
        goto error;
    }

    memcpy(pEvdev->abs_bitmask, abs_bitmask, len);

    len = ioctl(pInfo->fd, EVIOCGBIT(EV_LED, sizeof(led_bitmask)), led_bitmask);
    if (len < 0) {
        xf86IDrvMsg(pInfo, X_ERROR, "ioctl EVIOCGBIT failed: %s\n",
                    strerror(errno));
        goto error;
    }

    memcpy(pEvdev->led_bitmask, led_bitmask, len);

    /*
     * Do not try to validate absinfo data since it is not expected
     * to be static, always refresh it in evdev structure.
     */
    for (i = ABS_X; i <= ABS_MAX; i++) {
        if (EvdevBitIsSet(abs_bitmask, i)) {
            len = ioctl(pInfo->fd, EVIOCGABS(i), &pEvdev->absinfo[i]);
            if (len < 0) {
                xf86IDrvMsg(pInfo, X_ERROR, "ioctl EVIOCGABSi(%d) failed: %s\n",
                            i, strerror(errno));
                goto error;
            }
            xf86IDrvMsgVerb(pInfo, X_PROBED, 6, "absolute axis %#x [%d..%d]\n",
                            i, pEvdev->absinfo[i].maximum, pEvdev->absinfo[i].minimum);
        }
    }

    len = ioctl(pInfo->fd, EVIOCGBIT(EV_KEY, sizeof(key_bitmask)), key_bitmask);
    if (len < 0) {
        xf86IDrvMsg(pInfo, X_ERROR, "ioctl EVIOCGBIT failed: %s\n",
                    strerror(errno));
        goto error;
    }

    /* Copy the data so we have reasonably up-to-date info */
    memcpy(pEvdev->key_bitmask, key_bitmask, len);

    return Success;

error:
    return !Success;

}

/**
 * Issue an EVIOCGRAB on the device file, either as a grab or to ungrab, or
 * both. Return TRUE on success, otherwise FALSE. Failing the release is a
 * still considered a success, because it's not as if you could do anything
 * about it.
 */
static BOOL
EvdevGrabDevice(InputInfoPtr pInfo, int grab, int ungrab)
{
    EvdevPtr pEvdev = pInfo->private;

    if (pEvdev->grabDevice)
    {
        if (grab && ioctl(pInfo->fd, EVIOCGRAB, (void *)1)) {
            xf86IDrvMsg(pInfo, X_WARNING, "Grab failed (%s)\n",
                        strerror(errno));
            return FALSE;
        } else if (ungrab && ioctl(pInfo->fd, EVIOCGRAB, (void *)0))
            xf86IDrvMsg(pInfo, X_WARNING, "Release failed (%s)\n",
                        strerror(errno));
    }

    return TRUE;
}

/**
 * Some devices only have other axes (e.g. wheels), but we
 * still need x/y for these. The server relies on devices having
 * x/y as axes 0/1 and core/XI 1.x clients expect it too (#44655)
 */
static void
EvdevForceXY(InputInfoPtr pInfo, int mode)
{
    EvdevPtr pEvdev = pInfo->private;

    xf86IDrvMsg(pInfo, X_INFO, "Forcing %s x/y axes to exist.\n",
                (mode == Relative) ? "relative" : "absolute");

    if (mode == Relative)
    {
        EvdevSetBit(pEvdev->rel_bitmask, REL_X);
        EvdevSetBit(pEvdev->rel_bitmask, REL_Y);
    } else if (mode == Absolute)
    {
        EvdevSetBit(pEvdev->abs_bitmask, ABS_X);
        EvdevSetBit(pEvdev->abs_bitmask, ABS_Y);
        pEvdev->absinfo[ABS_X].minimum = 0;
        pEvdev->absinfo[ABS_X].maximum = 1000;
        pEvdev->absinfo[ABS_X].value = 0;
        pEvdev->absinfo[ABS_X].resolution = 0;
        pEvdev->absinfo[ABS_Y].minimum = 0;
        pEvdev->absinfo[ABS_Y].maximum = 1000;
        pEvdev->absinfo[ABS_Y].value = 0;
        pEvdev->absinfo[ABS_Y].resolution = 0;
    }
}

static int
EvdevProbe(InputInfoPtr pInfo)
{
    int i, has_rel_axes, has_abs_axes, has_keys, num_buttons, has_scroll;
    int has_lmr; /* left middle right */
    int has_mt; /* multitouch */
    int ignore_abs = 0, ignore_rel = 0;
    EvdevPtr pEvdev = pInfo->private;
    int rc = 1;

    xf86IDrvMsg(pInfo, X_PROBED, "Vendor %#hx Product %#hx\n",
                pEvdev->id_vendor, pEvdev->id_product);

    /* Trinary state for ignoring axes:
       - unset: do the normal thing.
       - TRUE: explicitly ignore them.
       - FALSE: unignore axes, use them at all cost if they're present.
     */
    if (xf86FindOption(pInfo->options, "IgnoreRelativeAxes"))
    {
        if (xf86SetBoolOption(pInfo->options, "IgnoreRelativeAxes", FALSE))
            ignore_rel = TRUE;
        else
            pEvdev->flags |= EVDEV_UNIGNORE_RELATIVE;

    }
    if (xf86FindOption(pInfo->options, "IgnoreAbsoluteAxes"))
    {
        if (xf86SetBoolOption(pInfo->options, "IgnoreAbsoluteAxes", FALSE))
           ignore_abs = TRUE;
        else
            pEvdev->flags |= EVDEV_UNIGNORE_ABSOLUTE;
    }

    has_rel_axes = FALSE;
    has_abs_axes = FALSE;
    has_keys = FALSE;
    has_scroll = FALSE;
    has_lmr = FALSE;
    has_mt = FALSE;
    num_buttons = 0;

    /* count all buttons */
#ifdef _F_EVDEV_SUPPORT_GAMEPAD
    for (i = BTN_MISC; i < BTN_THUMBR; i++)
#else
    for (i = BTN_MISC; i < BTN_JOYSTICK; i++)
#endif//_F_EVDEV_SUPPORT_GAMEPAD
    {
        int mapping = 0;
        if (EvdevBitIsSet(pEvdev->key_bitmask, i))
        {
            mapping = EvdevUtilButtonEventToButtonNumber(pEvdev, i);
            if (mapping > num_buttons)
                num_buttons = mapping;
        }
    }

    has_lmr = EvdevBitIsSet(pEvdev->key_bitmask, BTN_LEFT) ||
                EvdevBitIsSet(pEvdev->key_bitmask, BTN_MIDDLE) ||
                EvdevBitIsSet(pEvdev->key_bitmask, BTN_RIGHT);

    if (num_buttons)
    {
        pEvdev->flags |= EVDEV_BUTTON_EVENTS;
        pEvdev->num_buttons = num_buttons;
        xf86IDrvMsg(pInfo, X_PROBED, "Found %d mouse buttons\n", num_buttons);
    }

    for (i = 0; i < REL_MAX; i++) {
        if (EvdevBitIsSet(pEvdev->rel_bitmask, i)) {
            has_rel_axes = TRUE;
            break;
        }
    }

    if (has_rel_axes) {
        if (EvdevBitIsSet(pEvdev->rel_bitmask, REL_WHEEL) ||
            EvdevBitIsSet(pEvdev->rel_bitmask, REL_HWHEEL) ||
            EvdevBitIsSet(pEvdev->rel_bitmask, REL_DIAL)) {
            xf86IDrvMsg(pInfo, X_PROBED, "Found scroll wheel(s)\n");
            has_scroll = TRUE;
            if (!num_buttons)
                xf86IDrvMsg(pInfo, X_INFO,
                            "Forcing buttons for scroll wheel(s)\n");
            num_buttons = (num_buttons < 3) ? 7 : num_buttons + 4;
            pEvdev->num_buttons = num_buttons;
        }

        if (!ignore_rel)
        {
            xf86IDrvMsg(pInfo, X_PROBED, "Found relative axes\n");
            pEvdev->flags |= EVDEV_RELATIVE_EVENTS;

            if (EvdevBitIsSet(pEvdev->rel_bitmask, REL_X) &&
                EvdevBitIsSet(pEvdev->rel_bitmask, REL_Y)) {
                xf86IDrvMsg(pInfo, X_PROBED, "Found x and y relative axes\n");
            } else if (!EvdevBitIsSet(pEvdev->abs_bitmask, ABS_X) ||
                       !EvdevBitIsSet(pEvdev->abs_bitmask, ABS_Y))
                EvdevForceXY(pInfo, Relative);
        } else {
            xf86IDrvMsg(pInfo, X_INFO, "Relative axes present but ignored.\n");
            has_rel_axes = FALSE;
        }
    }

    for (i = 0; i < ABS_MAX; i++) {
        if (EvdevBitIsSet(pEvdev->abs_bitmask, i)) {
            has_abs_axes = TRUE;
            break;
        }
    }

#ifdef MULTITOUCH
    for (i = ABS_MT_SLOT; i < ABS_MAX; i++) {
        if (EvdevBitIsSet(pEvdev->abs_bitmask, i)) {
            has_mt = TRUE;
            break;
        }
    }
#endif

    if (ignore_abs && has_abs_axes)
    {
        xf86IDrvMsg(pInfo, X_INFO, "Absolute axes present but ignored.\n");
        has_abs_axes = FALSE;
    } else if (has_abs_axes) {
        xf86IDrvMsg(pInfo, X_PROBED, "Found absolute axes\n");
        pEvdev->flags |= EVDEV_ABSOLUTE_EVENTS;

        if (has_mt)
            xf86IDrvMsg(pInfo, X_PROBED, "Found absolute multitouch axes\n");

        if ((EvdevBitIsSet(pEvdev->abs_bitmask, ABS_X) &&
             EvdevBitIsSet(pEvdev->abs_bitmask, ABS_Y))) {
            xf86IDrvMsg(pInfo, X_PROBED, "Found x and y absolute axes\n");
            if (EvdevBitIsSet(pEvdev->key_bitmask, BTN_TOOL_PEN) ||
                EvdevBitIsSet(pEvdev->key_bitmask, BTN_STYLUS) ||
                EvdevBitIsSet(pEvdev->key_bitmask, BTN_STYLUS2))
            {
                xf86IDrvMsg(pInfo, X_PROBED, "Found absolute tablet.\n");
                pEvdev->flags |= EVDEV_TABLET;
                if (!pEvdev->num_buttons)
                {
                    pEvdev->num_buttons = 7; /* LMR + scroll wheels */
                    pEvdev->flags |= EVDEV_BUTTON_EVENTS;
                }
            } else if (EvdevBitIsSet(pEvdev->abs_bitmask, ABS_PRESSURE) ||
                EvdevBitIsSet(pEvdev->key_bitmask, BTN_TOUCH)) {
                if (has_lmr || EvdevBitIsSet(pEvdev->key_bitmask, BTN_TOOL_FINGER)) {
                    xf86IDrvMsg(pInfo, X_PROBED, "Found absolute touchpad.\n");
                    pEvdev->flags |= EVDEV_TOUCHPAD;
                } else {
                    xf86IDrvMsg(pInfo, X_PROBED, "Found absolute touchscreen\n");
                    pEvdev->flags |= EVDEV_TOUCHSCREEN;
                    pEvdev->flags |= EVDEV_BUTTON_EVENTS;
                }
            } else if (!(EvdevBitIsSet(pEvdev->rel_bitmask, REL_X) &&
                         EvdevBitIsSet(pEvdev->rel_bitmask, REL_Y)) && has_lmr) {
                    /* some touchscreens use BTN_LEFT rather than BTN_TOUCH */
                    xf86IDrvMsg(pInfo, X_PROBED, "Found absolute touchscreen\n");
                    pEvdev->flags |= EVDEV_TOUCHSCREEN;
                    pEvdev->flags |= EVDEV_BUTTON_EVENTS;
#ifdef _F_EVDEV_SUPPORT_GAMEPAD
            }  else if(EvdevIsGamePad(pInfo)) {
                    xf86IDrvMsg(pInfo, X_PROBED, "Found gamepad\n");
                    pEvdev->flags |= EVDEV_GAMEPAD;
#endif // _F_EVDEV_SUPPORT_GAMEPAD
            }
        } else {
#ifdef MULTITOUCH
            if (!EvdevBitIsSet(pEvdev->abs_bitmask, ABS_MT_POSITION_X) ||
                !EvdevBitIsSet(pEvdev->abs_bitmask, ABS_MT_POSITION_Y))
#endif
                EvdevForceXY(pInfo, Absolute);
        }
    }

    for (i = 0; i < BTN_MISC; i++) {
        if (EvdevBitIsSet(pEvdev->key_bitmask, i)) {
            xf86IDrvMsg(pInfo, X_PROBED, "Found keys\n");
            pEvdev->flags |= EVDEV_KEYBOARD_EVENTS;
            has_keys = TRUE;
            break;
        }
    }

    if (has_rel_axes || has_abs_axes)
    {
        char *str;
        int num_calibration = 0, calibration[4] = { 0, 0, 0, 0 };

        pEvdev->invert_x = xf86SetBoolOption(pInfo->options, "InvertX", FALSE);
        pEvdev->invert_y = xf86SetBoolOption(pInfo->options, "InvertY", FALSE);
        pEvdev->swap_axes = xf86SetBoolOption(pInfo->options, "SwapAxes", FALSE);

        str = xf86CheckStrOption(pInfo->options, "Calibration", NULL);
        if (str) {
            num_calibration = sscanf(str, "%d %d %d %d",
                    &calibration[0], &calibration[1],
                    &calibration[2], &calibration[3]);
            free(str);
            if (num_calibration == 4)
                EvdevSetCalibration(pInfo, num_calibration, calibration);
            else
                xf86IDrvMsg(pInfo, X_ERROR,
                            "Insufficient calibration factors (%d). Ignoring calibration\n",
                            num_calibration);
        }
    }

    if (has_rel_axes || has_abs_axes || num_buttons) {
        pInfo->flags |= XI86_SEND_DRAG_EVENTS;
	if (pEvdev->flags & EVDEV_TOUCHPAD) {
	    xf86IDrvMsg(pInfo, X_INFO, "Configuring as touchpad\n");
	    pInfo->type_name = XI_TOUCHPAD;
	    pEvdev->use_proximity = 0;
	} else if (pEvdev->flags & EVDEV_TABLET) {
	    xf86IDrvMsg(pInfo, X_INFO, "Configuring as tablet\n");
	    pInfo->type_name = XI_TABLET;
        } else if (pEvdev->flags & EVDEV_TOUCHSCREEN) {
            xf86IDrvMsg(pInfo, X_INFO, "Configuring as touchscreen\n");
            pInfo->type_name = XI_TOUCHSCREEN;
	} else {
	    xf86IDrvMsg(pInfo, X_INFO, "Configuring as mouse\n");
	    pInfo->type_name = XI_MOUSE;
	}

        rc = 0;
    }

    if (has_keys) {
        xf86IDrvMsg(pInfo, X_INFO, "Configuring as keyboard\n");
        pInfo->type_name = XI_KEYBOARD;
        rc = 0;
    }

    if (has_scroll &&
        (has_rel_axes || has_abs_axes || num_buttons || has_keys))
    {
        xf86IDrvMsg(pInfo, X_INFO, "Adding scrollwheel support\n");
        pEvdev->flags |= EVDEV_BUTTON_EVENTS;
        pEvdev->flags |= EVDEV_RELATIVE_EVENTS;
    }

#ifdef _F_EVDEV_SUPPORT_ROTARY_
    if ((!strncmp(pInfo->name, "tizen_rotary", sizeof("tizen_rotary"))) && (pEvdev->flags & EVDEV_RELATIVE_EVENTS)) {
        xf86IDrvMsg(pInfo, X_PROBED, "Found rotary device.\n");
        pEvdev->flags |= EVDEV_OFM;

        EvdevSetBit(pEvdev->rel_bitmask, REL_Z);
    }

    if ((!strncmp(pInfo->name, "tizen_detent", sizeof("tizen_detent"))) && (pEvdev->flags & EVDEV_RELATIVE_EVENTS)) {
        xf86IDrvMsg(pInfo, X_PROBED, "Found hall device.\n");
        pEvdev->flags |= EVDEV_HALLIC;
    }
#endif //_F_EVDEV_SUPPORT_ROTARY_

    if (rc)
        xf86IDrvMsg(pInfo, X_WARNING, "Don't know how to use device\n");

    return rc;
}

static void
EvdevSetCalibration(InputInfoPtr pInfo, int num_calibration, int calibration[4])
{
    EvdevPtr pEvdev = pInfo->private;

    if (num_calibration == 0) {
        pEvdev->flags &= ~EVDEV_CALIBRATED;
        pEvdev->calibration.min_x = 0;
        pEvdev->calibration.max_x = 0;
        pEvdev->calibration.min_y = 0;
        pEvdev->calibration.max_y = 0;
    } else if (num_calibration == 4) {
        pEvdev->flags |= EVDEV_CALIBRATED;
        pEvdev->calibration.min_x = calibration[0];
        pEvdev->calibration.max_x = calibration[1];
        pEvdev->calibration.min_y = calibration[2];
        pEvdev->calibration.max_y = calibration[3];
    }
}

static int
EvdevOpenDevice(InputInfoPtr pInfo)
{
    EvdevPtr pEvdev = pInfo->private;
    char *device = pEvdev->device;

    if (!device)
    {
        device = xf86CheckStrOption(pInfo->options, "Device", NULL);
        if (!device) {
            xf86IDrvMsg(pInfo, X_ERROR, "No device specified.\n");
            return BadValue;
        }

        pEvdev->device = device;
        xf86IDrvMsg(pInfo, X_CONFIG, "Device: \"%s\"\n", device);
    }

    if (pInfo->fd < 0)
    {
        do {
            pInfo->fd = open(device, O_RDWR | O_NONBLOCK, 0);
        } while (pInfo->fd < 0 && errno == EINTR);

        if (pInfo->fd < 0) {
            xf86IDrvMsg(pInfo, X_ERROR, "Unable to open evdev device \"%s\".\n", device);
            return BadValue;
        }
    }


    /* Check major/minor of device node to avoid adding duplicate devices. */
    pEvdev->min_maj = EvdevGetMajorMinor(pInfo);
    if (EvdevIsDuplicate(pInfo))
    {
        xf86IDrvMsg(pInfo, X_WARNING, "device file is duplicate. Ignoring.\n");
        close(pInfo->fd);
#ifdef MULTITOUCH
        mtdev_close_delete(pEvdev->mtdev);
        pEvdev->mtdev = NULL;
#endif
        return BadMatch;
    }

    return Success;
}

#ifdef _F_EVDEV_CONFINE_REGION_
DeviceIntPtr
GetMasterPointerFromId(int deviceid)
{
	DeviceIntPtr pDev = inputInfo.devices;
	while(pDev)
	{
		if( pDev->id == deviceid && pDev->master )
		{
			return pDev->master;
		}
		pDev = pDev->next;
	}

	return NULL;
}

Bool
IsMaster(DeviceIntPtr dev)
{
    return dev->type == MASTER_POINTER || dev->type == MASTER_KEYBOARD;
}

DeviceIntPtr
GetPairedDevice(DeviceIntPtr dev)
{
    if (!IsMaster(dev) && dev->master)
        dev = dev->master;

    return dev->spriteInfo->paired;
}

DeviceIntPtr
GetMaster(DeviceIntPtr dev, int which)
{
    DeviceIntPtr master;

    if (IsMaster(dev))
        master = dev;
    else
        master = dev->master;

    if (master)
    {
        if (which == MASTER_KEYBOARD)
        {
            if (master->type != MASTER_KEYBOARD)
                master = GetPairedDevice(master);
        } else
        {
            if (master->type != MASTER_POINTER)
                master = GetPairedDevice(master);
        }
    }

    return master;
}

static void
EvdevHookPointerCursorLimits(DeviceIntPtr pDev, ScreenPtr pScreen, CursorPtr pCursor,
                      BoxPtr pHotBox, BoxPtr pTopLeftBox)
{
    *pTopLeftBox = *pHotBox;
}

static void
EvdevHookPointerConstrainCursor (DeviceIntPtr pDev, ScreenPtr pScreen, BoxPtr pBox)
{
    InputInfoPtr pInfo;
    EvdevPtr pEvdev;
    BoxPtr pConfineBox;

    pInfo =  pDev->public.devicePrivate;
    if(!pInfo || !pInfo->private) return;
    pEvdev = pInfo->private;

    xf86IDrvMsg(pInfo, X_INFO, "[X11][EvdevHookPointerConstrainCursor] Enter !\n");

    miPointerPtr pPointer;
    pPointer = MIPOINTER(pDev);

    pConfineBox = pEvdev->pointer_confine_region;
    if(pConfineBox && pEvdev->confined_id)
    {
	    xf86IDrvMsg(pInfo, X_INFO, "[X11][EvdevHookPointerConstrainCursor][before] pBox (%d, %d, %d, %d), pPointer->limits (%d, %d, %d, %d), confined=%d\n",
			pBox->x1, pBox->y1, pBox->x2, pBox->y2, pPointer->limits.x1, pPointer->limits.y1, pPointer->limits.x2, pPointer->limits.y2, pPointer->confined);

	    if( IsMaster(pDev) && GetMasterPointerFromId(pEvdev->confined_id) == pDev )
	    {
		  xf86IDrvMsg(pInfo, X_INFO, "[X11][EvdevHookPointerConstrainCursor] confine routine --begin (pDev->id=%d)\n", pDev->id);
		  if( pBox->x1 < pConfineBox->x1 )
		  	pBox->x1 = pConfineBox->x1;
		  if( pBox->y1 < pConfineBox->y1 )
		  	pBox->y1 = pConfineBox->y1;
		  if( pBox->x2 > pConfineBox->x2 )
		  	pBox->x2 = pConfineBox->x2;
		  if( pBox->y2 > pConfineBox->y2 )
		  	pBox->y2 = pConfineBox->y2;
		  xf86IDrvMsg(pInfo, X_INFO, "[X11][EvdevHookPointerConstrainCursor] confine routine --end (pDev->id=%d)\n", pDev->id);
	    }
    }

    pPointer->limits = *pBox;
    pPointer->confined = PointerConfinedToScreen(pDev);

    if(pEvdev->confined_id)
    {
	    xf86IDrvMsg(pInfo, X_INFO, "[X11][EvdevHookPointerConstrainCursor][after] pBox (%d, %d, %d, %d), pPointer->limits (%d, %d, %d, %d), confined=%d\n",
			pBox->x1, pBox->y1, pBox->x2, pBox->y2, pPointer->limits.x1, pPointer->limits.y1, pPointer->limits.x2, pPointer->limits.y2, pPointer->confined);
    }

    xf86IDrvMsg(pInfo, X_INFO, "[X11][EvdevHookPointerConstrainCursor] Leave !\n");
}

static void
EvdevSetCursorLimits(InputInfoPtr pInfo, int region[6], int isSet)
{
	EvdevPtr pEvdev = pInfo->private;
	BoxPtr pConfineBox = pEvdev->pointer_confine_region;
	int v[2];
	int x, y;

	ScreenPtr pCursorScreen = NULL;

	pCursorScreen = miPointerGetScreen(pInfo->dev);

	if( !pCursorScreen )
	{
		xf86IDrvMsg(pInfo, X_ERROR, "[X11][SetCursorLimits] Failed to get screen information for pointer !\n");
		return;
	}

	if( isSet )
	{
		xf86IDrvMsg(pInfo, X_INFO, "[X11][SetCursorLimits] isSet = TRUE !!\n");

		//Clip confine region with screen's width/height
		if( region[0] < 0 )
			region[0] = 0;
		if( region[2] >= pCursorScreen->width )
			region[2] = pCursorScreen->width - 1;
		if( region [1] < 0 )
			region[1] = 0;
		if( region[3] >= pCursorScreen->height )
			region[3] = pCursorScreen->height - 1;

		//Do pointerwarp if region[4] is not zero and region[5] is zero
		if(region[4] && !region[5])
			goto warp_only;

		pConfineBox->x1 = region[0];
		pConfineBox->y1 = region[1];
		pConfineBox->x2 = region[2];
		pConfineBox->y2 = region[3];
		pEvdev->confined_id = pInfo->dev->id;

		pCursorScreen->ConstrainCursor(inputInfo.pointer, pCursorScreen, pConfineBox);
		xf86IDrvMsg(pInfo, X_INFO, "[X11][SetCursorLimits] Constrain information for cursor was set to TOPLEFT(%d, %d) BOTTOMRIGHT(%d, %d) !\n",
			region[0], region[1], region[2], region[3]);

		if( pCursorScreen->CursorLimits != EvdevHookPointerCursorLimits &&
			pCursorScreen->ConstrainCursor != EvdevHookPointerConstrainCursor)
		{
			//Backup original function pointer(s)
			pEvdev->pOrgConstrainCursor = pCursorScreen->ConstrainCursor;
			pEvdev->pOrgCursorLimits = pCursorScreen->CursorLimits;

			//Overriding function pointer(s)
			pCursorScreen->CursorLimits = EvdevHookPointerCursorLimits;
			pCursorScreen->ConstrainCursor = EvdevHookPointerConstrainCursor;
		}

		//Skip pointer warp if region[4] is zero
		if(!region[4])
		{
			xf86IDrvMsg(pInfo, X_INFO, "[X11][SetCursorLimits] region[4]=%d NOT pointer warp !\n", region[4]);
			return;
		}

warp_only:
		v[0] = region[0] + (int)((float)(region[2]-region[0])/2);
		v[1] = region[1] + (int)((float)(region[3]-region[1])/2);

		xf86PostMotionEventP(pInfo->dev, TRUE, REL_X, 2, v);
		xf86IDrvMsg(pInfo, X_INFO, "[X11][SetCursorLimits] Cursor was warped to (%d, %d) !\n", v[0], v[1]);
		miPointerGetPosition(pInfo->dev, &x, &y);
		xf86IDrvMsg(pInfo, X_INFO, "[X11][SetCursorLimits] Cursor is located at (%d, %d) !\n", x, y);
	}
	else
	{
		xf86IDrvMsg(pInfo, X_INFO, "[X11][SetCursorLimits] isSet = FALSE !!\n");

		pConfineBox->x1 = 0;
		pConfineBox->y1 = 0;
		pConfineBox->x2 = pCursorScreen->width - 1;
		pConfineBox->y2 = pCursorScreen->height - 1;
		pEvdev->confined_id = 0;

		pCursorScreen->ConstrainCursor(inputInfo.pointer, pCursorScreen, pConfineBox);
		xf86IDrvMsg(pInfo, X_INFO, "[X11][SetCursorLimits] Constrain information for cursor was restored ! TOPLEFT(%d, %d) BOTTOMRIGHT(%d, %d) !\n",
			pConfineBox->x1, pConfineBox->y1,
			pConfineBox->x2, pConfineBox->y2);

		//Restore original function pointer(s)
		pCursorScreen->CursorLimits = pEvdev->pOrgCursorLimits;
		pCursorScreen->ConstrainCursor = pEvdev->pOrgConstrainCursor;
	}
}

static void
EvdevSetConfineRegion(InputInfoPtr pInfo, int num_item, int region[6])
{
	EvdevPtr pEvdev = pInfo ? pInfo->private : NULL;

	if(!pEvdev)
	{
		xf86IDrvMsg(pInfo, X_ERROR, "[X11][SetConfineRegion] num_item != 6 && num_item != 1 !!\n");
		return;
	}

	if ((num_item != 6) && (num_item != 1) )
	{
		xf86IDrvMsg(pInfo, X_ERROR, "[X11][SetConfineRegion] num_item != 6 && num_item != 1 !!\n");
		return;
	}

	if (!pEvdev->pointer_confine_region)
	{
		xf86IDrvMsg(pInfo, X_ERROR, "[X11][SetConfineRegion] pEvdev->pointer_confine_region is NULL\n");
		xf86IDrvMsg(pInfo, X_ERROR, "[X11][SetConfineRegion] num_item=%d\n", num_item);
		return;
	}

	if( num_item == 6 )
	{
		xf86IDrvMsg(pInfo, X_INFO, "[X11][SetConfineRegion] num_item == 6\n");
		if ( (region[2]-region[0]>0) && (region[3]-region[1]>0) )
	    	{
			EvdevSetCursorLimits(pInfo, &region[0], 1);
			xf86IDrvMsg(pInfo, X_INFO, "[X11][SetConfineRegion] Confine region was set to TOPLEFT(%d, %d) BOTTOMRIGHT(%d, %d) pointerwarp=%d, confine=%d\n",
				region[0], region[1], region[2], region[3], region[4], region[5]);
			pEvdev->flags |= EVDEV_CONFINE_REGION;
	    	}
	}
	else//if( num_item == 1 )
	{
		xf86IDrvMsg(pInfo, X_INFO, "[X11][SetConfineRegion] num_item == 1\n");
		if( !region[0] && (pEvdev->flags & EVDEV_CONFINE_REGION) )
		{
			EvdevSetCursorLimits(pInfo, &region[0], 0);
			xf86IDrvMsg(pInfo, X_INFO, "[X11][SetConfineRegion] Confine region was unset !\n");
			pEvdev->flags &= ~EVDEV_CONFINE_REGION;
		}
	}
}
#endif /* #ifdef _F_EVDEV_CONFINE_REGION_ */

#ifdef _F_TOUCH_TRANSFORM_MATRIX_
static void
EvdevSetTransformMatrix(InputInfoPtr pInfo, int num_transform, float *tmatrix)
{
	int x, y;
	struct pixman_transform tr;
	EvdevPtr pEvdev = pInfo ? pInfo->private : NULL;

	if(!pEvdev)
		return;

	if( (num_transform != 9) && (num_transform != 1) )
	{
		pEvdev->use_transform = FALSE;
		return;
	}

	if( num_transform == 9 )
	{
		pEvdev->use_transform = TRUE;

		memcpy(pEvdev->transform, tmatrix, sizeof(pEvdev->transform));
		for (y=0; y<3; y++)
			for (x=0; x<3; x++)
				tr.matrix[y][x] = pixman_double_to_fixed((double)*tmatrix++);

		pixman_transform_invert(&pEvdev->inv_transform, &tr);
		ErrorF("[X11][EvdevSetTransformMatrix] Touch transform has been enabled !\n");
	}
	else if( (num_transform == 1) && (tmatrix[0] == 0) )
	{
		pEvdev->use_transform = FALSE;
		ErrorF("[X11][EvdevSetTransformMatrix] Touch transform has been disabled !\n");
	}
}
#endif /* #ifdef _F_TOUCH_TRANSFORM_MATRIX_ */

static void
EvdevUnInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
    EvdevPtr pEvdev = pInfo ? pInfo->private : NULL;
    if (pEvdev)
    {
#ifdef _F_EVDEV_SUPPORT_ROTARY_
        EvdevRotaryUnInit(pEvdev);
#endif //_F_EVDEV_SUPPORT_ROTARY_

        /* Release strings allocated in EvdevAddKeyClass. */
        XkbFreeRMLVOSet(&pEvdev->rmlvo, FALSE);
        /* Release string allocated in EvdevOpenDevice. */
        free(pEvdev->device);
        pEvdev->device = NULL;
    }
    xf86DeleteInput(pInfo, flags);
}

static int
EvdevPreInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
    EvdevPtr pEvdev;
    int rc = BadAlloc;
#ifdef _F_TOUCH_TRANSFORM_MATRIX_
    float tr[9];
    int num_transform = 0;
    const char *str;
#endif /* #ifdef _F_TOUCH_TRANSFORM_MATRIX_ */

    if (!(pEvdev = calloc(sizeof(EvdevRec), 1)))
    {
        xf86IDrvMsg(pInfo, X_ERROR, "Failed to allocate memory for private member of pInfo !\n");
        goto error;
    }

    pInfo->private = pEvdev;
    pInfo->type_name = "UNKNOWN";
    pInfo->device_control = EvdevProc;
    pInfo->read_input = EvdevReadInput;
    pInfo->switch_mode = EvdevSwitchMode;
#ifdef _F_EVDEV_SUPPORT_ROTARY_
    pEvdev->extra_rel_post_hallic= NULL;
    pEvdev->extra_rel_post_ofm= NULL;
#endif //_F_EVDEV_SUPPORT_ROTARY_

    rc = EvdevOpenDevice(pInfo);
    if (rc != Success)
        goto error;

#ifdef MULTITOUCH
    pEvdev->cur_slot = -1;
#endif

    /*
     * We initialize pEvdev->in_proximity to 1 so that device that doesn't use
     * proximity will still report events.
     */
    pEvdev->in_proximity = 1;
    pEvdev->use_proximity = 1;

    /* Grabbing the event device stops in-kernel event forwarding. In other
       words, it disables rfkill and the "Macintosh mouse button emulation".
       Note that this needs a server that sets the console to RAW mode. */
    pEvdev->grabDevice = xf86CheckBoolOption(pInfo->options, "GrabDevice", 0);

    /* If grabDevice is set, ungrab immediately since we only want to grab
     * between DEVICE_ON and DEVICE_OFF. If we never get DEVICE_ON, don't
     * hold a grab. */
    if (!EvdevGrabDevice(pInfo, 1, 1))
    {
        xf86IDrvMsg(pInfo, X_WARNING, "Device may already be configured.\n");
        rc = BadMatch;
        goto error;
    }

    EvdevInitButtonMapping(pInfo);

#ifdef _F_EVDEV_SUPPORT_GAMEPAD
    {
        int i;
        char tmp[25];

        memset(&pEvdev->abs_gamepad_labels, 0, sizeof(pEvdev->abs_gamepad_labels));

        for(i = 0 ; i < MAX_GAMEPAD_DEFINITION_ABS ; i++)
        {
            snprintf(tmp, sizeof(tmp), "GamePad_Condition_ABS%d", i+1);
            pEvdev->abs_gamepad_labels[i] = xf86SetIntOption(pInfo->options, tmp, 0);
        }
    }

    {
        int i;
        char tmp[25];

        memset(&pEvdev->key_gamepad_labels, 0, sizeof(pEvdev->key_gamepad_labels));

        for(i = 0 ; i < MAX_GAMEPAD_DEFINITION_KEY ; i++)
        {
            snprintf(tmp, sizeof(tmp), "GamePad_Condition_KEY%d", i+1);
            if(i == 0)
                pEvdev->key_gamepad_labels[i] = xf86SetIntOption(pInfo->options, tmp, BTN_GAMEPAD);
            else
                pEvdev->key_gamepad_labels[i] = xf86SetIntOption(pInfo->options, tmp, 0);
        }
    }
#endif//_F_EVDEV_SUPPORT_GAMEPAD

#ifdef _F_USE_DEFAULT_XKB_RULES_
    pEvdev->use_default_xkb_rmlvo = xf86SetBoolOption(pInfo->options, "UseDefaultXkbRMLVO", FALSE);
#endif

#ifdef _F_EVDEV_SUPPORT_ROTARY_
    pEvdev->HW_Calibration = xf86SetIntOption(pInfo->options, "HW_ROTARY_MAX", DEFAULT_HW_ROTARY_MAX);
#endif //_F_EVDEV_SUPPORT_ROTARY_

    if (EvdevCache(pInfo) || EvdevProbe(pInfo)) {
        rc = BadMatch;
        goto error;
    }

    EvdevAddDevice(pInfo);

    if (pEvdev->flags & EVDEV_BUTTON_EVENTS)
    {
        EvdevMBEmuPreInit(pInfo);
        Evdev3BEmuPreInit(pInfo);
        EvdevWheelEmuPreInit(pInfo);
        EvdevDragLockPreInit(pInfo);
    }

#ifdef MULTITOUCH
    if (strstr(pInfo->type_name, XI_TOUCHSCREEN))
    {
	pEvdev->mtdev = mtdev_new_open(pInfo->fd);

	if (pEvdev->mtdev)
	{
		pEvdev->cur_slot = pEvdev->mtdev->caps.slot.value;
	}
	else
	{
		xf86Msg(X_ERROR, "%s: Couldn't open mtdev device\n", pInfo->name);
		return FALSE;
	}
    }
    else
    {
	pEvdev->mtdev = NULL;
    }
#endif

#ifdef _F_TOUCH_TRANSFORM_MATRIX_
    pEvdev->use_transform = FALSE;

    if (strstr(pInfo->type_name, XI_TOUCHSCREEN))
    {
        str = xf86CheckStrOption(pInfo->options, "Transform", NULL);

        if (str)
        {
		num_transform = sscanf(str, "%f %f %f %f %f %f %f %f %f",
                                                &tr[0], &tr[1], &tr[2],
                                                &tr[3], &tr[4], &tr[5],
                                                &tr[6], &tr[7], &tr[8]);
		if (num_transform == 9)
	            EvdevSetTransformMatrix(pInfo, num_transform, tr);
		else
		{
	            xf86Msg(X_ERROR, "%s: Insufficient transform factors (%d). Ignoring transform\n",
	                pInfo->name, num_transform);
		}
        }
    }
#endif /* #ifdef _F_TOUCH_TRANSFORM_MATRIX_ */

#ifdef _F_EVDEV_CONFINE_REGION_
    pEvdev->confined_id = 0;
    pEvdev->pointer_confine_region = NULL;

    if (strstr(pInfo->name, XI_TOUCHPAD)
		|| strstr(pInfo->type_name, XI_MOUSE)
		|| strstr(pInfo->name, "Virtual Touchpad"))
    {
        pEvdev->pointer_confine_region = (BoxPtr)malloc(sizeof(BoxRec));

        if (!pEvdev->pointer_confine_region)
        {
		xf86IDrvMsg(pInfo, X_ERROR, "Failed to allocate memory for pointer confine region\nConfine feature may not work properly.");
		rc = BadAlloc;
		goto error;
        }
        else
        {
		xf86IDrvMsg(pInfo, X_INFO, "Succeed to allocate memory for pointer confine region\nConfine feature will work properly.");
        }

        memset(pEvdev->pointer_confine_region, 0, sizeof(BoxRec));
    }
#endif /* #ifdef _F_EVDEV_CONFINE_REGION_ */

#ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_
    pEvdev->rel_move_timer = NULL;
    pEvdev->rel_move_prop_set = 0;
    pEvdev->rel_move_status = 0;
#endif /* #ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_ */

#ifdef _F_GESTURE_EXTENSION_
    int alloc_size = sizeof(int)*num_slots(pEvdev);
    pEvdev->mt_status = NULL;

    if (strstr(pInfo->type_name, XI_TOUCHSCREEN))
    {
	    pEvdev->mt_status = (int *)malloc(alloc_size);

	    if (!pEvdev->mt_status)
	    {
	    	xf86IDrvMsg(pInfo, X_ERROR, "Failed to allocate memory to maintain multitouch status !\nX Gesture driver may not work properly.\n");
	    	rc = BadAlloc;
	    	goto error;
	    }

	    memset(pEvdev->mt_status, 0, alloc_size);
    }
#endif /* #ifdef _F_GESTURE_EXTENSION_ */
#ifdef _F_EVDEV_SUPPORT_GAMEPAD
    pEvdev->pre_x = 128;
    pEvdev->pre_y = 128;

    pEvdev->support_directional_key = xf86SetBoolOption(pInfo->options, "Support_Directional_Keys", FALSE);

    pEvdev->keycode_btnA = xf86SetIntOption(pInfo->options, "GamePad_ButtonA", 0);
    pEvdev->keycode_btnB = xf86SetIntOption(pInfo->options, "GamePad_ButtonB", 0);
    pEvdev->keycode_btnX = xf86SetIntOption(pInfo->options, "GamePad_ButtonX", 0);
    pEvdev->keycode_btnY = xf86SetIntOption(pInfo->options, "GamePad_ButtonY", 0);
    pEvdev->keycode_btnTL = xf86SetIntOption(pInfo->options, "GamePad_ButtonTL", 0);
    pEvdev->keycode_btnTR = xf86SetIntOption(pInfo->options, "GamePad_ButtonTR", 0);
    pEvdev->keycode_btnStart = xf86SetIntOption(pInfo->options, "GamePad_ButtonStart", 0);
    pEvdev->keycode_btnSelect = xf86SetIntOption(pInfo->options, "GamePad_ButtonSelect", 0);
    pEvdev->keycode_btnPlay = xf86SetIntOption(pInfo->options, "GamePad_ButtonPlay", 0);
#endif//_F_EVDEV_SUPPORT_GAMEPAD

    return Success;

error:
    if ((pInfo) && (pInfo->fd >= 0))
        close(pInfo->fd);
    return rc;
}

_X_EXPORT InputDriverRec EVDEV = {
    1,
    "evdev",
    NULL,
    EvdevPreInit,
    EvdevUnInit,
    NULL,
    evdevDefaults
};

static void
EvdevUnplug(pointer	p)
{
}

static pointer
EvdevPlug(pointer	module,
          pointer	options,
          int		*errmaj,
          int		*errmin)
{
    xf86AddInputDriver(&EVDEV, module, 0);
    return module;
}

static XF86ModuleVersionInfo EvdevVersionRec =
{
    "evdev",
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
    ABI_CLASS_XINPUT,
    ABI_XINPUT_VERSION,
    MOD_CLASS_XINPUT,
    {0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData evdevModuleData =
{
    &EvdevVersionRec,
    EvdevPlug,
    EvdevUnplug
};


/* Return an index value for a given button event code
 * returns 0 on non-button event.
 */
unsigned int
EvdevUtilButtonEventToButtonNumber(EvdevPtr pEvdev, int code)
{
    switch (code)
    {
        /* Mouse buttons */
        case BTN_LEFT:
            return 1;
        case BTN_MIDDLE:
            return 2;
        case BTN_RIGHT:
            return 3;
        case BTN_SIDE ... BTN_JOYSTICK - 1:
            return 8 + code - BTN_SIDE;

        /* Generic buttons */
        case BTN_0 ... BTN_2:
            return 1 + code - BTN_0;
        case BTN_3 ... BTN_MOUSE - 1:
            return 8 + code - BTN_3;

        /* Tablet stylus buttons */
        case BTN_TOUCH ... BTN_STYLUS2:
            return 1 + code - BTN_TOUCH;
#ifdef _F_EVDEV_SUPPORT_GAMEPAD
        /* Game pad buttons */
        case BTN_A ... BTN_THUMBR:
            return 8 + code - BTN_A;
#endif//_F_EVDEV_SUPPORT_GAMEPAD

        /* The rest */
        default:
            /* Ignore */
            return 0;
    }
}

/* Aligned with linux/input.h.
   Note that there are holes in the ABS_ range, these are simply replaced with
   MISC here */
static char* abs_labels[] = {
    AXIS_LABEL_PROP_ABS_X,              /* 0x00 */
    AXIS_LABEL_PROP_ABS_Y,              /* 0x01 */
    AXIS_LABEL_PROP_ABS_Z,              /* 0x02 */
    AXIS_LABEL_PROP_ABS_RX,             /* 0x03 */
    AXIS_LABEL_PROP_ABS_RY,             /* 0x04 */
    AXIS_LABEL_PROP_ABS_RZ,             /* 0x05 */
    AXIS_LABEL_PROP_ABS_THROTTLE,       /* 0x06 */
    AXIS_LABEL_PROP_ABS_RUDDER,         /* 0x07 */
    AXIS_LABEL_PROP_ABS_WHEEL,          /* 0x08 */
    AXIS_LABEL_PROP_ABS_GAS,            /* 0x09 */
    AXIS_LABEL_PROP_ABS_BRAKE,          /* 0x0a */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_HAT0X,          /* 0x10 */
    AXIS_LABEL_PROP_ABS_HAT0Y,          /* 0x11 */
    AXIS_LABEL_PROP_ABS_HAT1X,          /* 0x12 */
    AXIS_LABEL_PROP_ABS_HAT1Y,          /* 0x13 */
    AXIS_LABEL_PROP_ABS_HAT2X,          /* 0x14 */
    AXIS_LABEL_PROP_ABS_HAT2Y,          /* 0x15 */
    AXIS_LABEL_PROP_ABS_HAT3X,          /* 0x16 */
    AXIS_LABEL_PROP_ABS_HAT3Y,          /* 0x17 */
    AXIS_LABEL_PROP_ABS_PRESSURE,       /* 0x18 */
    AXIS_LABEL_PROP_ABS_DISTANCE,       /* 0x19 */
    AXIS_LABEL_PROP_ABS_TILT_X,         /* 0x1a */
    AXIS_LABEL_PROP_ABS_TILT_Y,         /* 0x1b */
    AXIS_LABEL_PROP_ABS_TOOL_WIDTH,     /* 0x1c */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_VOLUME          /* 0x20 */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MT_TOUCH_MAJOR, /* 0x30 */
    AXIS_LABEL_PROP_ABS_MT_TOUCH_MINOR, /* 0x31 */
    AXIS_LABEL_PROP_ABS_MT_WIDTH_MAJOR, /* 0x32 */
    AXIS_LABEL_PROP_ABS_MT_WIDTH_MINOR, /* 0x33 */
    AXIS_LABEL_PROP_ABS_MT_ORIENTATION, /* 0x34 */
    AXIS_LABEL_PROP_ABS_MT_POSITION_X,  /* 0x35 */
    AXIS_LABEL_PROP_ABS_MT_POSITION_Y,  /* 0x36 */
    AXIS_LABEL_PROP_ABS_MT_TOOL_TYPE,   /* 0x37 */
    AXIS_LABEL_PROP_ABS_MT_BLOB_ID,     /* 0x38 */
    AXIS_LABEL_PROP_ABS_MT_TRACKING_ID, /* 0x39 */
    AXIS_LABEL_PROP_ABS_MT_PRESSURE,    /* 0x3a */
};

static char* rel_labels[] = {
    AXIS_LABEL_PROP_REL_X,
    AXIS_LABEL_PROP_REL_Y,
    AXIS_LABEL_PROP_REL_Z,
    AXIS_LABEL_PROP_REL_RX,
    AXIS_LABEL_PROP_REL_RY,
    AXIS_LABEL_PROP_REL_RZ,
    AXIS_LABEL_PROP_REL_HWHEEL,
    AXIS_LABEL_PROP_REL_DIAL,
    AXIS_LABEL_PROP_REL_WHEEL,
    AXIS_LABEL_PROP_REL_MISC
};

static char* btn_labels[][16] = {
    { /* BTN_MISC group                 offset 0x100*/
        BTN_LABEL_PROP_BTN_0,           /* 0x00 */
        BTN_LABEL_PROP_BTN_1,           /* 0x01 */
        BTN_LABEL_PROP_BTN_2,           /* 0x02 */
        BTN_LABEL_PROP_BTN_3,           /* 0x03 */
        BTN_LABEL_PROP_BTN_4,           /* 0x04 */
        BTN_LABEL_PROP_BTN_5,           /* 0x05 */
        BTN_LABEL_PROP_BTN_6,           /* 0x06 */
        BTN_LABEL_PROP_BTN_7,           /* 0x07 */
        BTN_LABEL_PROP_BTN_8,           /* 0x08 */
        BTN_LABEL_PROP_BTN_9            /* 0x09 */
    },
    { /* BTN_MOUSE group                offset 0x110 */
        BTN_LABEL_PROP_BTN_LEFT,        /* 0x00 */
        BTN_LABEL_PROP_BTN_RIGHT,       /* 0x01 */
        BTN_LABEL_PROP_BTN_MIDDLE,      /* 0x02 */
        BTN_LABEL_PROP_BTN_SIDE,        /* 0x03 */
        BTN_LABEL_PROP_BTN_EXTRA,       /* 0x04 */
        BTN_LABEL_PROP_BTN_FORWARD,     /* 0x05 */
        BTN_LABEL_PROP_BTN_BACK,        /* 0x06 */
        BTN_LABEL_PROP_BTN_TASK         /* 0x07 */
    },
    { /* BTN_JOYSTICK group             offset 0x120 */
        BTN_LABEL_PROP_BTN_TRIGGER,     /* 0x00 */
        BTN_LABEL_PROP_BTN_THUMB,       /* 0x01 */
        BTN_LABEL_PROP_BTN_THUMB2,      /* 0x02 */
        BTN_LABEL_PROP_BTN_TOP,         /* 0x03 */
        BTN_LABEL_PROP_BTN_TOP2,        /* 0x04 */
        BTN_LABEL_PROP_BTN_PINKIE,      /* 0x05 */
        BTN_LABEL_PROP_BTN_BASE,        /* 0x06 */
        BTN_LABEL_PROP_BTN_BASE2,       /* 0x07 */
        BTN_LABEL_PROP_BTN_BASE3,       /* 0x08 */
        BTN_LABEL_PROP_BTN_BASE4,       /* 0x09 */
        BTN_LABEL_PROP_BTN_BASE5,       /* 0x0a */
        BTN_LABEL_PROP_BTN_BASE6,       /* 0x0b */
        NULL,
        NULL,
        NULL,
        BTN_LABEL_PROP_BTN_DEAD         /* 0x0f */
    },
    { /* BTN_GAMEPAD group              offset 0x130 */
        BTN_LABEL_PROP_BTN_A,           /* 0x00 */
        BTN_LABEL_PROP_BTN_B,           /* 0x01 */
        BTN_LABEL_PROP_BTN_C,           /* 0x02 */
        BTN_LABEL_PROP_BTN_X,           /* 0x03 */
        BTN_LABEL_PROP_BTN_Y,           /* 0x04 */
        BTN_LABEL_PROP_BTN_Z,           /* 0x05 */
        BTN_LABEL_PROP_BTN_TL,          /* 0x06 */
        BTN_LABEL_PROP_BTN_TR,          /* 0x07 */
        BTN_LABEL_PROP_BTN_TL2,         /* 0x08 */
        BTN_LABEL_PROP_BTN_TR2,         /* 0x09 */
        BTN_LABEL_PROP_BTN_SELECT,      /* 0x0a */
        BTN_LABEL_PROP_BTN_START,       /* 0x0b */
        BTN_LABEL_PROP_BTN_MODE,        /* 0x0c */
        BTN_LABEL_PROP_BTN_THUMBL,      /* 0x0d */
        BTN_LABEL_PROP_BTN_THUMBR       /* 0x0e */
    },
    { /* BTN_DIGI group                         offset 0x140 */
        BTN_LABEL_PROP_BTN_TOOL_PEN,            /* 0x00 */
        BTN_LABEL_PROP_BTN_TOOL_RUBBER,         /* 0x01 */
        BTN_LABEL_PROP_BTN_TOOL_BRUSH,          /* 0x02 */
        BTN_LABEL_PROP_BTN_TOOL_PENCIL,         /* 0x03 */
        BTN_LABEL_PROP_BTN_TOOL_AIRBRUSH,       /* 0x04 */
        BTN_LABEL_PROP_BTN_TOOL_FINGER,         /* 0x05 */
        BTN_LABEL_PROP_BTN_TOOL_MOUSE,          /* 0x06 */
        BTN_LABEL_PROP_BTN_TOOL_LENS,           /* 0x07 */
        NULL,
        NULL,
        BTN_LABEL_PROP_BTN_TOUCH,               /* 0x0a */
        BTN_LABEL_PROP_BTN_STYLUS,              /* 0x0b */
        BTN_LABEL_PROP_BTN_STYLUS2,             /* 0x0c */
        BTN_LABEL_PROP_BTN_TOOL_DOUBLETAP,      /* 0x0d */
        BTN_LABEL_PROP_BTN_TOOL_TRIPLETAP       /* 0x0e */
    },
    { /* BTN_WHEEL group                        offset 0x150 */
        BTN_LABEL_PROP_BTN_GEAR_DOWN,           /* 0x00 */
        BTN_LABEL_PROP_BTN_GEAR_UP              /* 0x01 */
    }
};

static void EvdevInitAxesLabels(EvdevPtr pEvdev, int mode, int natoms, Atom *atoms)
{
    Atom atom;
    int axis;
    char **labels;
    int labels_len = 0;

#ifdef _F_EVDEV_SUPPORT_GAMEPAD
    if (mode == Absolute || pEvdev->flags & EVDEV_GAMEPAD)
#else
    if (mode == Absolute)
#endif//_F_EVDEV_SUPPORT_GAMEPAD
    {
        labels     = abs_labels;
        labels_len = ArrayLength(abs_labels);
    } else if (mode == Relative)
    {
        labels     = rel_labels;
        labels_len = ArrayLength(rel_labels);
    } else
        return;

    memset(atoms, 0, natoms * sizeof(Atom));

    /* Now fill the ones we know */
    for (axis = 0; axis < labels_len; axis++)
    {
        if (pEvdev->axis_map[axis] == -1)
            continue;

        atom = XIGetKnownProperty(labels[axis]);
        if (!atom) /* Should not happen */
            continue;

        atoms[pEvdev->axis_map[axis]] = atom;
    }
}

static void EvdevInitButtonLabels(EvdevPtr pEvdev, int natoms, Atom *atoms)
{
    Atom atom;
    int button, bmap;

    /* First, make sure all atoms are initialized */
    atom = XIGetKnownProperty(BTN_LABEL_PROP_BTN_UNKNOWN);
    for (button = 0; button < natoms; button++)
        atoms[button] = atom;

#ifdef _F_EVDEV_SUPPORT_GAMEPAD
    for (button = BTN_MISC; button < BTN_THUMBR; button++)
#else
    for (button = BTN_MISC; button < BTN_JOYSTICK; button++)
#endif//_F_EVDEV_SUPPORT_GAMEPAD
    {
        if (EvdevBitIsSet(pEvdev->key_bitmask, button))
        {
            int group = (button % 0x100)/16;
            int idx = button - ((button/16) * 16);

            if ((unsigned int)group >= sizeof(btn_labels)/sizeof(btn_labels[0]))
                continue;

            if (!btn_labels[group][idx])
                continue;

            atom = XIGetKnownProperty(btn_labels[group][idx]);
            if (!atom)
                continue;

            /* Props are 0-indexed, button numbers start with 1 */
            bmap = EvdevUtilButtonEventToButtonNumber(pEvdev, button) - 1;
            if(bmap <0)
               continue;

            atoms[bmap] = atom;
        }
    }

    /* wheel buttons, hardcoded anyway */
    if (natoms > 3)
        atoms[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
    if (natoms > 4)
        atoms[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
    if (natoms > 5)
        atoms[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
    if (natoms > 6)
        atoms[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
}

static void
EvdevInitProperty(DeviceIntPtr dev)
{
    InputInfoPtr pInfo  = dev->public.devicePrivate;
    EvdevPtr     pEvdev = pInfo->private;
    int          rc;
    char         *device_node;
#ifdef _F_ENABLE_DEVICE_TYPE_PROP_
    char         *device_type;
#endif /* #ifdef _F_ENABLE_DEVICE_TYPE_PROP_ */
#ifdef _F_EVDEV_CONFINE_REGION_
    int region[6] = { 0, };
#endif /* _F_EVDEV_CONFINE_REGION_ */

    CARD32       product[2];

    prop_product_id = MakeAtom(XI_PROP_PRODUCT_ID, strlen(XI_PROP_PRODUCT_ID), TRUE);
    product[0] = pEvdev->id_vendor;
    product[1] = pEvdev->id_product;
    rc = XIChangeDeviceProperty(dev, prop_product_id, XA_INTEGER, 32,
                                PropModeReplace, 2, product, FALSE);
    if (rc != Success)
        return;

    XISetDevicePropertyDeletable(dev, prop_product_id, FALSE);

    /* Device node property */
    device_node = strdup(pEvdev->device);
    prop_device = MakeAtom(XI_PROP_DEVICE_NODE,
                           strlen(XI_PROP_DEVICE_NODE), TRUE);
    rc = XIChangeDeviceProperty(dev, prop_device, XA_STRING, 8,
                                PropModeReplace,
                                (device_node?strlen(device_node):0), device_node,
                                FALSE);
    free(device_node);

    if (rc != Success)
        return;

    XISetDevicePropertyDeletable(dev, prop_device, FALSE);

#ifdef _F_ENABLE_DEVICE_TYPE_PROP_
    /* Device node property */
    device_type = strdup(pInfo->type_name);
    prop_device_type = MakeAtom(XI_PROP_DEVICE_TYPE,
                           strlen(XI_PROP_DEVICE_TYPE), TRUE);
    rc = XIChangeDeviceProperty(dev, prop_device_type, XA_STRING, 8,
                                PropModeReplace,
                                (device_type?strlen(device_type):0), device_type,
                                FALSE);
    free(device_type);

    if (rc != Success)
        return;
#endif /* #ifdef _F_ENABLE_DEVICE_TYPE_PROP_ */

#ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_
    pEvdev->rel_move_status = 0;

    /* Relative movement property */
    prop_relative_move_status = MakeAtom(XI_PROP_REL_MOVE_STATUS,
                                        strlen(XI_PROP_REL_MOVE_STATUS), TRUE);
    prop_relative_move_ack = MakeAtom(XI_PROP_REL_MOVE_ACK,
                                        strlen(XI_PROP_REL_MOVE_ACK), TRUE);

    ErrorF("[%s] prop_relative_move_status = %d, prop_relative_move_ack = %d\n", __FUNCTION__, prop_relative_move_status, prop_relative_move_ack);
#endif /* #ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_ */

    if (EvdevDeviceIsVirtual(pEvdev->device))
    {
        BOOL virtual = 1;
        prop_virtual = MakeAtom(XI_PROP_VIRTUAL_DEVICE,
                                strlen(XI_PROP_VIRTUAL_DEVICE), TRUE);
        rc = XIChangeDeviceProperty(dev, prop_virtual, XA_INTEGER, 8,
                                    PropModeReplace, 1, &virtual, FALSE);
        XISetDevicePropertyDeletable(dev, prop_virtual, FALSE);
    }

    XISetDevicePropertyDeletable(dev, prop_device, FALSE);

#ifdef _F_EVDEV_CONFINE_REGION_
    if (pEvdev->flags & EVDEV_RELATIVE_EVENTS)
    {
        prop_confine_region = MakeAtom(EVDEV_PROP_CONFINE_REGION,
                strlen(EVDEV_PROP_CONFINE_REGION), TRUE);
        rc = XIChangeDeviceProperty(dev, prop_confine_region, XA_INTEGER,
                    32, PropModeReplace, 4, region, FALSE);
                    //here
                    //32, PropModeReplace, 6, region, FALSE);

        if (rc != Success)
            return;

        XISetDevicePropertyDeletable(dev, prop_confine_region, FALSE);
    }
#endif /* #ifdef _F_EVDEV_CONFINE_REGION_ */

#ifdef _F_TOUCH_TRANSFORM_MATRIX_
    if (strstr(pInfo->type_name, XI_TOUCHSCREEN))
    {
        /* matrix to transform */
        prop_transform = MakeAtom(EVDEV_PROP_TOUCH_TRANSFORM_MATRIX, strlen(EVDEV_PROP_TOUCH_TRANSFORM_MATRIX),  TRUE);
        rc = XIChangeDeviceProperty(dev, prop_transform, XIGetKnownProperty(XATOM_FLOAT), 32, PropModeReplace, 9, pEvdev->transform, FALSE);

        if (rc != Success)
            return;

        XISetDevicePropertyDeletable(dev, prop_transform, FALSE);
    }
#endif /* #ifdef _F_TOUCH_TRANSFORM_MATRIX_ */


    if (pEvdev->flags & (EVDEV_RELATIVE_EVENTS | EVDEV_ABSOLUTE_EVENTS))
    {
        BOOL invert[2];
        invert[0] = pEvdev->invert_x;
        invert[1] = pEvdev->invert_y;

        prop_invert = MakeAtom(EVDEV_PROP_INVERT_AXES, strlen(EVDEV_PROP_INVERT_AXES), TRUE);

        rc = XIChangeDeviceProperty(dev, prop_invert, XA_INTEGER, 8,
                PropModeReplace, 2,
                invert, FALSE);
        if (rc != Success)
            return;

        XISetDevicePropertyDeletable(dev, prop_invert, FALSE);

        prop_calibration = MakeAtom(EVDEV_PROP_CALIBRATION,
                strlen(EVDEV_PROP_CALIBRATION), TRUE);
        if (pEvdev->flags & EVDEV_CALIBRATED) {
            int calibration[4];

            calibration[0] = pEvdev->calibration.min_x;
            calibration[1] = pEvdev->calibration.max_x;
            calibration[2] = pEvdev->calibration.min_y;
            calibration[3] = pEvdev->calibration.max_y;

            rc = XIChangeDeviceProperty(dev, prop_calibration, XA_INTEGER,
                    32, PropModeReplace, 4, calibration,
                    FALSE);
        } else if (pEvdev->flags & EVDEV_ABSOLUTE_EVENTS) {
            rc = XIChangeDeviceProperty(dev, prop_calibration, XA_INTEGER,
                    32, PropModeReplace, 0, NULL,
                    FALSE);
        }
        if (rc != Success)
            return;

        XISetDevicePropertyDeletable(dev, prop_calibration, FALSE);

        prop_swap = MakeAtom(EVDEV_PROP_SWAP_AXES,
                strlen(EVDEV_PROP_SWAP_AXES), TRUE);

        rc = XIChangeDeviceProperty(dev, prop_swap, XA_INTEGER, 8,
                PropModeReplace, 1, &pEvdev->swap_axes, FALSE);
        if (rc != Success)
            return;

        XISetDevicePropertyDeletable(dev, prop_swap, FALSE);

        /* Axis labelling */
        if ((pEvdev->num_vals > 0) && (prop_axis_label = XIGetKnownProperty(AXIS_LABEL_PROP)))
        {
            int mode;
            Atom atoms[pEvdev->num_vals];

            if (pEvdev->flags & EVDEV_ABSOLUTE_EVENTS)
                mode = Absolute;
            else if (pEvdev->flags & EVDEV_RELATIVE_EVENTS)
                mode = Relative;
            else {
                xf86IDrvMsg(pInfo, X_ERROR, "BUG: mode is neither absolute nor relative\n");
                mode = Absolute;
            }

            EvdevInitAxesLabels(pEvdev, mode, pEvdev->num_vals, atoms);
            XIChangeDeviceProperty(dev, prop_axis_label, XA_ATOM, 32,
                                   PropModeReplace, pEvdev->num_vals, atoms, FALSE);
            XISetDevicePropertyDeletable(dev, prop_axis_label, FALSE);
        }
        /* Button labelling */
        if ((pEvdev->num_buttons > 0) && (prop_btn_label = XIGetKnownProperty(BTN_LABEL_PROP)))
        {
            Atom atoms[EVDEV_MAXBUTTONS];
            EvdevInitButtonLabels(pEvdev, EVDEV_MAXBUTTONS, atoms);
            XIChangeDeviceProperty(dev, prop_btn_label, XA_ATOM, 32,
                                   PropModeReplace, pEvdev->num_buttons, atoms, FALSE);
            XISetDevicePropertyDeletable(dev, prop_btn_label, FALSE);
        }
    }
}

static int
EvdevSetProperty(DeviceIntPtr dev, Atom atom, XIPropertyValuePtr val,
                 BOOL checkonly)
{
    InputInfoPtr pInfo  = dev->public.devicePrivate;
    EvdevPtr     pEvdev = pInfo->private;

    if (atom == prop_invert)
    {
        BOOL* data;
        if (val->format != 8 || val->size != 2 || val->type != XA_INTEGER)
            return BadMatch;

        if (!checkonly)
        {
            data = (BOOL*)val->data;
            pEvdev->invert_x = data[0];
            pEvdev->invert_y = data[1];
        }
    } else if (atom == prop_calibration)
    {
        if (val->format != 32 || val->type != XA_INTEGER)
            return BadMatch;
        if (val->size != 4 && val->size != 0)
            return BadMatch;

        if (!checkonly)
            EvdevSetCalibration(pInfo, val->size, val->data);
    } else if (atom == prop_swap)
    {
        if (val->format != 8 || val->type != XA_INTEGER || val->size != 1)
            return BadMatch;

        if (!checkonly)
            pEvdev->swap_axes = *((BOOL*)val->data);
    } else if (atom == prop_axis_label || atom == prop_btn_label ||
               atom == prop_product_id || atom == prop_device ||
#ifdef _F_ENABLE_DEVICE_TYPE_PROP_
               atom == prop_virtual || atom == prop_device_type)
#else /* #ifdef _F_ENABLE_DEVICE_TYPE_PROP_ */
               atom == prop_virtual)
#endif /* #ifdef _F_ENABLE_DEVICE_TYPE_PROP_ */
        return BadAccess; /* Read-only properties */
#ifdef _F_TOUCH_TRANSFORM_MATRIX_
    else if (atom == prop_transform)
    {
        float *f;

        if (val->format != 32 || val->type != XIGetKnownProperty(XATOM_FLOAT)
			|| ((val->size != 9) && (val->size != 1)))
            return BadMatch;
        if (!checkonly) {
            f = (float*)val->data;
            EvdevSetTransformMatrix(pInfo, val->size, f);
        }
    }
#endif /* #ifdef _F_TOUCH_TRANSFORM_MATRIX_ */
#ifdef _F_EVDEV_CONFINE_REGION_
    else if (atom == prop_confine_region)
    {
        if (val->format != 32 || val->type != XA_INTEGER)
            return BadMatch;
        if (val->size != 1 && val->size != 6)
            return BadMatch;

        if (!checkonly)
            EvdevSetConfineRegion(pInfo, val->size, val->data);
    }
#endif /* #ifdef _F_EVDEV_CONFINE_REGION_ */
#ifdef _F_ENABLE_REL_MOVE_STATUS_PROP_
    else if (atom == prop_relative_move_ack)
    {
        if (val->format != 8 || val->type != XA_INTEGER)
            return BadMatch;
        if (val->size != 1)
            return BadMatch;

        if (!checkonly)
            pEvdev->rel_move_ack = 1;
    }
#endif /* _F_ENABLE_REL_MOVE_STATUS_PROP_ */


    return Success;
}
