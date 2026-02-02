# Understanding Layer-Shell and Why It Conflicts with GTK Popups

## What is Layer-Shell?

**Layer-shell** (`wlr-layer-shell-unstable-v1`) is a Wayland protocol that lets desktop shell components (panels, docks, backgrounds, notifications) place their surfaces on specific **layers** of the screen, independent of normal application windows.

### The Layer Stack (bottom to top)

```
┌─────────────────────────────────┐
│  OVERLAY LAYER                  │  ← Notifications, lock screens
├─────────────────────────────────┤
│  TOP LAYER                      │  ← Panels, bars (Waybar lives here)
├─────────────────────────────────┤
│  [Regular Application Windows]  │  ← Your browser, terminals, etc.
├─────────────────────────────────┤
│  BOTTOM LAYER                   │  ← Docks that shouldn't cover windows
├─────────────────────────────────┤
│  BACKGROUND LAYER               │  ← Wallpapers
└─────────────────────────────────┘
```

### Key Properties of Layer-Shell Surfaces

1. **Anchored to screen edges**: Can stick to top/bottom/left/right
2. **Exclusive zones**: Can reserve space (push windows away)
3. **Always visible**: Don't minimize or hide when switching workspaces
4. **Compositor-controlled**: Position/size managed by compositor, not client

## Why Waybar Uses Layer-Shell

```cpp
// In Waybar's initialization:
auto gtk_window = window.gobj();
gtk_layer_init_for_window(gtk_window);          // Convert to layer-shell
gtk_layer_set_layer(gtk_window, GTK_LAYER_SHELL_LAYER_TOP);  // Put in TOP layer
gtk_layer_set_anchor(gtk_window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);  // Anchor to top
gtk_layer_set_exclusive_zone(gtk_window, height);  // Reserve space
```

This makes Waybar behave like a traditional panel:
- Always visible on top
- Windows don't overlap it
- Stays anchored to screen edge

## The Problem: GTK Popups + Layer-Shell = Incompatible

### How Normal GTK Applications Work

```
Regular GTK App Window Hierarchy:

┌─────────────────────────────┐
│ GtkWindow (app window)      │  ← Normal application window
│  ┌───────────────────────┐  │
│  │ GtkButton             │  │
│  │  [Click me]           │  │
│  └───────────────────────┘  │
└─────────────────────────────┘
        │
        │ User hovers button
        ↓
┌─────────────────────────────┐
│ GtkPopover (child surface)  │  ← Child of app window
│  "This is a tooltip"        │  ← Positioned relative to parent
└─────────────────────────────┘
```

**Key point**: GTK Popover is a **child surface** that:
- Is positioned **relative to its parent window**
- Inherits coordinate space from parent
- Can only appear **within or near** parent bounds

### What Happens with Layer-Shell

```
Waybar (Layer-Shell) Hierarchy:

┌─────────────────────────────────────────────────────┐
│ Layer-Shell Surface (Waybar)                        │ ← In TOP layer
│ ┌─────┐ ┌─────┐ ┌─────┐                            │ ← Anchored to screen edge
│ │ WS1 │ │ WS2 │ │ WS3 │  [modules...]              │ ← Size controlled by compositor
│ └─────┘ └─────┘ └─────┘                            │
└─────────────────────────────────────────────────────┘
        │
        │ Try to create GtkPopover
        ↓
        ❌ FAILS! ❌

WHY:
- Layer-shell surface is NOT a normal GTK window
- Compositor doesn't understand GTK's child surface protocol
- GTK tries to create popover as child, but layer-shell protocol
  doesn't support traditional parent-child window relationships
```

### The Technical Reason

**Two different protocols trying to talk:**

1. **GTK side** (what Popover expects):
   - "I'm a child surface of this GTK window"
   - "Position me relative to my parent widget"
   - "Use XDG-shell popup protocol to create popup surface"

2. **Layer-shell side** (what Waybar actually is):
   - "I'm a layer-shell surface, not an XDG-shell surface"
   - "I don't have 'child surfaces' - only layer-shell surfaces exist in my world"
   - "My coordinate space is managed by compositor, not GTK"

```
┌───────────────────────────────────────────────────────┐
│                   Wayland Compositor                  │
│                                                       │
│  ┌─────────────────┐    ┌─────────────────────┐     │
│  │ Layer-Shell     │    │ XDG-Shell           │     │
│  │ Protocol        │    │ Protocol            │     │
│  │                 │    │                     │     │
│  │ • Waybar        │    │ • Normal apps       │     │
│  │ • Panels        │    │ • Their popups      │     │
│  │ • Backgrounds   │    │ • Tooltips          │     │
│  └─────────────────┘    └─────────────────────┘     │
│         ↑                       ↑                    │
│         │                       │                    │
│         └───────────┬───────────┘                    │
│                     │                                │
│            Can't mix these!                          │
└───────────────────────────────────────────────────────┘
```

