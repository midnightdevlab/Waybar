#!/bin/bash
# Create a new workspace for a project, finding the lowest unused number
# Usage: waybar-workspace-create.sh [PROJECT]
# If PROJECT is provided, skips rofi and uses that project directly

# Get current active workspace to extract current project
ACTIVE_WS=$(hyprctl activeworkspace -j | jq -r '.name')
CURRENT_MONITOR=$(hyprctl monitors -j | jq -r '.[] | select(.focused == true) | .name')

# Extract current project from active workspace (remove leading dot and number/suffix)
CURRENT_PROJECT=""
if [[ "$ACTIVE_WS" =~ ^\.(([a-zA-Z]+)[0-9]+) ]]; then
    CURRENT_PROJECT="${BASH_REMATCH[2]}"
fi

# Check if project was provided as parameter
if [ -n "$1" ]; then
    SELECTED_PROJECT="$1"
else
    # Get all workspaces and extract unique project prefixes
    ALL_WORKSPACES=$(hyprctl workspaces -j | jq -r '.[].name')
    PROJECTS=$(echo "$ALL_WORKSPACES" | grep -oP '^\.\K[a-zA-Z]+(?=[0-9]+)' | sort -u)

    # Show rofi to select/create project
    SELECTED_PROJECT=$(echo "$PROJECTS" | rofi -dmenu -p "Project" -select "$CURRENT_PROJECT" -theme-str 'window {width: 400px;} listview {lines: 10;}')

    # Exit if cancelled
    if [ -z "$SELECTED_PROJECT" ]; then
        exit 0
    fi
fi

# Get all workspaces for this project
ALL_WORKSPACES=$(hyprctl workspaces -j | jq -r '.[].name')
PROJECT_WORKSPACES=$(echo "$ALL_WORKSPACES" | grep -P "^\.${SELECTED_PROJECT}[0-9]+")

# Extract numbers from project workspaces
if [ -z "$PROJECT_WORKSPACES" ]; then
    # No workspaces for this project, start with 0
    NEW_NUMBER=0
else
    # Extract all numbers and sort them
    NUMBERS=$(echo "$PROJECT_WORKSPACES" | grep -oP "(?<=^\.${SELECTED_PROJECT})[0-9]+" | sort -n)
    
    # Find the lowest unused number (check for holes or use max+1)
    NEW_NUMBER=0
    while IFS= read -r num; do
        if [ "$num" -eq "$NEW_NUMBER" ]; then
            NEW_NUMBER=$((NEW_NUMBER + 1))
        else
            # Found a hole
            break
        fi
    done <<< "$NUMBERS"
fi

# Construct the new workspace name with dot prefix (no suffix)
NEW_WORKSPACE=".${SELECTED_PROJECT}${NEW_NUMBER}"

echo "Creating workspace: $NEW_WORKSPACE on monitor $CURRENT_MONITOR"

# Create and switch to the new workspace
hyprctl dispatch workspace "name:$NEW_WORKSPACE"

# Track workspace in persistent list
WORKSPACE_LIST="$HOME/.config/hypr/workspaces-list"
# Remove any previous entry for this workspace (dedup)
if [ -f "$WORKSPACE_LIST" ]; then
    sed -i "/^${NEW_WORKSPACE} /d" "$WORKSPACE_LIST"
fi
# Add the new workspace with its monitor
echo "${NEW_WORKSPACE} ${CURRENT_MONITOR}" >> "$WORKSPACE_LIST"

# Optional: Show notification
# notify-send "Workspace Created" "$NEW_WORKSPACE"
