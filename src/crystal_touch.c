// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// ============================================================================
//  Crystal Touch Input & Display Rotation — implementation
// ============================================================================
//
// This file implements multitouch input handling for AuraOS using XInput2.
//
// The flow is:
//   1. touch_init() finds touchscreen hardware via XIQueryDevice and registers
//      for TouchBegin/TouchUpdate/TouchEnd events on the root window.
//   2. The main event loop calls touch_handle_event() for each XEvent. If it's
//      a touch event, we update our internal tracking array and run gesture
//      detection.
//   3. When a gesture is recognized (tap, pinch, swipe, etc.), we fire the
//      registered callback so the compositor or WM can act on it.
//   4. For simple taps and drags, we also synthesize XTest pointer events so
//      that normal X11 applications (which only understand mouse input) still
//      receive clicks and motion.
//
// Display rotation transforms touch coordinates so that finger positions map
// correctly when the screen is rotated (portrait mode on the Legion Go).
//
// Auto-rotation listens to iio-sensor-proxy via D-Bus for accelerometer data
// to automatically switch between landscape and portrait orientations.
//
// On-screen keyboard support uses D-Bus to communicate with Maliit or
// Squeekboard (common Linux OSKs) to show/hide the virtual keyboard.
//
// ============================================================================

// _GNU_SOURCE gives us access to M_PI and other POSIX extensions from <math.h>.
#define _GNU_SOURCE

#include "crystal_touch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// XInput2 headers — these provide the multitouch API.
// XI2 was introduced in X.Org 1.8 and version 2.2 added multitouch support.
#include <X11/extensions/XInput2.h>

// XTest extension — allows us to inject synthetic mouse events so that
// X11 apps that don't understand touch can still receive clicks and drags.
#include <X11/extensions/XTest.h>


// ============================================================================
//  Constants
// ============================================================================

// Maximum number of simultaneous touch points we track. Most touchscreens
// support 10 (one per finger), but we only need a few for gesture detection.
#define MAX_TOUCH_POINTS  10

// Gesture detection thresholds — these control how sensitive the gesture
// recognizer is. Tuned for the Legion Go's 8.8" 1920x1200 screen.

// Long press: hold for this many seconds without moving
#define LONG_PRESS_TIME   0.5

// Long press: maximum movement (pixels) allowed during the hold
#define LONG_PRESS_RADIUS 10.0f

// Tap: maximum duration (seconds) for a quick tap
#define TAP_MAX_TIME      0.3

// Tap: maximum movement (pixels) during the tap
#define TAP_MAX_DISTANCE  15.0f

// Pinch: minimum scale change before recognizing (20% zoom)
#define PINCH_THRESHOLD   0.2f

// Scroll: minimum vertical movement (pixels) for two-finger scroll
#define SCROLL_THRESHOLD  20.0f

// Swipe: minimum distance (pixels) for three-finger vertical swipes
#define SWIPE_3F_THRESHOLD 100.0f

// Swipe: minimum distance (pixels) for four-finger horizontal swipes
#define SWIPE_4F_THRESHOLD 150.0f


// ============================================================================
//  Module state (file-scoped)
// ============================================================================
// All touch state lives in static variables so the module is self-contained.
// Only one touchscreen input system runs at a time (there's one X display).

// The array of tracked touch points. Each slot is either active (finger down)
// or inactive (available for reuse).
static TouchPoint touch_points[MAX_TOUCH_POINTS];

// Current display rotation angle
static RotationAngle current_rotation = ROTATION_0;

// Gesture callback and its user-provided context pointer
static GestureCallback gesture_cb = NULL;
static void *gesture_userdata = NULL;

// XInput2 opcode — needed to identify GenericEvents that belong to XInput2.
// Every X11 extension has its own opcode; we store it at init time.
static int xi_opcode = 0;

// Whether the on-screen keyboard is currently showing
static bool osk_is_visible = false;

// Whether auto-rotation is active
static bool auto_rotation_enabled = false;

