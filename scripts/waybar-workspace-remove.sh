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

# Rename workspace (removes persistence, hyprland will auto-delete empty non-persistent workspaces).
# The 0.55 lua dispatcher resolves 'workspace' through a selector string only: a numeric id is
# rejected with "no such workspace" (the legacy `renameworkspace <id> <name>` did accept an id).
# hyprctl exits 0 even on that warning, so the failure has to be caught in the output.
RENAME_OUT=$(hyprctl dispatch "hl.dsp.workspace.rename({workspace='name:$WORKSPACE_NAME', name='$NEW_NAME'})" 2>&1)

if [ -n "$RENAME_OUT" ] && [ "$RENAME_OUT" != "ok" ]; then
    echo "Rename of '$WORKSPACE_NAME' failed: $RENAME_OUT"
    exit 1
fi

# Remove from persistent workspace list
WORKSPACE_LIST="$HOME/.config/hypr/workspaces-list"
if [ -f "$WORKSPACE_LIST" ]; then
    sed -i "/^${WORKSPACE_NAME} /d" "$WORKSPACE_LIST"
fi

echo "Removed workspace '$WORKSPACE_NAME'"
exit 0
