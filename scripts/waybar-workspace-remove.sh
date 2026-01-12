#!/bin/bash
# Remove a persistent workspace by renaming it (removing the dot prefix)
# Usage: waybar-workspace-remove.sh <workspace_name>

if [ -z "$1" ]; then
    echo "Usage: waybar-workspace-remove.sh <workspace_name>"
    exit 1
fi

WORKSPACE_NAME="$1"

# Get workspace info
WORKSPACE_INFO=$(hyprctl workspaces -j | jq -r ".[] | select(.name == \"$WORKSPACE_NAME\")")

if [ -z "$WORKSPACE_INFO" ]; then
    echo "Workspace '$WORKSPACE_NAME' not found"
    exit 1
fi

# Extract workspace ID and window count
WORKSPACE_ID=$(echo "$WORKSPACE_INFO" | jq -r '.id')
WINDOW_COUNT=$(echo "$WORKSPACE_INFO" | jq -r '.windows')

# Check if workspace is empty
if [ "$WINDOW_COUNT" -ne 0 ]; then
    echo "Workspace '$WORKSPACE_NAME' is not empty (has $WINDOW_COUNT windows)"
    exit 1
fi

# Remove leading dot from workspace name
if [[ "$WORKSPACE_NAME" =~ ^\.(.*) ]]; then
    NEW_NAME="${BASH_REMATCH[1]}"
else
    echo "Workspace '$WORKSPACE_NAME' does not start with a dot"
    exit 1
fi

echo "Removing workspace '$WORKSPACE_NAME' (id: $WORKSPACE_ID) by renaming to '$NEW_NAME'"

# Rename workspace (removes persistence, hyprland will auto-delete empty non-persistent workspaces)
hyprctl dispatch renameworkspace "$WORKSPACE_ID" "$NEW_NAME"

exit $?
