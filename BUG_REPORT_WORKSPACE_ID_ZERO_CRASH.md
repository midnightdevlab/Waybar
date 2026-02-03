# Bug Report: Hyprland Crash When Clicking Empty Workspaces with ID=0

## Executive Summary

Waybar crashes Hyprland (signal 6, ABRT, "dividing by zero") when clicking on certain inactive empty workspace buttons. The root cause is that workspaces with ID=0 are being dispatched to Hyprland, which triggers a divide-by-zero error in Hyprland's workspace switching code.

## The Bug

### Symptoms
- **Crash type**: Hyprland receives signal 6 (ABRT) with "dividing by zero" error
- **Trigger**: Left-clicking an inactive, empty, persistent workspace button in Waybar
- **Specific scenario**: "Single ws" (workspace groups with only 1 workspace) that are persistent and empty

### Root Cause Chain

1. **Named workspaces get ID=0**
   - When Waybar creates persistent workspaces from config (e.g., `".myproject1"`), it calls `parseWorkspaceId(".myproject1")`
   - Since ".myproject1" is not a number, `parseWorkspaceId` returns `std::nullopt`
   - The code sets `workspaceId = 0` as a fallback (line 63 in `fancy-workspaces.cpp`)

2. **Guard prevents creation but not all cases**
   - There's a guard at line 78 in `createWorkspace()` that skips workspaces with ID=0
   - Comment says: "these are workspace rules like "n[s:.]", not real workspaces"
   - However, this guard is TOO BROAD and blocks legitimate named persistent workspaces

3. **Workspaces with ID=0 can still exist**
   - Workspaces created BEFORE the guard was added (legacy state)
   - Workspaces returned by Hyprland's IPC socket `getSocket1JsonReply("workspaces")` that have ID=0
   - Named workspaces that Hyprland itself assigns ID=0 to (unclear if this happens, but possible)

4. **Click handler doesn't validate ID**
   - When clicking a workspace button, the handler checks:
     ```cpp
     if (id() > 0) {  // numbered workspace
       // dispatch workspace <id>
     } else if (!isSpecial()) {  // named workspace - THIS MATCHES FOR ID=0!
       // dispatch workspace name:<name>
     }
     ```
   - For workspaces with `id() == 0` and `!isSpecial()`, it dispatches: `workspace name:<workspacename>`

5. **Hyprland crashes**
   - Hyprland receives the dispatch command for a workspace with ID=0
   - Internal Hyprland code divides by workspace ID (or performs arithmetic assuming ID > 0)
   - Division by zero → ABRT signal

## Code Locations

### Where ID=0 is assigned
**File**: `src/modules/hyprland/fancy-workspaces.cpp`
**Lines**: 61-64
```cpp
auto workspaceId = parseWorkspaceId(name);
if (!workspaceId.has_value()) {
  workspaceId = 0;  // ← PROBLEM: Fallback to 0 for named workspaces
}
```

### Where the guard blocks creation
**File**: `src/modules/hyprland/fancy-workspaces.cpp`
**Lines**: 77-81
```cpp
// Skip workspaces with ID 0 (these are workspace rules like "n[s:.]", not real workspaces)
if (workspaceId == 0) {
  spdlog::debug("Workspace '{}' skipped: invalid id {}", workspaceName, workspaceId);
  return;  // ← BLOCKS creation, but doesn't prevent all ID=0 workspaces
}
```

### Where the crash dispatch happens
**File**: `src/modules/hyprland/fancy-workspace.cpp`
**Lines**: 228-233
```cpp
} else if (!isSpecial()) {  // named (this includes persistent)
  // ← This branch matches workspaces with ID=0!
  if (m_workspaceManager.moveToMonitor()) {
    m_ipc.getSocket1Reply("dispatch focusworkspaceoncurrentmonitor name:" + name());
  } else {
    m_ipc.getSocket1Reply("dispatch workspace name:" + name());  // ← CRASH
  }
}
```

## Why Named Workspaces Have ID=0

Named workspaces (like `.myproject1`, `myworkspace`, etc.) cannot be parsed as integers by `parseWorkspaceId()`:

```cpp
std::optional<int> FancyWorkspaces::parseWorkspaceId(std::string const& workspaceIdStr) {
  try {
    return workspaceIdStr == "special" ? -99 : std::stoi(workspaceIdStr);
  } catch (std::exception const& e) {
    // ← Throws exception for non-numeric names
    spdlog::debug("Workspace \"{}\" is not bound to an id: {}", workspaceIdStr, e.what());
    return std::nullopt;  // ← Returns nullopt
  }
}
```

This is **by design** - named workspaces should have non-numeric IDs. But the fallback to `0` creates a problematic state.

## Possible Sources of ID=0 Workspaces

1. **Persistent workspaces from Waybar config**
   - User configures: `"persistent-workspaces": { "monitor": [".myproject1"] }`
   - `createMonitorWorkspaceData()` assigns ID=0
   - Guard at line 78 SHOULD block these... but may not catch all cases

