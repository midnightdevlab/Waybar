# Implementation Plan: H001 - Track Group Empty State and Apply CSS Class

## Overview
Add empty state tracking to collapsed workspace groups so they use the same "empty" CSS class as individual empty workspaces.

## Current State Analysis

**ProjectGroup struct** (line 1219):
```cpp
struct ProjectGroup {
  std::string prefix;
  std::vector<Workspace*> workspaces;
  bool hasActive = false;
  int firstPosition = -1;
};
```

**Workspace has `isEmpty()` method** (line 271 in workspace.cpp):
```cpp
bool Workspace::isEmpty() const {
  auto ignore_list = m_workspaceManager.getIgnoredWindows();
  if (ignore_list.empty()) {
    return m_windows == 0;
  }
  // ... (handles ignored windows)
}
```

**Individual workspaces apply "empty" class** (line 237 in workspace.cpp):
```cpp
addOrRemoveClass(styleContext, isEmpty(), "empty");
```

---

## Implementation Changes

### Change 1: Extend ProjectGroup struct

**Location:** Line 1219 in `src/modules/hyprland/workspaces.cpp`

**Current:**
```cpp
struct ProjectGroup {
  std::string prefix;
  std::vector<Workspace*> workspaces;
  bool hasActive = false;
  int firstPosition = -1;
};
```

**Change to:**
```cpp
struct ProjectGroup {
  std::string prefix;
  std::vector<Workspace*> workspaces;
  bool hasActive = false;
  bool hasWindows = false;  // Track if any workspace in group has windows
  int firstPosition = -1;
};
```

**Rationale:** Mirrors the existing `hasActive` pattern for tracking group state.

---

### Change 2: Calculate hasWindows during group construction

**Location:** Lines 1236-1246 (inside the group construction loop)

**Current:**
```cpp
    if (prefix) {
      auto& group = groups[*prefix];
      group.prefix = *prefix;
      group.workspaces.push_back(workspace.get());
      
      if (workspace->isActive()) {
        group.hasActive = true;
      }
      
      if (group.firstPosition == -1) {
        group.firstPosition = i;
      }
    }
```

**Change to:**
```cpp
    if (prefix) {
      auto& group = groups[*prefix];
      group.prefix = *prefix;
      group.workspaces.push_back(workspace.get());
      
      if (workspace->isActive()) {
        group.hasActive = true;
      }
      
      // Check if this workspace has windows
      if (!workspace->isEmpty()) {
        group.hasWindows = true;
      }
      
      if (group.firstPosition == -1) {
        group.firstPosition = i;
      }
    }
```

**Rationale:** 
- Uses existing `isEmpty()` method - no duplication of window counting logic
- If any workspace in the group is not empty, the group has windows
- Same pattern as `hasActive` tracking
- Happens during group construction, so no extra iteration needed

---

### Change 3: Apply "empty" CSS class to collapsed buttons

**Location:** Lines 1294-1299 (when creating collapsed button)

**Current:**
```cpp
      // Create collapsed button with click handler
      auto collapsedBtn = std::make_unique<Gtk::Button>();
      collapsedBtn->set_relief(Gtk::RELIEF_NONE);
      collapsedBtn->set_label("[" + displayPrefix + "]");
      collapsedBtn->get_style_context()->add_class("collapsed-project");
      collapsedBtn->get_style_context()->add_class(MODULE_CLASS);
```

**Change to:**
```cpp
      // Create collapsed button with click handler
      auto collapsedBtn = std::make_unique<Gtk::Button>();
      collapsedBtn->set_relief(Gtk::RELIEF_NONE);
      collapsedBtn->set_label("[" + displayPrefix + "]");
      collapsedBtn->get_style_context()->add_class("collapsed-project");
      collapsedBtn->get_style_context()->add_class(MODULE_CLASS);
      
      // Apply empty class if group has no windows
      if (!group.hasWindows) {
        collapsedBtn->get_style_context()->add_class("empty");
      }
```

**Rationale:**
- Adds "empty" class only when group has no windows
- Matches the CSS class used by individual workspace buttons (line 237 in workspace.cpp)
- No need to remove the class - buttons are recreated on each update

