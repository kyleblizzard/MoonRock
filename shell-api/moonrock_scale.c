// Copyright (c) 2026 Kyle Blizzard
// SPDX-License-Identifier: BSD-3-Clause
//
// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
// www.blizzard.show/moonrock/
// ============================================================================
//  MoonRock -> Shell scale bridge — subscriber helper
// ============================================================================
//
// Implementation of the public reader API defined in moonrock_scale.h.
// Lives in moonrock/shell-api/ alongside the header so that any shell
// component — menubar, dock, desktop, systemcontrol — can link a single
// copy rather than rolling its own parser for each.
//
// Thread model: single-threaded, called from the owning X11 process's main
// thread only. There is no locking; subscribers read on startup and on
// PropertyNotify, both of which are main-loop events.
// ============================================================================

#include "moonrock_scale.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>


// Cached atom, interned once and reused. One atom per process — X atoms are
// per-display, but every shell component opens exactly one Display, so a
// static here is fine.
static Atom g_atom = None;


// Intern the atom on demand. XInternAtom with only_if_exists=False creates
// the atom if it doesn't already exist, which lets subscribers start up
// before the publisher without error.
Atom moonrock_scale_atom(Display *dpy)
{
    if (g_atom == None && dpy != NULL) {
        g_atom = XInternAtom(dpy, MOONROCK_SCALE_ATOM_NAME, False);
    }
    return g_atom;
}


bool moonrock_scale_init(Display *dpy)
{
    if (!dpy) return false;

    // Intern the atom so subscribers can compare against ev.xproperty.atom.
    (void)moonrock_scale_atom(dpy);

    Window root = DefaultRootWindow(dpy);

    // Additive event-mask update — never clobber whatever the caller has
    // already selected on the root window. Many shell components already
    // watch root for _NET_ACTIVE_WINDOW or _NET_WORKAREA; we only add
    // PropertyChangeMask on top of whatever bits they had.
    XWindowAttributes wa;
    if (XGetWindowAttributes(dpy, root, &wa)) {
        XSelectInput(dpy, root, wa.your_event_mask | PropertyChangeMask);
    } else {
        XSelectInput(dpy, root, PropertyChangeMask);
    }
    return true;
}


// Copy at most `max_len - 1` bytes from [src, src+src_len) into `dst` and
// NUL-terminate. Helper for the line parser so we don't overflow the
// fixed-size on-stack line buffer.
static void copy_line(char *dst, size_t max_len,
                      const char *src, size_t src_len)
{
    if (max_len == 0) return;
    if (src_len >= max_len) src_len = max_len - 1;
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
}


bool moonrock_scale_refresh(Display *dpy, MoonRockScaleTable *out)
{
    if (!dpy || !out) return false;

    // Reset the output table up front. Even on failure the caller gets a
    // well-defined "valid=false, count=0" state.
    memset(out, 0, sizeof(*out));

    Atom prop = moonrock_scale_atom(dpy);
    if (prop == None) return false;

    Atom actual_type = None;
    int  actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;

    Window root = DefaultRootWindow(dpy);

    // Ask for up to 4KB worth of the property value. The publisher caps its
    // output at this size too — MOONROCK_SCALE_MAX_OUTPUTS * ~80 bytes per
    // line fits comfortably. long_length is in 32-bit words, so 1024 words
    // = 4096 bytes.
    int status = XGetWindowProperty(
        dpy, root, prop,
        0,          // long_offset (start at byte 0)
        1024,       // long_length in 32-bit words
        False,      // don't delete on read
        AnyPropertyType,
        &actual_type, &actual_format,
        &nitems, &bytes_after, &data);

    if (status != Success || !data || nitems == 0) {
        if (data) XFree(data);
        return false;
    }

    // Parse line-by-line. We walk the buffer directly rather than copying
    // to a NUL-terminated heap string — the data returned by Xlib is an
    // opaque byte blob and may or may not be NUL-terminated.
    const char *s   = (const char *)data;
    const char *end = s + nitems;

    while (s < end && out->count < MOONROCK_SCALE_MAX_OUTPUTS) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        size_t llen = nl ? (size_t)(nl - s) : (size_t)(end - s);

        char line[256];
        copy_line(line, sizeof(line), s, llen);

        char  namebuf[MOONROCK_SCALE_NAME_MAX];
        int   x = 0, y = 0, w = 0, h = 0;
        float scale = 1.0f;

        // The %63s matches MOONROCK_SCALE_NAME_MAX - 1. sscanf with a
        // width specifier guarantees we never overflow namebuf.
        if (sscanf(line, "%63s %d %d %d %d %f",
                   namebuf, &x, &y, &w, &h, &scale) == 6) {

            MoonRockOutputScale *o = &out->outputs[out->count];
            strncpy(o->name, namebuf, sizeof(o->name) - 1);
            o->name[sizeof(o->name) - 1] = '\0';
            o->x      = x;
            o->y      = y;
            o->width  = w;
            o->height = h;
            // Clamp obviously-broken scales to 1.0. The publisher already
            // enforces 0.5–4.0 on its side, but defense in depth.
            o->scale  = (scale >= 0.25f && scale <= 8.0f) ? scale : 1.0f;
            out->count++;
        }

        if (!nl) break;
        s = nl + 1;
    }

    XFree(data);

    out->valid = (out->count > 0);
    return out->valid;
}


float moonrock_scale_for_point(const MoonRockScaleTable *t, int x, int y)
{
    if (!t || !t->valid) return 1.0f;

    for (int i = 0; i < t->count; i++) {
        const MoonRockOutputScale *o = &t->outputs[i];
        if (x >= o->x && x < o->x + o->width &&
            y >= o->y && y < o->y + o->height) {
            return o->scale > 0.0f ? o->scale : 1.0f;
        }
    }
    return 1.0f;
}


float moonrock_scale_for_name(const MoonRockScaleTable *t, const char *name)
{
    if (!t || !t->valid || !name) return 1.0f;

    for (int i = 0; i < t->count; i++) {
        if (strcmp(t->outputs[i].name, name) == 0) {
            return t->outputs[i].scale > 0.0f ? t->outputs[i].scale : 1.0f;
        }
    }
    return 1.0f;
}