2. **Workspaces from Hyprland IPC**
   - `initializeWorkspaces()` calls `getSocket1JsonReply("workspaces")`
   - If Hyprland returns a workspace with ID=0, it bypasses the guard
   - The workspace is created directly from Hyprland's data

3. **Legacy state**
   - Workspaces created before the guard was added
   - These remain in `m_workspaces` until removed

4. **Workspace rules vs workspace instances**
   - The guard comment mentions "workspace rules like n[s:.]"
   - There's confusion between:
     - Workspace RULES (patterns like "n[s:.]" from `workspacerules` IPC) → should have ID=0
     - Workspace INSTANCES (actual workspaces) → should never have ID=0
   - The guard blocks both, but it's unclear if that's correct

## The Fix Applied

### Safety Check in Click Handler
Added a defensive check to prevent dispatching any workspace with ID=0:

```cpp
// Safety check: Never dispatch workspaces with ID=0 (invalid state)
if (id() == 0) {
  spdlog::error("#DEBUG Refusing to dispatch workspace with ID=0: name='{}' isPersistent={}", 
                name(), isPersistent());
  return true;  // Consume the click to prevent issues
}
```

**Location**: `src/modules/hyprland/fancy-workspace.cpp`, lines 226-231

This is a **defensive fix** that prevents the crash symptom, but doesn't address the root cause of why workspaces with ID=0 exist in the first place.

## Open Questions

1. **Can Hyprland return workspaces with ID=0?**
   - Does Hyprland's `workspaces` IPC endpoint ever return workspaces with ID=0?
   - Or only workspace rules with ID=0?

2. **Should named workspaces have negative IDs instead?**
   - Similar to special workspaces (ID=-99), should named workspaces have unique negative IDs?
   - Or should they have a special ID range (e.g., starting from -1000)?

3. **Is the guard too broad?**
   - The guard blocks ALL workspaces with ID=0
   - Should it only block workspace RULES, not workspace INSTANCES?
   - How to distinguish between rules and instances?

4. **What about Hyprland's behavior?**
   - Why does Hyprland crash with "divide by zero" when switching to a workspace?
   - Is this a bug in Hyprland that should be fixed?
   - Should Hyprland validate workspace IDs before performing arithmetic?

## Proper Root Cause Fix (TODO)

The defensive fix prevents the crash, but the proper fix should:

1. **Never create workspaces with ID=0**
   - Named persistent workspaces should get proper IDs from Hyprland
   - Or use a different ID scheme (negative IDs, hash IDs, etc.)

2. **Validate workspaces from Hyprland IPC**
   - Check if `workspaces` IPC can return ID=0
   - If so, filter or handle these specially

3. **Clarify the distinction**
   - Separate workspace RULES (patterns) from workspace INSTANCES (actual workspaces)
   - Only block rules with ID=0, not instances

4. **Alternative: Ask Hyprland for the workspace ID**
   - When creating a persistent workspace, ask Hyprland what ID it assigns
   - Use `getSocket1JsonReply("workspaces")` and find by name
   - Store the Hyprland-assigned ID instead of defaulting to 0

## Testing Recommendations

To reproduce and test:

1. Configure a persistent named workspace:
   ```json
   "persistent-workspaces": {
     "monitor": [".myproject1"]
   }
   ```

2. Ensure the workspace exists but is empty and inactive

3. Click on the workspace button

4. Without the fix: Hyprland crashes with SIGABRT
5. With the fix: Click is consumed, error logged, no crash

## Debug Logging Added

Comprehensive logging was added to trace workspace creation and dispatching:

1. **In `createMonitorWorkspaceData`**: Logs when ID=0 is assigned
2. **In `createWorkspace`**: Logs when workspaces are skipped due to ID=0  
3. **In `doUpdate`**: Scans all workspaces and warns about any with ID=0
4. **In click handler**: Logs all workspace properties before dispatching

These logs will help identify exactly when and how workspaces with ID=0 are created.

## Related Code Patterns

### Named Workspace ID Handling
- **Special workspaces**: ID = -99 (or negative for named special)
- **Numbered workspaces**: ID > 0 (e.g., 1, 2, 3...)
- **Named workspaces**: ID = 0 (← PROBLEM)

### Dispatch Commands by Workspace Type
```cpp
if (id() > 0)              → "dispatch workspace <id>"
else if (!isSpecial())     → "dispatch workspace name:<name>"  // ID=0 matches here!
else if (id() != -99)      → "dispatch togglespecialworkspace <name>"
else                       → "dispatch togglespecialworkspace"
```

The second branch (`else if (!isSpecial())`) is intended for named workspaces, but inadvertently matches workspaces with ID=0.

## Conclusion

The bug is caused by a **semantic mismatch**:
- Named workspaces naturally have non-numeric names
- The code defaults their ID to 0 when they can't be parsed
- A guard tries to block ID=0 workspaces
- But some ID=0 workspaces slip through the guard
- The click handler doesn't validate ID before dispatching
- Hyprland crashes when receiving workspace dispatch with ID=0

The **defensive fix** (blocking ID=0 in click handler) prevents the crash but is not a proper solution. The **proper fix** requires understanding why ID=0 workspaces exist and preventing their creation or assigning them valid IDs.
