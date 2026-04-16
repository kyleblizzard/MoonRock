// Copyright (c) 2026 Kyle Blizzard
// SPDX-License-Identifier: BSD-3-Clause
//
// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
// www.blizzard.show/moonrock/
//
// ============================================================================
//  MoonRock Touch Input & Display Rotation — header
// ============================================================================
//
// This module makes AuraOS work as a tablet-like experience on devices with
// touchscreens (primarily the Lenovo Legion Go). It handles three things:
//
//   1. Touch-to-pointer translation:
//      XInput2 multitouch events get converted into synthetic pointer events
//      so that normal X11 apps (which only understand mouse clicks) can respond
//      to finger taps and drags.
//
//   2. Gesture recognition:
//      Multi-finger gestures (pinch, swipe, long-press) are detected and
//      dispatched through a callback. The compositor or WM can then map these
//      to actions like zooming, switching Spaces, or opening Mission Control.
//
//   3. Display rotation:
//      The GL projection matrix and touch coordinates can be rotated to support
//      portrait and inverted orientations. Auto-rotation via the accelerometer
//      (iio-sensor-proxy on Linux) is also supported.
//
//   4. On-screen keyboard triggering:
//      When a text field gains focus and no physical keyboard is detected, the
//      module can request an on-screen keyboard (e.g., Maliit or Squeekboard).
//
// ============================================================================

#ifndef MR_TOUCH_H
#define MR_TOUCH_H

#include <stdbool.h>
#include <X11/Xlib.h>

// ────────────────────────────────────────────────────────────────────────────
// Display rotation angles
// ────────────────────────────────────────────────────────────────────────────
// These correspond to physical rotation of the device. ROTATION_0 is the
// default landscape orientation. The compositor uses this to rotate its GL
// projection matrix, and the touch system uses it to remap coordinates so
// that a finger tap at screen position (x, y) maps to the correct logical
// position after rotation.
typedef enum {
    ROTATION_0   = 0,    // Normal landscape (default)
    ROTATION_90  = 90,   // Portrait — device rotated clockwise (right)
    ROTATION_180 = 180,  // Upside-down landscape
    ROTATION_270 = 270,  // Portrait — device rotated counter-clockwise (left)
} RotationAngle;

// ────────────────────────────────────────────────────────────────────────────
// Touch point tracking (multitouch)
// ────────────────────────────────────────────────────────────────────────────
// Each finger on the screen gets its own TouchPoint. The `id` comes from
// XInput2 and uniquely identifies that finger for the duration of the touch.
// We track both the current position and where the touch started so that
// gesture recognition can measure distance and direction.
typedef struct {
    int id;                   // Touch point ID assigned by XInput2
    float x, y;              // Current position (screen coordinates)
    float start_x, start_y; // Position when the finger first touched down
    double start_time;       // Timestamp (seconds) when touch began
    bool active;             // True while this finger is still on the screen
} TouchPoint;

// ────────────────────────────────────────────────────────────────────────────
// Gesture types
// ────────────────────────────────────────────────────────────────────────────
// Each gesture maps to a high-level user action. The compositor or WM decides
// what to *do* with each gesture — this module just detects them.
typedef enum {
    GESTURE_NONE = 0,        // No gesture detected yet

    // Single-finger gestures
    GESTURE_TAP,             // Quick tap → left click
    GESTURE_LONG_PRESS,      // Hold 500ms without moving → right click

    // Two-finger gestures
    GESTURE_TWO_FINGER_TAP,  // Two fingers tap together → right click
    GESTURE_PINCH_IN,        // Pinch fingers together → zoom out
    GESTURE_PINCH_OUT,       // Spread fingers apart → zoom in
    GESTURE_SCROLL,          // Two-finger drag → scroll content

    // Three-finger gestures
    GESTURE_SWIPE_UP,        // Three fingers up → Mission Control
    GESTURE_SWIPE_DOWN,      // Three fingers down → App Exposé

    // Four-finger gestures
    GESTURE_SWIPE_LEFT,      // Four fingers left → next Space
    GESTURE_SWIPE_RIGHT,     // Four fingers right → previous Space
} GestureType;

// ────────────────────────────────────────────────────────────────────────────
// Gesture callback
// ────────────────────────────────────────────────────────────────────────────
// When a gesture is recognized, this function pointer is called. The meaning
// of param1/param2 depends on the gesture type:
//   - TAP/LONG_PRESS: param1 = x position, param2 = y position
//   - PINCH_IN/OUT:   param1 = scale factor (1.0 = no change)
//   - SCROLL:         param2 = vertical scroll delta
//   - SWIPE_*:        param1 or param2 = swipe distance in pixels
typedef void (*GestureCallback)(GestureType type, float param1, float param2,
                                void *userdata);

// ────────────────────────────────────────────────────────────────────────────
// Initialization and shutdown
// ────────────────────────────────────────────────────────────────────────────

// Initialize XInput2 multitouch event handling on the given display/root.
// Returns true if at least one touch device was found and configured.
bool touch_init(Display *dpy, Window root);

// Release all resources and unregister event listeners.
void touch_shutdown(void);

// ────────────────────────────────────────────────────────────────────────────
// Event processing
// ────────────────────────────────────────────────────────────────────────────

// Feed an XEvent into the touch system. If it's an XInput2 touch event,
// this function handles it and returns true (caller should skip it).
// For non-touch events, returns false (caller processes normally).
bool touch_handle_event(Display *dpy, XEvent *ev);

// ────────────────────────────────────────────────────────────────────────────
// Gesture callback registration
// ────────────────────────────────────────────────────────────────────────────

// Set the function that gets called when a gesture is recognized.
// Pass NULL to disable gesture callbacks.
void touch_set_gesture_callback(GestureCallback cb, void *userdata);

// ────────────────────────────────────────────────────────────────────────────
// Display rotation
// ────────────────────────────────────────────────────────────────────────────

// Set the current display rotation. The compositor should re-setup its GL
// projection after calling this.
void touch_set_rotation(RotationAngle angle);

// Get the current display rotation angle.
RotationAngle touch_get_rotation(void);

// Transform a coordinate pair from physical screen space into logical
// (rotated) space. This is needed so that touch events land on the correct
// window even when the display is rotated.
//   screen_w / screen_h: the physical screen dimensions (before rotation)
void touch_transform_coords(float *x, float *y, int screen_w, int screen_h);

// ────────────────────────────────────────────────────────────────────────────
// Auto-rotation (accelerometer)
// ────────────────────────────────────────────────────────────────────────────

// Enable auto-rotation by connecting to iio-sensor-proxy over D-Bus.
// Returns true if the accelerometer is available and we started listening.
bool touch_enable_auto_rotation(void);

// Stop listening for orientation changes.
void touch_disable_auto_rotation(void);

// ────────────────────────────────────────────────────────────────────────────
// On-screen keyboard
// ────────────────────────────────────────────────────────────────────────────

// Request the on-screen keyboard to appear (e.g., when a text field is tapped).
void touch_request_osk(void);

// Dismiss the on-screen keyboard.
void touch_dismiss_osk(void);

// Check whether the on-screen keyboard is currently visible.
bool touch_osk_visible(void);

// ────────────────────────────────────────────────────────────────────────────
// Debug / introspection
// ────────────────────────────────────────────────────────────────────────────

// Get a pointer to the internal touch point array and the number of slots.
// Useful for drawing touch indicators (debug circles) on the compositor layer.
TouchPoint *touch_get_points(int *count);

#endif // MR_TOUCH_H
