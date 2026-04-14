# Crystal Compositor — Standalone Build
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
#   - libcrystal.so       — shared library for window managers to link against
#   - crystal_api.h       — the only header a WM needs to include
#   - crystal.pc          — pkg-config file for dependency discovery
#   - shaders/*           — GLSL shader files installed to datadir/crystal/shaders

project('crystal', 'c',
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
crystal_sources = [
  'src/crystal.c',
  'src/crystal_api.c',
  'src/crystal_shaders.c',
  'src/crystal_anim.c',
  'src/crystal_mission_control.c',
  'src/crystal_touch.c',
  'src/crystal_display.c',
  'src/crystal_color.c',
  'src/crystal_plugin.c',
  'src/crystal_robust.c',
  'src/wm_compat.c',
]

# ── Include directories ────────────────────────────────────────────────
# The include/ directory contains all Crystal headers (public + internal).
crystal_inc = include_directories('include')

# ── Build the shared library ───────────────────────────────────────────
crystal_lib = library('crystal',
  crystal_sources,
  include_directories: crystal_inc,
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
# Only crystal_api.h is the public API. Internal headers stay in the
# source tree but aren't installed — they're implementation details.
install_headers('include/crystal_api.h')

# ── Install GLSL shaders ──────────────────────────────────────────────
# Shaders are installed to $prefix/share/crystal/shaders/ so the library
# can find them at runtime. The embedded fallback shaders in crystal_shaders.c
# mean these aren't strictly required, but having them on disk allows
# users to edit shaders without recompiling.
install_data(
  'shaders/basic.vert',
  'shaders/basic.frag',
  'shaders/blur.frag',
  'shaders/shadow.frag',
  install_dir: get_option('datadir') / 'crystal' / 'shaders',
)

# ── pkg-config ─────────────────────────────────────────────────────────
# Generate a .pc file so other projects can find Crystal with:
#   dependency('crystal')  (in meson)
#   pkg-config --cflags --libs crystal  (in makefiles)
pkg = import('pkgconfig')
pkg.generate(
  crystal_lib,
  name: 'crystal',
  description: 'Crystal Compositor — X11 OpenGL compositing engine',
  version: meson.project_version(),
)

# ── Declare dependency for subproject use ──────────────────────────────
# If another meson project includes Crystal as a subproject, they can use:
#   crystal_dep = subproject('crystal').get_variable('crystal_dep')
crystal_dep = declare_dependency(
  link_with: crystal_lib,
  include_directories: crystal_inc,
)
