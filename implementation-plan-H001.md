# Implementation Plan: H001 - Track Last Active Workspace Per Group

## Overview
Add tracking of last active workspace per group with monitor awareness to provide intuitive "return to where I was" behavior when clicking collapsed workspace groups.

## Files to Modify

### 1. `include/modules/hyprland/workspaces.hpp`

**Location:** After line 215 (in private section, near m_collapsedButtons)

**Add:**
```cpp
// Track last active workspace per group+monitor for collapsed button clicks
std::map<std::string, std::string> m_lastActivePerGroup;
```

**Why here:** Keeping it with other project collapsing state variables for logical grouping.

---

### 2. `src/modules/hyprland/workspaces.cpp`

#### Change 1: Update `onWorkspaceActivated()` to track history

**Location:** Lines 366-372 (the onWorkspaceActivated function)

**Current code:**
```cpp
void Workspaces::onWorkspaceActivated(std::string const &payload) {
  const auto [workspaceIdStr, workspaceName] = splitDoublePayload(payload);
  const auto workspaceId = parseWorkspaceId(workspaceIdStr);
  if (workspaceId.has_value()) {
    m_activeWorkspaceId = *workspaceId;
  }
}
```

**Add after line 371 (inside the if block):**
```cpp
    // Track last active workspace per group for collapsed button behavior
    auto prefix = extractProjectPrefix(workspaceName);
    if (prefix) {
      // Build compound key: {prefix}@{monitor}
      // Get monitor from workspace object or current monitor
      std::string monitor = getBarOutput();  // Current bar's monitor
      std::string key = *prefix + "@" + monitor;
      m_lastActivePerGroup[key] = workspaceName;
      spdlog::trace("Tracked last active workspace: {} for key {}", workspaceName, key);
    }
```

**Rationale:** This is called every time a workspace becomes active, perfect place to update our tracking map.

**Note:** Need to verify if we can get the workspace's actual monitor or if getBarOutput() is sufficient. May need to look up the workspace object to get its monitor.

---

#### Change 2: Update collapsed button click handler to use history

**Location:** Lines 1286-1296 (collapsed button signal_clicked handler in applyProjectCollapsing)

**Current code:**
```cpp
      // Add click handler to expand and switch to first workspace
      Workspace* firstWorkspace = group.workspaces[0];
      collapsedBtn->signal_clicked().connect([this, firstWorkspace]() {
        try {
          std::string workspaceName = firstWorkspace->name();
          spdlog::debug("Workspace collapsed group clicked: switching to {}", workspaceName);
          m_ipc.getSocket1Reply("dispatch workspace name:" + workspaceName);
        } catch (const std::exception& e) {
          spdlog::error("Workspace group click failed: {}", e.what());
        }
      });
```

**Replace with:**
```cpp
      // Add click handler to expand and switch to last active (or first) workspace
      Workspace* firstWorkspace = group.workspaces[0];
      std::string groupPrefix = prefix;  // Capture by value
      collapsedBtn->signal_clicked().connect([this, firstWorkspace, groupPrefix]() {
        try {
          // Build compound key for this group+monitor
          std::string monitor = getBarOutput();
          std::string key = groupPrefix + "@" + monitor;
          
          // Look up last active workspace for this group
          std::string workspaceName;
          auto it = m_lastActivePerGroup.find(key);
          if (it != m_lastActivePerGroup.end()) {
            workspaceName = it->second;
            spdlog::debug("Workspace collapsed group '{}' clicked: switching to last active {}", 
                         groupPrefix, workspaceName);
          } else {
            // No history, fall back to first workspace
            workspaceName = firstWorkspace->name();
            spdlog::debug("Workspace collapsed group '{}' clicked: no history, switching to first {}", 
                         groupPrefix, workspaceName);
          }
          
          m_ipc.getSocket1Reply("dispatch workspace name:" + workspaceName);
        } catch (const std::exception& e) {
          spdlog::error("Workspace group click failed: {}", e.what());
        }
      });
```

**Rationale:** Lambda now checks the history map first, falls back to first workspace if no entry exists.

---

## Implementation Steps

1. **Add member variable** to header file
   - Simple one-line addition to private section

2. **Update onWorkspaceActivated()**
   - Add tracking logic after workspace ID is set
   - Need to verify monitor identification approach
   - Consider if we need to handle special workspaces differently

3. **Update collapsed button click handler**
   - Replace simple first-workspace logic with history lookup
   - Capture groupPrefix by value in lambda
   - Add fallback behavior for missing history

4. **Test scenarios**
   - Switch between workspaces in same group, verify clicking returns to last
   - Test first-time click (no history) falls back correctly
   - Test with multiple monitors having same group names
   - Verify monitor identifier is extracted correctly

5. **Edge cases to consider**
   - What if the last active workspace no longer exists? (Should still work - Hyprland handles invalid workspace names)
   - Special workspaces - should we track them? (Probably not, but check if extractProjectPrefix filters them)
   - Workspace renamed - map will have old name, next activation will update it

---

## Open Questions

### Q1: Monitor identification
**Question:** Should we use `getBarOutput()` (current bar's monitor) or get the workspace's actual monitor?

**Options:**
- `getBarOutput()` - simpler, assumes user clicks on the bar for the monitor they want
- Lookup workspace object's monitor - more accurate if workspaces can span monitors

**Recommendation:** Start with `getBarOutput()` since collapsed buttons only appear on their respective monitor's bar.

### Q2: Workspace lookup validation
**Question:** Should we validate that the workspace name from history still exists before switching?

**Answer:** No - Hyprland's `dispatch workspace name:X` handles non-existent workspaces gracefully, so we can just pass the name and let Hyprland handle it.

### Q3: Map cleanup
**Question:** Should we clean up m_lastActivePerGroup entries when workspaces are destroyed?

**Answer:** Not critical - memory overhead is minimal (just strings), and stale entries don't cause issues. Could add cleanup in `onWorkspaceDestroyed()` if needed later.

---

## Testing Plan

### Manual Tests

1. **Basic functionality**
   - Switch to `.ab3`
   - Switch to `.wh2` (ab collapses)
   - Click [ab]
   - ✓ Should go to `.ab3`

2. **First-time behavior**
   - Never visit `.idea` group before
   - Click [idea]
   - ✓ Should go to `.idea0` (first workspace)

3. **Multi-monitor**
   - Monitor 1: switch to `.ab3`
   - Monitor 2: switch to `.ab1`
   - Monitor 1: click [ab]
   - ✓ Should go to `.ab3` (not `.ab1`)

4. **History updates**
   - Switch to `.ab1`
   - Switch to `.ab3`
   - Switch to `.wh2` (ab collapses)
   - Click [ab]
   - ✓ Should go to `.ab3` (most recent)

### Debug Verification
- Check trace logs show "Tracked last active workspace: X for key Y"
- Verify compound keys format correctly as `.prefix@monitor-name`
- Confirm fallback log messages appear on first click

---

## Rollback Strategy

If issues arise:
1. Git revert the commit
2. Feature is purely additive - removing it won't break existing collapse behavior
3. No configuration needed, no breaking changes to APIs

---

## Estimated Effort

- Header change: 1 line
- onWorkspaceActivated update: ~10 lines
- Click handler update: ~20 lines
- Testing: ~15 minutes

**Total:** ~30 lines of code, low complexity