// Cached screen dimensions (set during init, used for coordinate transforms)
static int screen_width = 0;
static int screen_height = 0;

// Whether a long-press has already been fired for the current single-finger
// touch. This prevents the long-press from firing repeatedly while the
// finger is still held down.
static bool long_press_fired = false;

// Whether the current set of touches has already triggered a gesture.
// Once a gesture fires, we don't want to keep re-firing it every frame.
static bool gesture_fired = false;


// ============================================================================
//  Helper: monotonic clock (seconds)
// ============================================================================
// Returns the current time in seconds from a monotonic clock source.
// CLOCK_MONOTONIC is immune to system clock changes (NTP, user adjustments),
// so gesture timing won't glitch if the clock jumps.
static double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}


// ============================================================================
//  Touch point management
// ============================================================================
// These functions maintain the touch_points[] array. Each finger on the screen
// occupies one slot. When a finger lifts off, that slot becomes available.

// Find a touch point by its XInput2 ID.
// Returns NULL if no active point has that ID.
static TouchPoint *find_touch_by_id(int id)
{
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        if (touch_points[i].active && touch_points[i].id == id) {
            return &touch_points[i];
        }
    }
    return NULL;
}

// Find the first inactive slot in the touch point array.
// Returns NULL if all slots are in use (very unlikely with 10 slots).
static TouchPoint *find_free_slot(void)
{
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        if (!touch_points[i].active) {
            return &touch_points[i];
        }
    }
    return NULL;
}

// Count how many fingers are currently touching the screen.
static int count_active_touches(void)
{
    int count = 0;
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        if (touch_points[i].active) count++;
    }
    return count;
}

// Get the Nth active touch point (0-indexed).
// Used by gesture recognition to access specific fingers (e.g., "the first
// two fingers" for pinch detection). Returns NULL if index is out of range.
static TouchPoint *get_active_touch(int index)
{
    int seen = 0;
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        if (touch_points[i].active) {
            if (seen == index) return &touch_points[i];
            seen++;
        }
    }
    return NULL;
}


// ============================================================================
//  Touch point lifecycle
// ============================================================================
// Called by touch_handle_event() when XInput2 sends TouchBegin/Update/End.

// A new finger touched the screen. Record its position and timestamp.
static void add_touch_point(int id, double x, double y)
{
    TouchPoint *tp = find_free_slot();
    if (!tp) {
        fprintf(stderr, "[touch] Warning: no free touch point slots\n");
        return;
    }

    tp->id = id;
    tp->x = (float)x;
    tp->y = (float)y;
    tp->start_x = (float)x;
    tp->start_y = (float)y;
    tp->start_time = get_time();
    tp->active = true;

    // Reset gesture state when a new finger goes down. A new finger means
    // the user might be starting a different gesture.
    long_press_fired = false;
    gesture_fired = false;
}

// A finger that's already on the screen moved to a new position.
static void update_touch_point(int id, double x, double y)
{
    TouchPoint *tp = find_touch_by_id(id);
    if (!tp) return;  // Unknown ID — shouldn't happen, but be safe

    tp->x = (float)x;
    tp->y = (float)y;
}

// A finger lifted off the screen. We check for tap gestures here because
// a tap is only recognized once the finger is released (within a short time
// window and without much movement).
static void end_touch_point(int id, double x, double y)
{
    TouchPoint *tp = find_touch_by_id(id);
    if (!tp) return;

    // Update to final position
    tp->x = (float)x;
    tp->y = (float)y;

    // Check for a tap: quick touch with minimal movement
    double elapsed = get_time() - tp->start_time;
    float dist = hypotf(tp->x - tp->start_x, tp->y - tp->start_y);

    if (elapsed < TAP_MAX_TIME && dist < TAP_MAX_DISTANCE && !gesture_fired) {
        int active = count_active_touches();

        if (active == 1) {
            // Single-finger tap → left click
            // Fire gesture callback and also synthesize a pointer click
            if (gesture_cb) {
                gesture_cb(GESTURE_TAP, tp->x, tp->y, gesture_userdata);
            }
        } else if (active == 2) {
            // Two-finger tap → right click
            if (gesture_cb) {
                gesture_cb(GESTURE_TWO_FINGER_TAP, tp->x, tp->y, gesture_userdata);
            }
        }

        gesture_fired = true;
    }

    // Mark this touch point as inactive (slot can be reused)
    tp->active = false;
}