---

## How It Works

1. **Group Construction** (lines 1236-1246):
   - As workspaces are added to groups, check each workspace's `isEmpty()` status
   - Set `group.hasWindows = true` if any workspace is not empty
   - Default value is `false`, so groups with all empty workspaces stay `false`

2. **Button Creation** (lines 1294+):
   - When creating collapsed button, check `group.hasWindows`
   - If `false` (all workspaces empty), add "empty" CSS class
   - If `true` (has windows), don't add the class

3. **Updates**:
   - `applyProjectCollapsing()` is called during workspace updates
   - Buttons are recreated, so CSS classes update automatically
   - No need for explicit class removal or state tracking

---

## CSS Class Behavior

**Empty group** (no windows in any workspace):
```html
<button class="collapsed-project workspaces empty">
  [ab]
</button>
```

**Populated group** (has windows in at least one workspace):
```html
<button class="collapsed-project workspaces">
  [wh]
</button>
```

This matches the pattern used for individual workspaces:
- Empty workspace: has "empty" class
- Populated workspace: no "empty" class

---

## Testing Plan

### Test 1: Empty Group Detection
1. Create workspaces: `.test1`, `.test2`, `.test3` (all empty)
2. Switch away so group collapses
3. Check CSS: collapsed button should have "empty" class
4. ✓ Expected: `class="collapsed-project workspaces empty"`

### Test 2: Populated Group Detection
1. Open window in `.ab2`
2. Check CSS: collapsed button should NOT have "empty" class
3. ✓ Expected: `class="collapsed-project workspaces"`

### Test 3: State Change - Empty to Populated
1. Start with empty group `.idea`
2. Verify collapsed button has "empty" class
3. Open window in `.idea1`
4. Verify "empty" class is removed on next update
5. ✓ Expected: CSS updates reflect window opening

### Test 4: State Change - Populated to Empty
1. Start with group `.wh` containing windows
2. Verify collapsed button has no "empty" class
3. Close all windows in the group
4. Verify "empty" class is added on next update
5. ✓ Expected: CSS updates reflect window closing

### Test 5: Multi-Monitor
1. Same group name on different monitors
2. Monitor 1: `.ab` has windows
3. Monitor 2: `.ab` is empty
4. ✓ Expected: Monitor 1 shows populated, Monitor 2 shows empty (independent per bar)

### Visual Verification
- Use browser inspector or `waybar -l debug` to check CSS classes
- Verify styling matches empty workspace appearance (font, color, etc.)
- Test with custom CSS themes

---

## Edge Cases Handled

1. **All workspaces empty**: `hasWindows` stays `false`, "empty" class applied ✓
2. **One workspace has windows**: `hasWindows` becomes `true`, no "empty" class ✓
3. **Windows in ignored list**: `isEmpty()` handles this already ✓
4. **Group with single workspace**: Same logic applies ✓
5. **Active empty group**: Can be both active and empty (different states) ✓

---

## Files Modified

Only **one file** needs changes:
- `src/modules/hyprland/workspaces.cpp`

---

## Code Metrics

- Lines added: ~6
- Lines modified: 0
- Complexity: Very low
- New dependencies: None (uses existing `isEmpty()` method)

---

## Rollback Strategy

If issues arise:
1. Git revert the commit
2. Feature only adds CSS class - removing it won't break styling
3. Existing CSS will simply not match the class, defaulting to base styles
4. No data structures changed, no behavior modified beyond CSS

---

## Notes

**Why not track window count?**
- `isEmpty()` already exists and handles edge cases (ignored windows, etc.)
- No need to duplicate the logic
- More maintainable - changes to empty detection happen in one place

**Why not use a separate CSS class?**
- "empty" is already the standard class for empty workspaces
- Consistent with existing Waybar conventions
- Users' existing CSS for empty workspaces will automatically apply to empty groups

**Update frequency:**
- `applyProjectCollapsing()` is called during workspace updates
- Same frequency as workspace state updates
- No performance impact - just a boolean check during group construction
