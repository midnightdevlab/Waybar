#!/bin/bash
# Wrapper script to create a new workspace for the current project
# Extracts project from active workspace and calls waybar-workspace-create.sh

ACTIVE_WS=$(hyprctl activeworkspace -j | jq -r '.name')

# Extract project prefix from workspace name (format: .project0, .project1, etc.)
if [[ "$ACTIVE_WS" =~ ^\.(([a-zA-Z]+)[0-9]+) ]]; then
    PROJECT="${BASH_REMATCH[2]}"
    
    # Get the directory where this script is located
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    
    # Call the main creation script with the project parameter
    exec "$SCRIPT_DIR/waybar-workspace-create.sh" "$PROJECT"
else
    # Not a numbered project workspace, fall back to interactive mode
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    exec "$SCRIPT_DIR/waybar-workspace-create.sh"
fi
