waybar-hyprland-fancy-workspaces(5)

# NAME

waybar - hyprland fancy workspaces module (custom enhanced version)

# DESCRIPTION

The *fancy-workspaces* module is a custom enhanced version of the hyprland workspaces module with additional features including project-based grouping, workspace collapsing, window icons, and smart click handlers.

# CONFIGURATION

Addressed by *hyprland/fancy-workspaces*

*format*: ++
	typeof: string ++
	default: {id} ++
	The format, how information should be displayed.

*format-icons*: ++
	typeof: array ++
	Based on the workspace ID and state, the corresponding icon gets selected. See *icons*.

*window-rewrite*: ++
	typeof: object ++
	Regex rules to map window class to an icon or preferred method of representation for a workspace's window.
	Keys are the rules, while the values are the methods of representation. Values may use the placeholders {class} and {title} to use the window's original class and/or title respectively.
	Rules may specify `class<...>`, `title<...>`, or both in order to fine-tune the matching.
	You may assign an empty value to a rule to have it ignored from generating any representation in workspaces. ++
This setting is ignored if *workspace-taskbar.enable* is set to true.

*window-rewrite-default*: ++
	typeof: string ++
	default: "?" ++
	The default method of representation for a workspace's window. This will be used for windows whose classes do not match any of the rules in *window-rewrite*. ++
	This setting is ignored if *workspace-taskbar.enable* is set to true.

*format-window-separator*: ++
	typeof: string ++
	default: " " ++
	The separator to be used between windows in a workspace. ++
	This setting is ignored if *workspace-taskbar.enable* is set to true.

*workspace-taskbar*: ++
	typeof: object ++
	Contains settings for the workspace taskbar, an alternative mode for the workspaces module which displays the window icons as images instead of text.
	
		*enable*: ++
	typeof: bool ++
	default: false ++
	Enables the workspace taskbar mode.

		*update-active-window*: ++
	typeof: bool ++
	default: false ++
	If true, the active/focused window will have an 'active' class. Could cause higher CPU usage due to more frequent redraws.

		*reverse-direction*: ++
	typeof: bool ++
	default: false ++
	If true, the taskbar windows will be added in reverse order (right to left if orientation is horizontal, bottom to top if vertical).

		*active-window-position*: ++
	typeof: "none" | "first" | "last" ++
	default: "none" ++
	If set to "first", the active window will be moved at the beginning of the taskbar. If set to "last", it will be moved at the end. It will only work if *update-active-window* is set to true.

		*format*: ++
	typeof: string ++
	default: {icon} ++
	Format to use for each window in the workspace taskbar. Available placeholders are {icon} and {title}.

		*icon-size*: ++
	typeof: int ++
	default: 16 ++
	Size of the icons in the workspace taskbar.

		*icon-theme*: ++
	typeof: string | array ++
	default: [] ++
	Icon theme to use for the workspace taskbar. If an array is provided, the first theme that is found for a given icon will be used. If no theme is found (or the array is empty), the default icon theme is used.

		*orientation*: ++
	typeof: "horizontal" | "vertical" ++
	default: horizontal ++
	Direction in which the workspace taskbar is displayed.

		*ignore-list*: ++
	typeof: array ++
	default: [] ++
	Regex patterns to match against window class or window title. If a window's class OR title matches any of the patterns, it will not be shown.

		*on-click-window*: ++
	typeof: string ++
	default: "" ++
	Command to run when a window is clicked. Available placeholders are: ++
	  - {address} Hyprland address of the clicked window. ++
	  - {button} Pressed button number, see https://api.gtkd.org/gdk.c.types.GdkEventButton.button.html. ++
	See https://github.com/Alexays/Waybar/wiki/Module:-Hyprland#workspace-taskbars-example for a full example.

*show-special*: ++
	typeof: bool ++
	default: false ++
	If set to true, special workspaces will be shown.

*special-visible-only*: ++
	typeof: bool ++
	default: false ++
	If this and show-special are to true, special workspaces will be shown only if visible.

*persistent-only*: ++
	typeof: bool ++
	default: false ++
	If set to true, only persistent workspaces will be shown on bar.

*all-outputs*: ++
	typeof: bool ++
	default: false ++
	If set to false workspaces group will be shown only in assigned output. Otherwise, all workspace groups are shown.

*active-only*: ++
	typeof: bool ++
	default: false ++
	If set to true, only the active workspace will be shown.

*move-to-monitor*: ++
	typeof: bool ++
	default: false ++
	If set to true, open the workspace on the current monitor when clicking on a workspace button.
	Otherwise, the workspace will open on the monitor where it was previously assigned.
	Analog to using `focusworkspaceoncurrentmonitor` dispatcher instead of `workspace` in Hyprland.