// ============================================================================
//  Gesture firing helper
// ============================================================================
// Calls the registered gesture callback (if any) with the given gesture type
// and parameters. Also sets gesture_fired to prevent duplicate detection.

static void fire_gesture(GestureType type, float param1, float param2)
{
    gesture_fired = true;
    if (gesture_cb) {
        gesture_cb(type, param1, param2, gesture_userdata);
    }
}


// ============================================================================
//  Gesture recognition
// ============================================================================
// Called every time a touch point moves. Examines the current set of active
// touches and checks if they match any known gesture pattern.
//
// Gesture priority (to avoid conflicts):
//   - More fingers take priority (4-finger swipe > 3-finger swipe > pinch)
//   - Movement-based gestures require a minimum threshold before firing
//   - Once a gesture fires, no more gestures are detected until all fingers
//     lift and a new touch sequence begins
//
// The thresholds are tuned for the Legion Go's screen. On a larger screen
// you'd want larger thresholds; on a phone, smaller ones.

static void check_gestures(void)
{
    // Don't re-fire if we already detected a gesture for this touch sequence
    if (gesture_fired) return;

    int active = count_active_touches();

    if (active == 1) {
        // ── Single finger: check for long press ──
        // A long press is a finger held in one place for at least 500ms.
        // This is the touch equivalent of a right-click (context menu).
        TouchPoint *tp = get_active_touch(0);
        if (!tp) return;

        double elapsed = get_time() - tp->start_time;
        float dist = hypotf(tp->x - tp->start_x, tp->y - tp->start_y);

        if (elapsed > LONG_PRESS_TIME && dist < LONG_PRESS_RADIUS
            && !long_press_fired) {
            fire_gesture(GESTURE_LONG_PRESS, tp->x, tp->y);
            long_press_fired = true;
            // Deactivate this touch so it doesn't also generate a tap on release
            tp->active = false;
        }
    }
    else if (active == 2) {
        // ── Two fingers: pinch-to-zoom or scroll ──
        TouchPoint *a = get_active_touch(0);
        TouchPoint *b = get_active_touch(1);
        if (!a || !b) return;

        // Pinch detection: compare the current distance between the two fingers
        // to their starting distance. If the ratio changes enough, it's a pinch.
        float start_dist = hypotf(a->start_x - b->start_x,
                                  a->start_y - b->start_y);
        float curr_dist  = hypotf(a->x - b->x, a->y - b->y);

        // Avoid division by zero if both fingers started at the same spot
        float scale = curr_dist / fmaxf(start_dist, 1.0f);

        if (fabsf(scale - 1.0f) > PINCH_THRESHOLD) {
            // Scale > 1.0 means fingers moved apart (zoom in)
            // Scale < 1.0 means fingers moved together (zoom out)
            fire_gesture(scale > 1.0f ? GESTURE_PINCH_OUT : GESTURE_PINCH_IN,
                         scale, 0.0f);
            return;  // Pinch takes priority over scroll
        }

        // Scroll detection: if both fingers are moving in the same vertical
        // direction, treat it as a scroll gesture. We average the Y movement
        // of both fingers to get a smooth scroll delta.
        float dy = ((a->y + b->y) / 2.0f)
                 - ((a->start_y + b->start_y) / 2.0f);

        if (fabsf(dy) > SCROLL_THRESHOLD) {
            fire_gesture(GESTURE_SCROLL, 0.0f, dy);
        }
    }
    else if (active == 3) {
        // ── Three fingers: swipe up (Mission Control) or down (Exposé) ──
        // Average the vertical movement of all three fingers. If they all
        // move in the same direction past the threshold, it's a swipe.
        float avg_dy = 0.0f;
        for (int i = 0; i < 3; i++) {
            TouchPoint *tp = get_active_touch(i);
            if (!tp) return;
            avg_dy += (tp->y - tp->start_y);
        }
        avg_dy /= 3.0f;

        if (avg_dy < -SWIPE_3F_THRESHOLD) {
            fire_gesture(GESTURE_SWIPE_UP, 0.0f, avg_dy);
        } else if (avg_dy > SWIPE_3F_THRESHOLD) {
            fire_gesture(GESTURE_SWIPE_DOWN, 0.0f, avg_dy);
        }
    }
    else if (active == 4) {
        // ── Four fingers: swipe left/right (switch Spaces) ──
        // Same idea as three-finger swipe but horizontal, and requires a
        // larger threshold to avoid accidental triggers.
        float avg_dx = 0.0f;
        for (int i = 0; i < 4; i++) {
            TouchPoint *tp = get_active_touch(i);
            if (!tp) return;
            avg_dx += (tp->x - tp->start_x);
        }
        avg_dx /= 4.0f;

        if (avg_dx < -SWIPE_4F_THRESHOLD) {
            fire_gesture(GESTURE_SWIPE_LEFT, avg_dx, 0.0f);
        } else if (avg_dx > SWIPE_4F_THRESHOLD) {
            fire_gesture(GESTURE_SWIPE_RIGHT, avg_dx, 0.0f);
        }
    }
}