## Why Can't We Just Use Low-Level GTK?

You might think: "Can't we just create a regular `Gtk::Window` as a popup and it will work?"

### Attempt 1: Create Regular Gtk::Window as Popup

```cpp
// Create a regular GTK window (not layer-shell)
auto popup = new Gtk::Window(Gtk::WINDOW_POPUP);
popup->show();
```

**Problem**: On Wayland, the **compositor** controls window positioning, not the client.

```
You try:                Compositor says:
popup->move(100, 200)   → "No! I decide where windows go"
                           (Wayland security model)

Result: Window appears centered or wherever compositor wants
```

### Why Wayland Doesn't Allow Client Positioning

**X11 (old system)**:
- Client: "Place my window at (x=100, y=200)"
- X Server: "OK" ✓
- **Problem**: Any app could position windows anywhere (security/UX issues)

**Wayland (new system)**:
- Client: "I want to show a window"
- Compositor: "I'll place it where I think it should go"
- Client: "But I need it at specific coordinates!"
- Compositor: "No. That's my job."

**Why this exists**: 
- Security (apps can't overlay fake password dialogs)
- Multi-monitor (compositor knows about all monitors)
- Consistency (apps can't break UI rules)

### Attempt 2: Use Hyprland IPC to Move Window

```cpp
// Create window
popup->show();

// Then tell Hyprland to move it via IPC
hyprctl dispatch movewindowpixel exact 100 200
```

**Problems we hit**:
1. **Timing**: Window might not exist yet when IPC command runs
2. **Monitor confusion**: Coordinates are monitor-relative, but which monitor?
3. **Windowrule conflicts**: Other rules (like `center`) override our positioning
4. **Focus stealing**: Moving window can steal focus and move cursor
5. **Fragile**: Specific to Hyprland, breaks on other compositors

### Attempt 3: Use Layer-Shell for Popup Too

```cpp
auto popup = new Gtk::Window();
gtk_layer_init_for_window(popup->gobj());  // Make it layer-shell
gtk_layer_set_layer(popup->gobj(), GTK_LAYER_SHELL_LAYER_OVERLAY);
```

**Problems**:
- **No anchoring**: Can't position relative to icon - only relative to screen edges
- **Breaks hover**: Layer-shell surfaces don't receive proper enter/leave events
- **Can't be child**: Layer-shell surfaces can't have parent-child relationships

## The Fundamental Mismatch

```
What We Want:
┌─────────────────────────────────────────────────┐
│ Waybar (TOP layer)                              │
│   [WS1] [WS2] [WS3]                            │
└─────────────────────────────────────────────────┘
      ↓
   Hover icon → popup appears below it, interactive

What Wayland/Layer-Shell Offers:
      Option A: Layer-shell popup
         - Can position at screen edges only
         - Breaks hover events
         
      Option B: Regular window + compositor control
         - Compositor decides position
         - Can't anchor to specific widget
         
      Option C: Use IPC hacks
         - Fragile, compositor-specific
         - Complex multi-monitor handling
         - Focus/cursor issues
```

## Why Other Apps Don't Do This

Looking at other Wayland bars/panels:
- **Waybar**: Uses simple tooltips (non-interactive, auto-hide)
- **AGS**: Hover tooltips OR click-to-show menus (separate choice)
- **Eww**: Simple tooltips, can't do widget-anchored popups
- **Polybar**: X11 only, doesn't face this issue

**The pattern**: Everyone accepts that **interactive hover tooltips on layer-shell = not feasible**.

## The Practical Solution

Accept the architectural limitation and work with it:

```
Hover = Information (standard tooltip, auto-hide)
Click = Action (focus window, or show menu)
```

This is what every other Wayland bar does, and it works reliably.

## Summary

1. **Layer-shell** puts surfaces in special compositor-managed layers
2. **GTK Popover** expects normal window parent-child relationships
3. **These two protocols are incompatible** - can't mix them
4. **Wayland doesn't allow client positioning** - compositor controls placement
5. **Workarounds exist but are fragile** - timing, monitor detection, focus issues
6. **Standard solution**: Simple tooltips (hover) + actions (click)

The architectural mismatch isn't a bug - it's by design for security and consistency. Fighting it leads to complex, fragile code that breaks in edge cases (as we experienced in H001 and H002).
