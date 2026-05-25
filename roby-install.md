# Waybar Custom Build - Installation Guide

## Overview
This is Roby's customized Waybar fork with enhanced Hyprland workspace features:
- Workspace collapsing by project prefix
- Enhanced display with bracket notation
- Click-to-expand collapsed groups
- Transform workspace names for cleaner display

## Installation on Arch Linux

Installs as a proper Arch package (`waybar-custom`) via `makepkg` + `pacman`,
replacing the official `extra/waybar` cleanly. Pacman tracks every installed
file, so uninstall and roundtrip back to upstream are one-liners.

### Initial installation / update

```bash
cd /home/roby/Developer/opensource/Waybar
./scripts/install.sh
```

The script will:
1. Offer to clean up any stale files left by the old `local-install.sh` under `/usr/local`.
2. Fix root-owned files in `build/` from prior `sudo` builds (if any).
3. Run `makepkg -fsi` from `roby-arch-package/` — installs missing build deps,
   builds the package from the current working tree (committed + uncommitted
   changes), and installs via `pacman -U`. pacman automatically removes the
   official `waybar` package via the PKGBUILD's `conflicts`/`replaces`.

After install:
- Binary at `/usr/bin/waybar` (canonical Arch path — NOT `/usr/local/bin`)
- Config at `/etc/xdg/waybar/`
- systemd user unit at `/usr/lib/systemd/user/waybar.service`
- Man pages at `/usr/share/man/man5/`

### Restart Waybar

```bash
killall waybar
waybar &
# or
systemctl --user restart waybar
```

### Uninstallation

```bash
sudo pacman -R waybar-custom    # remove the custom build
sudo pacman -S waybar           # restore the official upstream package
```

## Configuration

### Enable Features

Add to `~/.config/waybar/config`:

```json
{
  "hyprland/workspaces": {
    "collapse-inactive-projects": true,
    "transform-workspace-names": true,
    "sort-by": "NAME",
    "format": "{name}"
  }
}
```

### Feature Flags

**`collapse-inactive-projects`** (default: `false`)
- Collapses inactive project workspaces into `[prefix]` buttons
- Click collapsed button to switch to first workspace
- Example: `.prj0 .prj1 .prj2` (inactive) → `[prj]`

**`transform-workspace-names`** (default: `false`)
- Transforms display names for cleaner look
- Single workspace: `.prj0` → `prj`
- Multiple workspaces: `.prj0 .prj1` → `[prj 0 1]`
- Works independently of collapsing

**Both enabled:**
- Inactive groups: `[prj]` (collapsed, clickable)
- Active groups: `[prj 0 1 2]` (expanded with brackets)
- Single workspaces: `web` (clean name, no brackets)

### CSS Styling

For tight spacing in grouped workspaces, add to `~/.config/waybar/style.css`:

```css
/* Tight spacing for grouped workspace elements */
.grouped {
  margin: 0;
  padding: 0 2px;
  min-width: 0;
}

.workspace-label {
  padding: 0 1px;
  margin: 0;
  opacity: 0.7;
}

/* Collapsed project button styling */
.collapsed-project {
  /* Add your styles */
}
```

## Helper Scripts

Two helper scripts are installed in `~/.local/bin/`:

### 1. Workspace Navigation
**`waybar-workspace-nav.sh`**

Navigates workspaces following Waybar's sort order.

```bash
# Usage
waybar-workspace-nav.sh next all         # Next workspace (any)
waybar-workspace-nav.sh prev all         # Previous workspace (any)
waybar-workspace-nav.sh next nonempty    # Next non-empty workspace
waybar-workspace-nav.sh prev nonempty    # Previous non-empty workspace
```

**Hyprland bindings:**
```conf
# All workspaces
bind = $mainMod, bracketright, exec, ~/.local/bin/waybar-workspace-nav.sh next all
bind = $mainMod, bracketleft, exec, ~/.local/bin/waybar-workspace-nav.sh prev all

# Only non-empty workspaces
bind = $mainMod SHIFT, bracketright, exec, ~/.local/bin/waybar-workspace-nav.sh next nonempty
bind = $mainMod SHIFT, bracketleft, exec, ~/.local/bin/waybar-workspace-nav.sh prev nonempty
```

### 2. Project Workspace Creator
**`waybar-workspace-create.sh`**

Creates new workspace for a project with smart number allocation.

```bash
# Usage
waybar-workspace-create.sh
```

Shows rofi menu with:
- List of existing projects (extracted from all workspaces)
- Current project pre-selected
- Can type new project name to create

Finds lowest unused number:
- Fills holes: `.prj0`, `.prj2` exist → creates `.prj1`
- Or max+1: `.prj0`, `.prj1` exist → creates `.prj2`
- Or starts at 0: no workspaces → creates `.prj0`

Auto-detects monitor suffix from current monitor's workspaces.

**Hyprland binding:**
```conf
bind = $mainMod SHIFT, N, exec, ~/.local/bin/waybar-workspace-create.sh
```

## Workspace Naming Convention

For features to work, use this naming pattern:
```
.{project}{number}{monitor}
```

Examples:
- `.prj0+` - project "prj", number 0, monitor with "+" suffix
- `.dev1-` - project "dev", number 1, monitor with "-" suffix
- `.web2|` - project "web", number 2, monitor with "|" suffix

**Pattern explanation:**
- `.` prefix (required) - identifies project workspaces
- Letters - project name (e.g., `prj`, `dev`, `web`)
- Digit - workspace number within project (0-9)
- Suffix - monitor identifier (your choice: `+`, `-`, `|`, `_`, etc.)

## Troubleshooting

### Waybar not found
```bash
which waybar
# Should show: /usr/bin/waybar
pacman -Qi waybar-custom    # confirm the custom build is the installed package
```
If `which waybar` still shows `/usr/local/bin/waybar`, you have leftover debris
from the old `local-install.sh`. Re-run `./scripts/install.sh` and accept the
cleanup prompt.

### Features not working
```bash
# Check config is valid JSON
jq . ~/.config/waybar/config

# Check debug logs
killall waybar
waybar -l debug 2>&1 | grep -i workspace
```

### Build fails
```bash
# Clean rebuild
rm -rf build
meson setup build
ninja -C build
```

### Workspaces not showing
- Check workspace names match pattern: `.{letters}{digit}{suffix}`
- Ensure workspaces have valid IDs (not workspace rules)
- Check waybar logs for "skipped" messages

## Files Modified

- `include/modules/hyprland/workspaces.hpp`
- `src/modules/hyprland/workspaces.cpp`
- `man/waybar-hyprland-workspaces.5.scd`

## Git Repository

Fork location: https://github.com/yourusername/Waybar

## Version Info

- Base: Waybar 0.14.0
- Custom features: H002 (collapsing) + H003 (enhanced display)
- Last updated: 2026-01-08

## Notes

- The repo-root `build/` directory is for dev iteration (`meson compile -C build && ./build/waybar`); `makepkg` builds in its own scratch dir under `roby-arch-package/src/` and does not touch `build/`.
- Config changes don't require rebuild, just restart waybar.
- Code changes require: `./scripts/install.sh` → restart waybar.
- Man pages installed at `/usr/share/man/man5/` (canonical Arch path).