// ============================================================================
//  Synthetic pointer events
// ============================================================================
// X11 applications don't understand touch events — they only know about mouse
// clicks and motion. When a single finger taps or drags, we inject fake mouse
// events using XTest so that regular apps work as expected.

// Send a synthetic mouse button press or release at the given coordinates.
// button: X11 button number (1 = left, 2 = middle, 3 = right)
// is_press: true for button down, false for button up
static void synthesize_click(Display *dpy, float x, float y, int button,
                             bool is_press)
{
    // Move the pointer to the touch position first
    XTestFakeMotionEvent(dpy, DefaultScreen(dpy), (int)x, (int)y, 0);

    // Then press or release the button
    XTestFakeButtonEvent(dpy, button, is_press ? True : False, 0);

    // Flush to make sure the events are sent immediately
    XFlush(dpy);
}

// Send a synthetic mouse motion event (for finger drag → pointer movement).
static void synthesize_motion(Display *dpy, float x, float y)
{
    XTestFakeMotionEvent(dpy, DefaultScreen(dpy), (int)x, (int)y, 0);
    XFlush(dpy);
}


// ============================================================================
//  Public API: Initialization
// ============================================================================

bool touch_init(Display *dpy, Window root)
{
    // ── Step 1: Check that the XInput2 extension is available ──
    // Every X11 extension registers itself with an opcode. We need this
    // opcode later to identify which GenericEvents belong to XInput2.
    int xi_event, xi_error;
    if (!XQueryExtension(dpy, "XInputExtension",
                         &xi_opcode, &xi_event, &xi_error)) {
        fprintf(stderr, "[touch] XInput2 extension not available\n");
        return false;
    }

    // ── Step 2: Verify XInput2 version (need 2.2+ for multitouch) ──
    // Version 2.0 had basic device events; 2.2 added TouchBegin/Update/End.
    int major = 2, minor = 2;
    if (XIQueryVersion(dpy, &major, &minor) != Success) {
        fprintf(stderr, "[touch] XInput2 version too old "
                        "(have %d.%d, need 2.2+)\n", major, minor);
        return false;
    }
    fprintf(stderr, "[touch] XInput2 version %d.%d available\n",
            major, minor);

    // ── Step 3: Find touch-capable devices ──
    // XIQueryDevice returns info about all input devices. We scan through
    // them looking for devices that have a XITouchClass, which indicates
    // they support multitouch input.
    int ndevices;
    XIDeviceInfo *devices = XIQueryDevice(dpy, XIAllDevices, &ndevices);
    bool found_touch = false;

    for (int i = 0; i < ndevices; i++) {
        for (int j = 0; j < devices[i].num_classes; j++) {
            if (devices[i].classes[j]->type == XITouchClass) {
                XITouchClassInfo *tc =
                    (XITouchClassInfo *)devices[i].classes[j];

                fprintf(stderr, "[touch] Found touch device: \"%s\" "
                        "(%d simultaneous touch points)\n",
                        devices[i].name, tc->num_touches);

                // ── Step 4: Register for touch events on the root window ──
                // We select TouchBegin, TouchUpdate, and TouchEnd events.
                // These arrive as GenericEvents (XInput2's event delivery
                // mechanism) and contain the touch point ID and coordinates.
                XIEventMask mask;
                mask.deviceid = devices[i].deviceid;
                mask.mask_len = XIMaskLen(XI_TouchEnd);
                mask.mask = calloc(mask.mask_len, 1);

                if (!mask.mask) {
                    fprintf(stderr, "[touch] Failed to allocate event mask\n");
                    continue;
                }

                // Set bits for the three touch event types
                XISetMask(mask.mask, XI_TouchBegin);
                XISetMask(mask.mask, XI_TouchUpdate);
                XISetMask(mask.mask, XI_TouchEnd);

                XISelectEvents(dpy, root, &mask, 1);
                free(mask.mask);

                found_touch = true;
                // Don't break — there might be multiple touch devices.
                // Registering on all of them is fine.
            }
        }
    }

    XIFreeDeviceInfo(devices);

    if (!found_touch) {
        fprintf(stderr, "[touch] No touch devices found\n");
        return false;
    }

    // ── Step 5: Initialize internal state ──
    memset(touch_points, 0, sizeof(touch_points));
    current_rotation = ROTATION_0;
    osk_is_visible = false;
    long_press_fired = false;
    gesture_fired = false;

    // Cache screen dimensions for coordinate transforms
    screen_width = DisplayWidth(dpy, DefaultScreen(dpy));
    screen_height = DisplayHeight(dpy, DefaultScreen(dpy));

    fprintf(stderr, "[touch] Initialized (%dx%d screen)\n",
            screen_width, screen_height);
    return true;
}


