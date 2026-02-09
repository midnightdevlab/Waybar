#!/bin/bash
# Workspace navigation script - sorts by NAME (alphabetical)
# Usage: waybar-workspace-nav.sh next|prev [nonempty]

DIRECTION="$1"
FILTER="${2:-all}"  # "nonempty" or "all" (default)

# Get current active workspace
ACTIVE_WS=$(hyprctl activeworkspace -j | jq -r '.name')
if [ -z "$ACTIVE_WS" ]; then
    exit 1
fi

# Get current monitor
ACTIVE_MONITOR=$(hyprctl activeworkspace -j | jq -r '.monitor')

# Get workspaces on current monitor
if [ "$FILTER" = "nonempty" ]; then
    # Filter to only workspaces with windows
    WORKSPACES=$(hyprctl workspaces -j | jq -r --arg mon "$ACTIVE_MONITOR" '.[] | select(.monitor == $mon and .windows > 0) | .name')
else
    # All workspaces
    WORKSPACES=$(hyprctl workspaces -j | jq -r --arg mon "$ACTIVE_MONITOR" '.[] | select(.monitor == $mon) | .name')
fi

# Exit if no workspaces found
if [ -z "$WORKSPACES" ]; then
    exit 1
fi

# Sort alphabetically by name
SORTED=$(echo "$WORKSPACES" | sort)

# Convert to array
mapfile -t WS_ARRAY <<< "$SORTED"

# Exit if no workspaces after filtering
if [ ${#WS_ARRAY[@]} -eq 0 ]; then
    exit 1
fi

# Find current workspace index
CURRENT_INDEX=-1
for i in "${!WS_ARRAY[@]}"; do
    if [ "${WS_ARRAY[$i]}" = "$ACTIVE_WS" ]; then
        CURRENT_INDEX=$i
        break
    fi
done

# If current workspace not in filtered list (e.g., it's empty but we're filtering nonempty)
# start from beginning or end depending on direction
if [ $CURRENT_INDEX -eq -1 ]; then
    if [ "$DIRECTION" = "next" ]; then
        CURRENT_INDEX=-1  # Will become 0 after increment
    else
        CURRENT_INDEX=${#WS_ARRAY[@]}  # Will become last after decrement
    fi
fi

# Calculate next/prev index with wraparound
TOTAL=${#WS_ARRAY[@]}
if [ "$DIRECTION" = "next" ]; then
    NEW_INDEX=$(( (CURRENT_INDEX + 1) % TOTAL ))
elif [ "$DIRECTION" = "prev" ]; then
    NEW_INDEX=$(( (CURRENT_INDEX - 1 + TOTAL) % TOTAL ))
else
    exit 1
fi

TARGET_WS="${WS_ARRAY[$NEW_INDEX]}"

# Switch to workspace
hyprctl dispatch workspace "name:$TARGET_WS"
