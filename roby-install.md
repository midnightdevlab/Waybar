# Waybar Custom Build - Installation Guide

## Overview
This is Roby's customized Waybar fork with enhanced Hyprland workspace features:
- Workspace collapsing by project prefix
- Enhanced display with bracket notation
- Click-to-expand collapsed groups
- Transform workspace names for cleaner display

## Installation on Arch Linux

### Initial Installation

1. **Remove official Waybar package**
   ```bash
   sudo pacman -R waybar
   ```
   This prevents conflicts and ensures no scripts hardcode the path.

2. **Build the custom version**
   ```bash
   cd /home/roby/Developer/opensource/Waybar
   meson setup build  # Only needed once
   ninja -C build
   ```

3. **Install to system**
   ```bash
   sudo ninja -C build install
   ```
   This installs to `/usr/local/bin/waybar` which takes precedence in PATH.

4. **Verify installation**
   ```bash
   which waybar         # Should show: /usr/local/bin/waybar
   waybar --version     # Should show: 0.14.0 (or current version)
   ```

5. **Restart Waybar**
   ```bash
   killall waybar
   waybar &
   # Or if using systemd:
   systemctl --user restart waybar
   ```

### Update Workflow

When you make changes to the code:

```bash
cd /home/roby/Developer/opensource/Waybar
git pull  # If pulling from remote
ninja -C build
sudo ninja -C build install
killall waybar
waybar &
```

### Uninstallation

To remove custom build and revert to official:

```bash
cd /home/roby/Developer/opensource/Waybar
sudo ninja -C build uninstall
sudo pacman -S waybar
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
# Should show: /usr/local/bin/waybar
# If not, check PATH includes /usr/local/bin before /usr/sbin
```

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

- Build directory must be kept for reinstalls: `/home/roby/Developer/opensource/Waybar/build/`
- Config changes don't require rebuild, just restart waybar
- Code changes require: rebuild → reinstall → restart
- Man pages are also installed to `/usr/local/share/man/man5/`