// ============================================================================
//  Public API: Event handling
// ============================================================================

bool touch_handle_event(Display *dpy, XEvent *ev)
{
    // XInput2 events arrive as GenericEvents (type == GenericEvent).
    // We need to check the extension field to make sure it's from XInput2
    // and not some other extension.
    if (ev->type != GenericEvent) return false;

    XGenericEventCookie *cookie = &ev->xcookie;

    // Check if this GenericEvent belongs to XInput2
    if (cookie->extension != xi_opcode) return false;

    // XGetEventData() fills in the cookie's data pointer with the actual
    // event payload. This must be called before accessing cookie->data,
    // and XFreeEventData() must be called when we're done.
    if (!XGetEventData(dpy, cookie)) return false;

    bool consumed = false;

    switch (cookie->evtype) {

    case XI_TouchBegin: {
        // A new finger touched the screen
        XIDeviceEvent *te = (XIDeviceEvent *)cookie->data;

        // Apply rotation transform so the coordinates match the logical screen
        float tx = (float)te->root_x;
        float ty = (float)te->root_y;
        touch_transform_coords(&tx, &ty, screen_width, screen_height);

        add_touch_point(te->detail, tx, ty);

        // Synthesize a pointer motion to the touch location. This makes the
        // X cursor follow the finger, which helps apps that highlight on hover.
        synthesize_motion(dpy, tx, ty);

        consumed = true;
        break;
    }

    case XI_TouchUpdate: {
        // A finger already on the screen moved to a new position
        XIDeviceEvent *te = (XIDeviceEvent *)cookie->data;

        float tx = (float)te->root_x;
        float ty = (float)te->root_y;
        touch_transform_coords(&tx, &ty, screen_width, screen_height);

        update_touch_point(te->detail, tx, ty);

        // Run gesture detection on every movement update
        check_gestures();

        // If this is a single-finger drag and no gesture has been detected,
        // synthesize pointer motion for drag-to-select, scrollbar dragging, etc.
        if (count_active_touches() == 1 && !gesture_fired) {
            synthesize_motion(dpy, tx, ty);
        }

        consumed = true;
        break;
    }

    case XI_TouchEnd: {
        // A finger lifted off the screen
        XIDeviceEvent *te = (XIDeviceEvent *)cookie->data;

        float tx = (float)te->root_x;
        float ty = (float)te->root_y;
        touch_transform_coords(&tx, &ty, screen_width, screen_height);

        // Check how many fingers were down before this one lifts
        int was_active = count_active_touches();

        end_touch_point(te->detail, tx, ty);

        // If this was a single-finger tap that got recognized, synthesize
        // a full click (press + release) so X11 apps see a mouse click
        if (was_active == 1 && gesture_fired) {
            // The gesture callback already fired from end_touch_point().
            // Now send the synthetic click.
            synthesize_click(dpy, tx, ty, 1, true);   // Button 1 press
            synthesize_click(dpy, tx, ty, 1, false);   // Button 1 release
        }

        // If a long press was fired, send a right-click instead
        if (long_press_fired && was_active == 1) {
            synthesize_click(dpy, tx, ty, 3, true);   // Button 3 press
            synthesize_click(dpy, tx, ty, 3, false);   // Button 3 release
        }

        consumed = true;
        break;
    }
    }  // end switch

    // Always free the event data when done — this is required by XInput2.
    XFreeEventData(dpy, cookie);
    return consumed;
}