*ignore-workspaces*: ++
	typeof: array ++
	default: [] ++
	Regexes to match against workspaces names. If there's a match, the workspace will not be shown. This takes precedence over *show-special*, *all-outputs*, and *active-only*.

*sort-by*: ++
	typeof: string ++
	default: "default" ++
	If set to number, workspaces will sort by number.
	If set to name, workspaces will sort by name.
	If set to id, workspaces will sort by id.
	If set to special-centered, workspaces will sort by default with special workspaces in the center.
	If none of those, workspaces will sort with default behavior.

*collapse-inactive-projects*: ++
	typeof: bool ++
	default: false ++
	If enabled, workspaces from inactive projects will be collapsed into a single representative button showing [.prefix]. Workspaces must follow the naming pattern: .{prefix}{num}{monitor} (e.g., .prj0+, .dev1-, .web2|). Projects are identified by the letter prefix before the first digit. For example, .prj0+ and .prj1+ belong to project ".prj", while .dev0+ belongs to project ".dev". When a workspace from a project is active, all workspaces in that project are shown expanded. Inactive projects with multiple workspaces are shown as [.prefix]. Single workspaces are never collapsed. Clicking a collapsed group switches to the first workspace in that group.

*transform-workspace-names*: ++
	typeof: bool ++
	default: false ++
	If enabled, workspace names are transformed for cleaner display. Single workspaces show only the project name (e.g., .prj0 → prj). Multiple workspaces in a project are shown with bracket notation (e.g., .prj0 .prj1 .prj2 → [prj 0 1 2]) where only the numbers are clickable workspace buttons. Works independently of collapse-inactive-projects, allowing name transformation without collapsing.

*expand*: ++
	typeof: bool ++
	default: false ++
	Enables this module to consume all left over space dynamically.

# FORMAT REPLACEMENTS

*{id}*: id of workspace assigned by compositor

*{name}*: workspace name assigned by compositor

*{icon}*: Icon, as defined in *format-icons*.

# ICONS

Additional to workspace name matching, the following *format-icons* can be set.

- *default*: Will be shown, when no string match is found and none of the below conditions have defined icons.
- *active*: Will be shown, when workspace is active
- *special*: Will be shown on non-active special workspaces
- *empty*: Will be shown on non-active, non-special empty persistent workspaces
- *visible*: Will be shown on workspaces that are visible but not active. For example: this is useful if you want your visible workspaces on other monitors to have the same look as active.
- *persistent*: Will be shown on non-empty persistent workspaces

# EXAMPLES

```
"hyprland/workspaces": {
	"format": "{name}: {icon}",
	"format-icons": {
		"1": "",
		"2": "",
		"3": "",
		"4": "",
		"5": "",
		"active": "",
		"default": ""
	},
	"persistent-workspaces": {
		"*": 5, // 5 workspaces by default on every monitor
		"HDMI-A-1": 3 // but only three on HDMI-A-1
	}
}
```

```
"hyprland/workspaces": {
	"format": "{name}: {icon}",
	"format-icons": {
		"1": "",
		"2": "",
		"3": "",
		"4": "",
		"5": "",
		"active": "",
		"default": ""
	},
	"persistent-workspaces": {
		"*": [ 2,3,4,5 ], // 2-5 on every monitor
		"HDMI-A-1": [ 1 ] // but only workspace 1 on HDMI-A-1
	}
}
```

```
"hyprland/workspaces": {
	"format": "{name}\n{windows}",
	"format-window-separator": "\n",
	"window-rewrite-default": "",
	"window-rewrite": {
		"title<.*youtube.*>": "", // Windows whose titles contain "youtube"
		"class<firefox>": "", // Windows whose classes are "firefox"
		"class<firefox> title<.*github.*>": "", // Windows whose class is "firefox" and title contains "github". Note that "class" always comes first.
		"foot": "", // Windows that contain "foot" in either class or title. For optimization reasons, it will only match against a title if at least one other window explicitly matches against a title.
		"code": "󰨞",
		"title<.* - (.*) - VSCodium>": "codium $1"  // captures part of the window title and formats it into output
	}
}
```

```
"hyprland/workspaces": {
	// Formatting omitted for brevity
	"ignore-workspaces": [
		"(special:)?chrome-sharing-indicator"
	]
}
```

# Style

- *#workspaces*
- *#workspaces button*
- *#workspaces button.active*
- *#workspaces button.empty*
- *#workspaces button.visible*
- *#workspaces button.persistent*
- *#workspaces button.special*
- *#workspaces button.urgent*
- *#workspaces button.hosting-monitor* (gets applied if workspace-monitor == waybar-monitor)
- *#workspaces .workspace-label*
- *#workspaces .taskbar-window* (each window in the taskbar, only if 'workspace-taskbar.enable' is true)
- *#workspaces .taskbar-window.active* (applied to the focused window, only if 'workspace-taskbar.update-active-window' is true)
