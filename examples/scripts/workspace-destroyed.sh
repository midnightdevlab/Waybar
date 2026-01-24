#!/bin/bash
# Hook script called when workspace is destroyed
# Args: $1 = workspace name

WORKSPACE="$1"
WORKSPACE_LIST="$HOME/.config/hypr/workspaces-list"

# Remove entry for this workspace
if [ -f "$WORKSPACE_LIST" ]; then
    sed -i "/^${WORKSPACE} /d" "$WORKSPACE_LIST"
fi

# Optional: log for debugging
# echo "$(date): Destroyed workspace ${WORKSPACE}" >> /tmp/waybar-hooks.log
