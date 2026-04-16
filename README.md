# MoonRock Compositor

MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
www.blizzard.show/moonrock/

Licensed under the BSD 3-Clause License. See [LICENSE](LICENSE).

---

MoonRock is a standalone OpenGL compositor for X11 on Linux. The name is a nod to Apple's Quartz Compositor -- same idea, different stack.

I built this because I needed it. AuraOS is a pixel-perfect Mac OS X Snow Leopard desktop environment I'm building for the Lenovo Legion Go S (a gaming handheld running Nobara Linux). Getting the compositing right is everything. Window shadows, transparency, animations -- if you get those wrong, the whole illusion falls apart. I looked at picom and other off-the-shelf compositors, and none of them gave me the level of control I needed. Picom is great for what it is, but I couldn't get real Gaussian blur drop shadows cached per window, or frosted glass blur-behind for panels, or a genie minimize animation that actually looks right. So I wrote my own.

MoonRock targets XLibre, an actively maintained fork of X11. No Wayland. I know that's a hot take in 2026, but Wayland still doesn't give me what I need for this project, and XLibre is solid.

## How It Works

The rendering pipeline is straightforward once you see it laid out.

MoonRock uses GLX with `texture_from_pixmap` to grab window contents as OpenGL textures with zero-copy. No readback, no extra buffer copies. The window's pixmap goes straight to a GL texture and gets composited on the GPU.

Every frame, MoonRock walks the window stack from back to front. Each window gets its shadow rendered first (a Gaussian blur pass that's computed once and cached until the window resizes), then the window texture itself. For panels and overlays that need frosted glass, there's a blur-behind pass that samples what's already been drawn to the backbuffer and runs it through a separable Gaussian kernel before the panel gets composited on top.

Rendering is damage-based. If nothing changed, nothing gets drawn. X damage events tell MoonRock which windows have new content, and only those regions get recomposited. The whole thing is render-on-demand, not a 60fps spin loop burning battery on a handheld.

For fullscreen games and video, MoonRock supports direct scanout bypass. If a single window covers the entire screen and nothing else needs compositing, the window's buffer goes straight to the display with zero compositor overhead. On a gaming handheld, that matters.

## Features

**Compositing core.** OpenGL via GLX, texture_from_pixmap zero-copy, damage-based render-on-demand. About 11,000 lines of C.

**Shadows.** Real Gaussian blur drop shadows, not the box-shadow approximations you see in most compositors. Computed once per window geometry and cached until the window resizes.

**Frosted glass.** Blur-behind support for panels and overlays. The menubar and dock in AuraOS use this to get that translucent material look Snow Leopard had.

**Animations.** Genie minimize and restore, smooth window open/close, workspace transitions. Eight easing curves (ease-in, ease-out, ease-in-out, spring, bounce, etc.) so animations feel physical, not robotic.

**Mission Control.** Full Expose and Spaces implementation. All windows tile out for overview, multiple virtual desktops with smooth transitions between them. Built it because AuraOS needs it to feel like a real Mac desktop.

**Multitouch.** The Legion Go has a touchscreen, so MoonRock handles multitouch input and gesture recognition natively. Pinch, swipe, three-finger drag -- all wired into compositor actions.

**Display handling.** Rotation, VRR (variable refresh rate), HDR passthrough, and fractional DPI scaling. The Legion Go's display is 1920x1200 in landscape, 1200x1920 in portrait mode, and MoonRock handles rotation without breaking a sweat.

**Direct scanout.** Fullscreen bypass for gaming. Zero compositor overhead when you're actually playing something.

**Plugin API.** Theme engine and extension hooks. The compositor's visual behavior is configurable without recompiling.

## Building

MoonRock uses Meson and Ninja. Standard build:

```bash
cd MoonRock
meson setup build
meson compile -C build
```

Dependencies: OpenGL, GLX, Xlib, XComposite, XDamage, XFixes, XRender, XInput2, Cairo, Pango. On Fedora/Nobara:

```bash
sudo dnf install mesa-libGL-devel libX11-devel libXcomposite-devel \
  libXdamage-devel libXfixes-devel libXrender-devel libXi-devel \
  cairo-devel pango-devel meson ninja-build
```

## Why Not Wayland

I get asked this a lot. XLibre gives me direct access to every window's pixmap, full control over the compositing pipeline, and a mature ecosystem of tools for debugging and profiling. MoonRock is a compositor that sits on top of a display server, not a display server itself. That separation is a feature. I don't need to reimplement input handling, clipboard, drag-and-drop, and session management from scratch. X does that. I just do the pretty part.

## License

All Rights Reserved. Copyright (c) 2026 Kyle Blizzard.

This code is publicly visible for portfolio purposes only. See the [LICENSE](LICENSE) file for details.