// ============================================================================
//  Public API: Gesture callback
// ============================================================================

void touch_set_gesture_callback(GestureCallback cb, void *userdata)
{
    gesture_cb = cb;
    gesture_userdata = userdata;
}


// ============================================================================
//  Public API: Display rotation
// ============================================================================

void touch_set_rotation(RotationAngle angle)
{
    // Validate the angle — only 0, 90, 180, 270 are meaningful
    switch (angle) {
    case ROTATION_0:
    case ROTATION_90:
    case ROTATION_180:
    case ROTATION_270:
        break;
    default:
        fprintf(stderr, "[touch] Invalid rotation angle: %d\n", angle);
        return;
    }

    if (current_rotation != angle) {
        fprintf(stderr, "[touch] Display rotation changed: %d° → %d°\n",
                current_rotation, angle);
        current_rotation = angle;
    }

    // Note: The compositor must re-setup its GL projection matrix after this.
    // Crystal's render loop should call touch_get_rotation() when building
    // the orthographic projection and glViewport to apply the rotation.
}

RotationAngle touch_get_rotation(void)
{
    return current_rotation;
}

void touch_transform_coords(float *x, float *y, int screen_w, int screen_h)
{
    // Transform physical touch coordinates into logical screen coordinates
    // based on the current rotation angle.
    //
    // Imagine the screen as a rectangle. When you rotate the device 90°
    // clockwise, the physical top-left becomes the logical top-right.
    // We need to remap (x, y) so that a finger tap at a physical position
    // maps to the correct logical window position.
    //
    // Coordinate transforms for each rotation:
    //   0°:   no change
    //   90°:  (x, y) → (y, W - x)    where W = screen width
    //   180°: (x, y) → (W - x, H - y)
    //   270°: (x, y) → (H - y, x)    where H = screen height

    float tx = *x;
    float ty = *y;

    switch (current_rotation) {
    case ROTATION_0:
        // No transformation needed — screen is in default orientation
        break;

    case ROTATION_90:
        // Device rotated 90° clockwise (right shoulder down)
        // Physical X axis maps to logical Y axis (inverted)
        *x = ty;
        *y = (float)screen_w - tx;
        break;

    case ROTATION_180:
        // Device upside down — both axes are mirrored
        *x = (float)screen_w - tx;
        *y = (float)screen_h - ty;
        break;

    case ROTATION_270:
        // Device rotated 90° counter-clockwise (left shoulder down)
        // Physical Y axis maps to logical X axis (inverted)
        *x = (float)screen_h - ty;
        *y = tx;
        break;
    }
}


