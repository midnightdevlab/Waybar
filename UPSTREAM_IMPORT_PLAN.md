# Plan: Safely Import Upstream hyprland/workspaces Module

## Current State
- Branch: `P001-upstream-merge-strategy.H001-rename-fancy-workspaces`
- Our custom code is in: `fancy-workspace*.{hpp,cpp}`
- Original workspace files are **deleted** (renamed to fancy-*)
- Module registered as: `hyprland/fancy-workspaces`
- Classes: `FancyWorkspace` and `FancyWorkspaces`

## Goal
Import vanilla `hyprland/workspaces` from `origin/master` so both modules coexist:
- `hyprland/workspaces` (vanilla from upstream)
- `hyprland/fancy-workspaces` (our custom version)

## Risk Assessment
**Low Risk** because:
- ✅ Our code is in different files (`fancy-*`)
- ✅ Our classes have different names
- ✅ No file name conflicts
- ⚠️ But: need to update `meson.build` and `factory.cpp` to include BOTH

**Potential Issues:**
- Factory.cpp already has our include - need to add vanilla include too
- Meson.build needs both sets of files
- Test files might conflict (but we already renamed ours)

## Safe Import Strategy

### Step 1: Create Safety Backup
```bash
git branch backup-fancy-workspaces HEAD
```
This creates a branch pointing to current state. If anything goes wrong, we can:
```bash
git reset --hard backup-fancy-workspaces
```

### Step 2: Check What Files Upstream Has
```bash
git show origin/master:include/modules/hyprland/workspace.hpp | head -20
git show origin/master:include/modules/hyprland/workspaces.hpp | head -20
git show origin/master:src/modules/hyprland/workspace.cpp | head -20
git show origin/master:src/modules/hyprland/workspaces.cpp | head -20
```
Verify the files exist and preview them.

### Step 3: Extract Vanilla Files from Upstream
```bash
# Create the files with upstream content
git show origin/master:include/modules/hyprland/workspace.hpp > include/modules/hyprland/workspace.hpp
git show origin/master:include/modules/hyprland/workspaces.hpp > include/modules/hyprland/workspaces.hpp
git show origin/master:src/modules/hyprland/workspace.cpp > src/modules/hyprland/workspace.cpp
git show origin/master:src/modules/hyprland/workspaces.cpp > src/modules/hyprland/workspaces.cpp

git add include/modules/hyprland/workspace.hpp \
        include/modules/hyprland/workspaces.hpp \
        src/modules/hyprland/workspace.cpp \
        src/modules/hyprland/workspaces.cpp
```

### Step 4: Update Build System (meson.build)
Add the vanilla files back:
```meson
# Around line 320-326
src_files += files(
    'src/modules/hyprland/backend.cpp',
    'src/modules/hyprland/language.cpp',
    'src/modules/hyprland/submap.cpp',
    'src/modules/hyprland/window.cpp',
    'src/modules/hyprland/windowcount.cpp',
    'src/modules/hyprland/workspace.cpp',          # Vanilla
    'src/modules/hyprland/workspaces.cpp',         # Vanilla
    'src/modules/hyprland/fancy-workspace.cpp',    # Custom (already there)
    'src/modules/hyprland/fancy-workspaces.cpp',   # Custom (already there)
    'src/modules/hyprland/windowcreationpayload.cpp',
)
```

### Step 5: Update Factory Registration
In `src/factory.cpp`, add vanilla module registration:
```cpp
#include "modules/hyprland/workspaces.hpp"          // Vanilla
#include "modules/hyprland/fancy-workspaces.hpp"    // Custom (already there)

// ...

if (ref == "hyprland/workspaces") {
  return new waybar::modules::hyprland::Workspaces(id, bar_, config_[name]);
}
if (ref == "hyprland/fancy-workspaces") {
  return new waybar::modules::hyprland::FancyWorkspaces(id, bar_, config_[name]);
}
```

### Step 6: Update util/enum.cpp
Need both enum instantiations:
```cpp
#include "modules/hyprland/workspaces.hpp"
#include "modules/hyprland/fancy-workspaces.hpp"

// ...

template struct EnumParser<modules::hyprland::Workspaces::SortMethod>;
template struct EnumParser<modules::hyprland::Workspaces::ActiveWindowPosition>;
template struct EnumParser<modules::hyprland::FancyWorkspaces::SortMethod>;
template struct EnumParser<modules::hyprland::FancyWorkspaces::ActiveWindowPosition>;
```

### Step 7: Build and Test
```bash
cd build
ninja
```

Expected result: Both modules compile and link successfully.

### Step 8: Commit
```bash
git add -A
git commit -m "add vanilla hyprland/workspaces from upstream alongside fancy-workspaces

Both modules now coexist:
- hyprland/workspaces (vanilla)
- hyprland/fancy-workspaces (custom)
"
```

### Step 9: Test Both Modules
Create two config entries to verify:
```json
{
  "modules-left": ["hyprland/workspaces"],  // Test vanilla
  "hyprland/workspaces": { /* minimal config */ }
}
```

Then test fancy:
```json
{
  "modules-left": ["hyprland/fancy-workspaces"],  // Test custom
  "hyprland/fancy-workspaces": { /* your config */ }
}
```

## Rollback Plan
If anything goes wrong at any step:
```bash
git reset --hard backup-fancy-workspaces
# Or if committed:
git revert HEAD
```

## Expected Outcome
- ✅ 8 workspace-related files total:
  - 4 vanilla: `workspace.{hpp,cpp}` + `workspaces.{hpp,cpp}`
  - 4 custom: `fancy-workspace.{hpp,cpp}` + `fancy-workspaces.{hpp,cpp}`
- ✅ Both modules registered in factory
- ✅ Both compile successfully
- ✅ No conflicts (different files, different classes)
- ✅ Can merge from upstream without touching our custom code

## Next Steps After Success
1. Test that vanilla module works
2. Verify custom module still works
3. Try merging latest upstream changes
4. Update hypothesis decision document
