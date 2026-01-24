#!/bin/bash
# Hook script called when workspace is created
# Args: $1 = workspace name, $2 = monitor name

WORKSPACE="$1"
MONITOR="$2"
WORKSPACE_LIST="$HOME/.config/hypr/workspaces-list"

# Create file if it doesn't exist
touch "$WORKSPACE_LIST"

# Remove any existing entry for this workspace (dedup)
sed -i "/^${WORKSPACE} /d" "$WORKSPACE_LIST"

# Append new entry
echo "${WORKSPACE} ${MONITOR}" >> "$WORKSPACE_LIST"

# Optional: log for debugging
# echo "$(date): Created workspace ${WORKSPACE} on ${MONITOR}" >> /tmp/waybar-hooks.log