// ============================================================================
//  Public API: Auto-rotation (accelerometer via iio-sensor-proxy)
// ============================================================================
// iio-sensor-proxy is a D-Bus service that reads accelerometer data from the
// hardware and exposes an "AccelerometerOrientation" property. On the Legion
// Go, this tells us whether the device is in landscape, portrait, etc.
//
// Full D-Bus integration would require libdbus or sd-bus. For now, we use a
// simpler approach: shell out to `monitor-sensor` or poll the D-Bus property
// using `gdbus`. A future version should use sd-bus for proper async handling.

bool touch_enable_auto_rotation(void)
{
    // Check if iio-sensor-proxy is running by looking for its D-Bus name.
    // We use system() for simplicity — a production version would use sd-bus.
    int ret = system("gdbus introspect --system "
                     "--dest net.hadess.SensorProxy "
                     "--object-path /net/hadess/SensorProxy "
                     "> /dev/null 2>&1");

    if (ret != 0) {
        fprintf(stderr, "[touch] iio-sensor-proxy not available "
                        "(auto-rotation disabled)\n");
        return false;
    }

    auto_rotation_enabled = true;
    fprintf(stderr, "[touch] Auto-rotation enabled via iio-sensor-proxy\n");

    // Claim the accelerometer so iio-sensor-proxy starts reporting
    system("gdbus call --system "
           "--dest net.hadess.SensorProxy "
           "--object-path /net/hadess/SensorProxy "
           "--method net.hadess.SensorProxy.ClaimAccelerometer "
           "> /dev/null 2>&1");

    return true;
}

void touch_disable_auto_rotation(void)
{
    if (!auto_rotation_enabled) return;

    // Release the accelerometer claim
    system("gdbus call --system "
           "--dest net.hadess.SensorProxy "
           "--object-path /net/hadess/SensorProxy "
           "--method net.hadess.SensorProxy.ReleaseAccelerometer "
           "> /dev/null 2>&1");

    auto_rotation_enabled = false;
    fprintf(stderr, "[touch] Auto-rotation disabled\n");
}

// Poll the current accelerometer orientation.
// Call this periodically (e.g., once per second) from the compositor's
// main loop when auto-rotation is enabled.
//
// This is NOT exposed in the header because it's an internal detail —
// the compositor should just call touch_enable_auto_rotation() and then
// periodically call this helper to update the rotation angle.
//
// Returns true if the rotation changed (so the compositor knows to update
// its projection matrix).
// TODO: Replace polling with D-Bus signal listening via sd-bus for lower
//       latency and less CPU overhead.
static bool poll_orientation(void)
{
    if (!auto_rotation_enabled) return false;

    // Read the AccelerometerOrientation property from iio-sensor-proxy.
    // The property value is a string like "normal", "left-up", "right-up",
    // or "bottom-up".
    FILE *fp = popen(
        "gdbus call --system "
        "--dest net.hadess.SensorProxy "
        "--object-path /net/hadess/SensorProxy "
        "--method org.freedesktop.DBus.Properties.Get "
        "net.hadess.SensorProxy AccelerometerOrientation 2>/dev/null",
        "r");

    if (!fp) return false;

    char buf[256];
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return false;
    }
    pclose(fp);

    // Parse the orientation string from the D-Bus response
    // The response looks like: (<'normal'>,)
    RotationAngle new_angle = current_rotation;

    if (strstr(buf, "normal")) {
        new_angle = ROTATION_0;
    } else if (strstr(buf, "left-up")) {
        // Left side of device is pointing up → rotated 270° (counter-clockwise)
        new_angle = ROTATION_270;
    } else if (strstr(buf, "right-up")) {
        // Right side of device is pointing up → rotated 90° (clockwise)
        new_angle = ROTATION_90;
    } else if (strstr(buf, "bottom-up")) {
        new_angle = ROTATION_180;
    }

    if (new_angle != current_rotation) {
        touch_set_rotation(new_angle);
        return true;
    }
    return false;
}


