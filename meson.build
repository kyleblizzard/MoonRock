# MoonRock Compositor — Standalone Build
# Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
#
# Build:
#   meson setup builddir
#   ninja -C builddir
#
# Install:
#   ninja -C builddir install
#
# This produces:
#   - libmoonrock.so       — shared library for window managers to link against
#   - moonrock_api.h       — the only header a WM needs to include
#   - moonrock.pc          — pkg-config file for dependency discovery
#   - shaders/*           — GLSL shader files installed to datadir/moonrock/shaders

project('moonrock', 'c',
  version: '0.1.0',
  license: 'Proprietary',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'default_library=shared',
  ],
)

# ── Compiler setup ──────────────────────────────────────────────────────
cc = meson.get_compiler('c')

# _GNU_SOURCE is needed for M_PI and other POSIX extensions in C11 mode.
add_project_arguments('-D_GNU_SOURCE', language: 'c')

# ── Dependencies ────────────────────────────────────────────────────────
# Core X11
x11_dep        = dependency('x11')
xcomposite_dep = dependency('xcomposite')
xdamage_dep    = dependency('xdamage')
xfixes_dep     = dependency('xfixes')
xext_dep       = dependency('xext')
xrender_dep    = dependency('xrender')
xrandr_dep     = dependency('xrandr')

# XInput2 (multitouch gestures) and XTest (synthetic events)
xi_dep    = dependency('xi')
xtst_dep  = dependency('xtst')

# OpenGL + GLX
gl_dep = dependency('gl')

# Math library (-lm)
math_dep = cc.find_library('m', required: true)

# Dynamic loading (-ldl, for plugin system)
dl_dep = cc.find_library('dl', required: true)

# XShape (input shapes for shadow click-through)
# xext covers this on most systems

# ── Source files ────────────────────────────────────────────────────────
moonrock_sources = [
  'src/moonrock.c',
  'src/moonrock_api.c',
  'src/moonrock_shaders.c',
  'src/moonrock_anim.c',
  'src/moonrock_mission_control.c',
  'src/moonrock_touch.c',
  'src/moonrock_display.c',
  'src/moonrock_color.c',
  'src/moonrock_plugin.c',
  'src/moonrock_robust.c',
  'src/wm_compat.c',
]

# ── Include directories ────────────────────────────────────────────────
# The include/ directory contains all MoonRock headers (public + internal).
moonrock_inc = include_directories('include')

# ── Build the shared library ───────────────────────────────────────────
moonrock_lib = library('moonrock',
  moonrock_sources,
  include_directories: moonrock_inc,
  dependencies: [
    x11_dep,
    xcomposite_dep,
    xdamage_dep,
    xfixes_dep,
    xext_dep,
    xrender_dep,
    xrandr_dep,
    xi_dep,
    xtst_dep,
    gl_dep,
    math_dep,
    dl_dep,
  ],
  install: true,
)

# ── Install public header ──────────────────────────────────────────────
# Only moonrock_api.h is the public API. Internal headers stay in the
# source tree but aren't installed — they're implementation details.
install_headers('include/moonrock_api.h')

# ── Install GLSL shaders ──────────────────────────────────────────────
# Shaders are installed to $prefix/share/moonrock/shaders/ so the library
# can find them at runtime. The embedded fallback shaders in moonrock_shaders.c
# mean these aren't strictly required, but having them on disk allows
# users to edit shaders without recompiling.
install_data(
  'shaders/basic.vert',
  'shaders/basic.frag',
  'shaders/blur.frag',
  'shaders/shadow.frag',
  install_dir: get_option('datadir') / 'moonrock' / 'shaders',
)

# ── pkg-config ─────────────────────────────────────────────────────────
# Generate a .pc file so other projects can find MoonRock with:
#   dependency('moonrock')  (in meson)
#   pkg-config --cflags --libs moonrock  (in makefiles)
pkg = import('pkgconfig')
pkg.generate(
  moonrock_lib,
  name: 'moonrock',
  description: 'MoonRock Compositor — X11 OpenGL compositing engine',
  version: meson.project_version(),
)

# ── Declare dependency for subproject use ──────────────────────────────
# If another meson project includes MoonRock as a subproject, they can use:
#   moonrock_dep = subproject('moonrock').get_variable('moonrock_dep')
moonrock_dep = declare_dependency(
  link_with: moonrock_lib,
  include_directories: moonrock_inc,
)
