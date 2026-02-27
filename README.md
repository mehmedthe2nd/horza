# Horza

Vibecoded project disclaimer: Horza is built fast and iteratively; expect rapid changes and occasional rough edges between releases.

## What It Is

Horza is a workspace overview plugin for Hyprland.

What you get:
- a fast workspace overview with live/cached workspace cards
- smooth workspace transit (`horza:workspace`)
- drag-and-drop window moves between workspace cards
- configurable background blur/tint, titles, and card styling

Goal:
- GNOME-like overview feel, but lightweight

## Animation Tuning

Horza follows your Hyprland animation config.

If you want it snappier or smoother, tune your Hyprland animation block (especially `windowsMove` speed/curve).

## Install

Build:
```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j"$(nproc)"
```

Install:
```bash
cd build
make install
```

Default install path with this setup:
- `$HOME/.local/lib/libhorza.so`

Load plugin:
```bash
hyprctl plugin load "$HOME/.local/lib/libhorza.so"
```

Reload plugin after rebuild:
```bash
hyprctl plugin unload "$HOME/.local/lib/libhorza.so"
hyprctl plugin load "$HOME/.local/lib/libhorza.so"
```

## Before You Build

Horza is a Hyprland plugin, so version matching matters.

Compatibility rule:
- build Horza against the same Hyprland build you are currently running
- whenever Hyprland updates, rebuild Horza

If this is not matched, Hyprland will reject the plugin at load time.

Requirements:
- `cmake` 3.19+
- `pkg-config`
- C++ compiler with C++23 support
- Hyprland development package (`hyprland.pc`)
- `pixman-1` development package
- `libdrm` development package

Quick check:
```bash
pkg-config --modversion hyprland pixman-1 libdrm
```

If this command fails for any package, install that package's development headers first.

## Config

Put options in your `hyprland.conf` inside `plugin { horza { ... } }`.

Example:
```ini
plugin {
  horza {
    preset = custom

    capture_scale = 0.96
    display_scale = 0.70
    overview_gap = 16.0
    inactive_tile_size_percent = 85.0

    persistent_cache = true
    cache_ttl_ms = 1500.0
    cache_max_entries = 96
    capture_budget_ms = 4.0
    max_captures_per_frame = 1
    live_preview_fps = 6.0
    live_preview_radius = 2
    prewarm_all = false

    background_source = black
    background_blur_radius = 3.0
    background_blur_passes = 1
    background_blur_spread = 1.0
    background_blur_strength = 1.0
    background_tint = 0.35

    card_shadow = true
    card_shadow_mode = fast
    card_shadow_texture = ""
    card_shadow_alpha = 0.16
    card_shadow_size = 14.0
    card_shadow_offset_y = 8.0

    show_window_titles = false
    title_font_size = 14
    title_font_family = "Inter"
    title_background_alpha = 0.35

    freeze_animations_in_overview = true
    esc_only = true

    async_close_handoff = false
    async_close_fade_start = 0.88
    async_close_fade_curve = ease_out
    async_close_min_alpha = 0.0
    close_drop_delay_ms = 100.0

    drag_hover_jump_delay_ms = 1000.0

    orientation = horizontal
    center_offset = 0.0
    corner_radius = 5
  }
}
```

Use `true` or `false` for all boolean options.