// ============================================================================
//  Public API: On-screen keyboard
// ============================================================================
// We support two common Linux on-screen keyboards:
//   - Maliit: Used by some mobile Linux distros, communicates over D-Bus
//   - Squeekboard: Used by Phosh/Purism, also D-Bus based
//
// For the initial implementation, we try Maliit first, then fall back to
// Squeekboard, then fall back to spawning an onboard instance.

void touch_request_osk(void)
{
    if (osk_is_visible) return;  // Already showing

    fprintf(stderr, "[touch] Requesting on-screen keyboard\n");

    // Try Maliit first (common on Plasma Mobile, Lomiri)
    int ret = system(
        "gdbus call --session "
        "--dest org.maliit.server "
        "--object-path /org/maliit/server/address "
        "--method org.maliit.Server.Activate "
        "> /dev/null 2>&1");

    if (ret == 0) {
        osk_is_visible = true;
        return;
    }

    // Try Squeekboard (Phosh / PureOS)
    ret = system(
        "busctl call --user sm.puri.OSK0 /sm/puri/OSK0 "
        "sm.puri.OSK0 SetVisible b true "
        "> /dev/null 2>&1");

    if (ret == 0) {
        osk_is_visible = true;
        return;
    }

    // Last resort: spawn 'onboard' (GNOME on-screen keyboard)
    ret = system("pgrep onboard > /dev/null 2>&1");
    if (ret != 0) {
        // onboard is not running — start it in the background
        ret = system("onboard &> /dev/null &");
        if (ret != 0) {
            fprintf(stderr, "[touch] No on-screen keyboard available\n");
            return;
        }
    }
    // If onboard is already running, it shows itself via its systray icon.
    // We can send it a D-Bus call to explicitly show.
    system("dbus-send --type=method_call --dest=org.onboard.Onboard "
           "/org/onboard/Onboard/Keyboard "
           "org.onboard.Onboard.Keyboard.Show "
           "> /dev/null 2>&1");

    osk_is_visible = true;
}

void touch_dismiss_osk(void)
{
    if (!osk_is_visible) return;

    fprintf(stderr, "[touch] Dismissing on-screen keyboard\n");

    // Try Maliit
    system("gdbus call --session "
           "--dest org.maliit.server "
           "--object-path /org/maliit/server/address "
           "--method org.maliit.Server.Deactivate "
           "> /dev/null 2>&1");

    // Try Squeekboard
    system("busctl call --user sm.puri.OSK0 /sm/puri/OSK0 "
           "sm.puri.OSK0 SetVisible b false "
           "> /dev/null 2>&1");

    // Try onboard
    system("dbus-send --type=method_call --dest=org.onboard.Onboard "
           "/org/onboard/Onboard/Keyboard "
           "org.onboard.Onboard.Keyboard.Hide "
           "> /dev/null 2>&1");

    osk_is_visible = false;
}

bool touch_osk_visible(void)
{
    return osk_is_visible;
}


// ============================================================================
//  Public API: Debug / introspection
// ============================================================================

TouchPoint *touch_get_points(int *count)
{
    if (count) {
        *count = MAX_TOUCH_POINTS;
    }
    return touch_points;
}


// ============================================================================
//  Public API: Shutdown
// ============================================================================

void touch_shutdown(void)
{
    fprintf(stderr, "[touch] Shutting down touch input\n");

    // Disable auto-rotation if it was enabled (releases the accelerometer)
    if (auto_rotation_enabled) {
        touch_disable_auto_rotation();
    }

    // Dismiss on-screen keyboard if visible
    if (osk_is_visible) {
        touch_dismiss_osk();
    }

    // Clear all touch state
    memset(touch_points, 0, sizeof(touch_points));
    gesture_cb = NULL;
    gesture_userdata = NULL;
    xi_opcode = 0;
    current_rotation = ROTATION_0;
    long_press_fired = false;
    gesture_fired = false;
}
