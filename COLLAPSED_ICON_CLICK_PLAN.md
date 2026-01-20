# Collapsed Group Icon Click Feature - Implementation Plan

## Current Behavior - ACTUAL (Verified with Logging)
When clicking on a collapsed group button (e.g., `[wb]`):
- **ANY click** (icon, label, or bracket) triggers the Button's `clicked` handler (line 1580)
- It switches to the last active workspace in that group, or the first workspace if no history
- The EventBox `button_press_event` handler (lines 1545-1555) **is NEVER triggered**

**Root Cause**: The EventBox is inside the Button's content, but GTK is not properly capturing/stopping the event before it reaches the Button. The icon click events propagate through to the parent Button.

## Desired Behavior

### Click on Label/Brackets
- Keep current behavior: focus last used workspace in group
- This happens when clicking on `[`, `]`, or the prefix label

### Click on Icon
Smart window selection based on last active workspace:
1. Identify all windows represented by that icon (can be multiple due to deduplication)
2. Check if any of those windows are in the "last active workspace for the group"
   - If YES: focus that window
3. If NO window in last active workspace:
   - Focus the first window for that application (KISS approach)
   - This window might be in a different workspace in the group

## Code Structure Analysis

### Current Icon Click Handler (lines 1545-1555)
```cpp
eventBox->signal_button_press_event().connect([this, firstWindowAddress](GdkEventButton* event) -> bool {
  if (event->button == 1) {
    spdlog::debug("[WICONS] Collapsed icon clicked, focusing window: {}", firstWindowAddress);
    m_ipc.getSocket1Reply("dispatch focuswindow address:0x" + firstWindowAddress);
    return true;  // Stop propagation to parent button
  }
  return false;
});
```

**Problem**: Only captures `firstWindowAddress` (addresses[0]), ignores other windows

### Available Data (lines 1478-1493)
```cpp
std::map<std::string, std::vector<std::string>> iconToAddresses;
// For each icon, we have ALL window addresses across all workspaces in group
```

### Last Active Workspace Tracking
- Key: `groupPrefix + "@" + monitor`
- Map: `m_lastActivePerGroup` (line 1588)
- Value: workspace name (e.g., ".wb6")

### Window to Workspace Mapping
Need to find which workspace each window address belongs to.
We have `iconToWorkspaceAndTitles[iconName]` which stores pairs of (wsName, windowTitle).
But we need (windowAddress -> wsName) mapping.

## Architecture Question: Single Large Button vs Sibling Buttons

### Current Structure: `[One Large Button containing: label + icons]`
```
Button (collapsed-project class)
├── Box (contentBox)
│   ├── Label ("[")
│   ├── Label (prefix "wb")
│   ├── EventBox → Image (icon1)
│   ├── EventBox → Image (icon2)
│   └── Label ("]")
```

**Problems:**
1. All clicks go to the outer Button - EventBox can't intercept
2. Complex event handling trying to distinguish click targets
3. The entire visual block acts as one clickable unit

### Proposed Structure: `[Sibling Buttons]`
```
Box (contains siblings)
├── Button (label button) → "[wb]"
├── Button (icon1) → Image
├── Button (icon2) → Image
```

**Benefits:**
1. ✅ **Clean event handling**: Each button has its own click handler, no propagation issues
2. ✅ **Simpler code**: No need for EventBox wrappers or event interception
3. ✅ **Independent styling**: Can style label vs icons differently with CSS classes
4. ✅ **Natural GTK behavior**: Buttons handle their own clicks

**Potential Concerns:**
1. **Visual appearance**: Need to ensure buttons look grouped together
   - Solution: Use CSS to remove spacing, add borders to create visual unity
   
2. **CSS selectors**: Does `.collapsed-project` class need to apply to all?
   - Can apply same class to all sibling buttons
   - Or use parent Box class and child selectors
   
3. **Hover/focus states**: Each button might highlight individually
   - This might actually be GOOD - shows which element you're about to click
   - Can override in CSS if unified hover is needed

### CSS Implications

Current (assumed):
```css
.collapsed-project {
  /* Styles the entire collapsed group as one unit */
}
```

New approach options:

**Option A - Same class on all:**
```css
.collapsed-project-label { /* Label button */ }
.collapsed-project-icon  { /* Icon buttons */ }
```

**Option B - Parent container:**
```css
.collapsed-project-group { /* Box containing siblings */ }
.collapsed-project-group > button { /* All buttons */ }
```

### Recommendation

**Use sibling buttons** - it's the simpler, more maintainable approach:

1. **Cleaner architecture**: Each UI element does one thing
2. **No event fighting**: GTK works as designed
3. **Easier to reason about**: Click label → workspace, click icon → window
4. **Future-proof**: Adding new elements is straightforward

The only real "benefit" of the large button was visual grouping, which CSS can handle just as well.

## Implementation Steps - REVISED for Sibling Buttons Architecture

### Step 0: Refactor to Sibling Buttons
Replace the single large Button containing EventBoxes with sibling Buttons.

**Current code (lines 1448-1620):**
```cpp
auto collapsedBtn = std::make_unique<Gtk::Button>();
auto* contentBox = Gtk::manage(new Gtk::Box(...));
// Add labels and EventBoxes to contentBox
collapsedBtn->add(*contentBox);
collapsedBtn->signal_clicked().connect([...]() { /* switch workspace */ });
m_box.add(*collapsedBtn);
```

**New code:**
```cpp
// Create container box for the group (not a button)
auto* groupBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
groupBox->get_style_context()->add_class("collapsed-project-group");

// Create label button: [prefix]
auto labelBtn = std::make_unique<Gtk::Button>();
labelBtn->set_relief(Gtk::RELIEF_NONE);
labelBtn->get_style_context()->add_class("collapsed-project-label");
labelBtn->set_label("[" + displayPrefix + "]");

// Add click handler for label - switches to workspace
labelBtn->signal_clicked().connect([this, firstWorkspace, groupPrefix]() {
  // Same logic as before - switch to last active or first workspace
});

groupBox->pack_start(*labelBtn, false, false);

// Create icon buttons
for (const auto& iconName : iconNamesOrdered) {
  auto* iconBtn = Gtk::manage(new Gtk::Button());
  iconBtn->set_relief(Gtk::RELIEF_NONE);
  iconBtn->get_style_context()->add_class("collapsed-project-icon");
  
  auto* icon = Gtk::manage(new Gtk::Image());
  icon->set_pixel_size(m_windowIconSize);
  // Load icon...
  iconBtn->add(*icon);
  iconBtn->set_tooltip_text(tooltip);
  
  // Add click handler for icon - smart window focus
  std::vector<std::string> allAddresses = iconToAddresses[iconName];
  std::map<std::string, std::string> addrToWs = addressToWorkspace;
  iconBtn->signal_clicked().connect([this, allAddresses, addrToWs, groupPrefix]() {
    std::string targetAddress = selectBestWindowForIcon(
      allAddresses, addrToWs, groupPrefix, getBarOutput()
    );
    m_ipc.getSocket1Reply("dispatch focuswindow address:0x" + targetAddress);
  });
  
  groupBox->pack_start(*iconBtn, false, false);
}

// Add the group to main box
m_box.add(*groupBox);
// Track for cleanup - change from m_collapsedButtons to m_collapsedGroups
m_collapsedGroups.push_back(groupBox);
```

**Key Changes:**
- Container is now a Box, not a Button
- Label is a Button (handles workspace switching)
- Each icon is a Button (handles window focusing)
- No more EventBox wrappers
- No event propagation issues

### Step 1: Build Window Address to Workspace Mapping
When collecting icons (lines 1480-1495), also build:
```cpp
std::map<std::string, std::string> addressToWorkspace;
// Maps window address -> workspace name
```

### Step 2: Implement Smart Window Selection

Add the helper method to select the best window:

```cpp
std::string Workspaces::selectBestWindowForIcon(
  const std::vector<std::string>& addresses,
  const std::map<std::string, std::string>& addressToWorkspace,
  const std::string& groupPrefix,
  const std::string& monitor
) {
  if (addresses.empty()) {
    spdlog::error("[ICON_CLICK] No addresses provided");
    return "";
  }
  
  // Build key for last active lookup
  std::string key = groupPrefix + "@" + monitor;
  
  // Try to find last active workspace
  auto it = m_lastActivePerGroup.find(key);
  if (it != m_lastActivePerGroup.end()) {
    std::string lastActiveWs = it->second;
    
    // Look for a window in that workspace
    for (const auto& addr : addresses) {
      auto wsIt = addressToWorkspace.find(addr);
      if (wsIt != addressToWorkspace.end() && wsIt->second == lastActiveWs) {
        spdlog::info("[ICON_CLICK] Found window in last active workspace '{}': {}", 
                     lastActiveWs, addr);
        return addr;
      }
    }
    
    spdlog::debug("[ICON_CLICK] No window in last active workspace '{}', using first", 
                  lastActiveWs);
  } else {
    spdlog::debug("[ICON_CLICK] No last active workspace for group '{}', using first", 
                  groupPrefix);
  }
  
  // Fallback: return first window
  return addresses[0];
}
```

### Step 3: Update Header File

In `include/modules/hyprland/workspaces.hpp`:

```cpp
private:
  // Change tracking variable
  std::vector<Gtk::Box*> m_collapsedGroups;  // Was m_collapsedButtons
  
  // Add helper method
  std::string selectBestWindowForIcon(
    const std::vector<std::string>& addresses,
    const std::map<std::string, std::string>& addressToWorkspace,
    const std::string& groupPrefix,
    const std::string& monitor
  );
```

### Step 4: Update Cleanup Code

Find where `m_collapsedButtons` is cleared (around line 1420):

```cpp
// Old
for (auto& btn : m_collapsedButtons) {
  m_box.remove(*btn);
}
m_collapsedButtons.clear();

// New  
for (auto* groupBox : m_collapsedGroups) {
  m_box.remove(*groupBox);
}
m_collapsedGroups.clear();
```

## Edge Cases

1. **Empty addresses vector**: Should never happen (icon only added if addresses exist)
2. **addressToWorkspace missing entry**: Shouldn't happen but fallback to first window
3. **Last active workspace deleted**: Fallback to first window (already handled)
4. **Multiple windows in last active workspace**: Pick first match (KISS)

## Testing Checklist

- [ ] Collapsed group displays correctly: `[prefix] icon1 icon2`
- [ ] Click on label button switches to last active workspace in group
- [ ] Click on label button (no history) switches to first workspace in group
- [ ] Click on icon representing single window focuses that window
- [ ] Click on icon representing multiple windows in same workspace focuses one
- [ ] Click on icon representing windows across multiple workspaces:
  - [ ] When last active workspace has a matching window → focuses it
  - [ ] When last active workspace doesn't have matching window → focuses first
- [ ] CSS styling still looks good (may need adjustments)
- [ ] Empty groups show correctly
- [ ] No event handling conflicts or errors in logs

## CSS Considerations

User's CSS may need updates to style the new structure:

**Old selectors that might break:**
```css
.collapsed-project { /* Was the button */ }
```

**New selectors needed:**
```css
.collapsed-project-group { /* Container box */ }
.collapsed-project-label { /* Label button */ }
.collapsed-project-icon  { /* Icon buttons */ }

/* Or unified styling: */
.collapsed-project-group > button { /* All buttons */ }
```

We should apply `.collapsed-project` class to the container Box for backward compatibility, and add specific classes for label/icon buttons.

## Files to Modify

1. `src/modules/hyprland/workspaces.cpp`:
   - **Refactor collapsed group creation** (~lines 1448-1620): Replace Button+EventBox with Box+Buttons
   - **Build addressToWorkspace map** (~line 1490)
   - **Add selectBestWindowForIcon() method** (new, ~30 lines)
   - **Update cleanup code** (~line 1420): Change m_collapsedButtons to m_collapsedGroups

2. `include/modules/hyprland/workspaces.hpp`:
   - Change `std::vector<std::unique_ptr<Gtk::Button>> m_collapsedButtons` to `std::vector<Gtk::Box*> m_collapsedGroups`
   - Add `selectBestWindowForIcon()` declaration

## Estimated Lines Changed

- ~80 lines refactored (collapsed group creation - structural change)
- ~30 lines added (selectBestWindowForIcon method)
- ~5 lines modified (cleanup code)
- ~10 lines added (addressToWorkspace mapping)
- Total: ~125 lines (but most are restructuring existing code)
