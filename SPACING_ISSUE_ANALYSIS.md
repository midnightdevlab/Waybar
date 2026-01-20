# Workspace Spacing Issue - Analysis and Plan

## Problem Summary
The space between workspace label and window icons grows by 4 pixels (from 70px to 74px total button width, IconBox from 42px to 46px).

## Root Cause Analysis

### Before State (ts=1768919778128-129)
- **IconBox**: 42x26, spacing=2, visible=true, icons=4
- **Icon sizes**: 20x26, 20x26, 1x1, 1x1
- **Button total**: 70x30
- **Content total**: 60x26

### After State (ts=1768919786483-484)
- **IconBox**: 46x26, spacing=2, visible=true, icons=6
- **Icon sizes**: 1x26, 1x26, 20x26, 20x26, 1x1, 1x1
- **Button total**: 74x30
- **Content total**: 64x26

### Key Findings

1. **Icon Count Increases**: From 4 icons to 6 icons (+2 icons)
   
2. **Ghost Icons Detected**: Icons with abnormal sizes:
   - `1x1` icons (appear in both before/after)
   - `1x26` icons (only in "after" state)
   
3. **IconBox Width Growth**: 42px → 46px (+4 pixels)
   - With spacing=2, each additional widget adds: width + spacing
   - 2 new 1x26 icons = 1 + 2 + 1 + 2 = 6 pixels? (but we only see 4px growth)
   
4. **The 1x1 Icons Are Not Being Removed**: These persist across updates and continue accumulating

5. **Widget Accumulation Pattern**:
   - Before: 2 real icons (20x26) + 2 ghost icons (1x1) = 4 total
   - After: 2 ghost (1x26) + 2 real (20x26) + 2 ghost (1x1) = 6 total

## Hypothesis

**The `updateWindowIcons()` method is NOT properly cleaning up all icon widgets before adding new ones.**

### Evidence:
1. The code claims to clear icons:
   ```cpp
   for (auto* img : m_iconImages) {
     m_iconBox.remove(*img);
     delete img;
   }
   m_iconImages.clear();
   ```

2. But the log shows icons persist (1x1 icons remain, new 1x26 appear)

3. **Problem**: The code only tracks `Gtk::Image*` in `m_iconImages`, but actually adds `Gtk::EventBox*` containers to the box:
   ```cpp
   auto* eventBox = new Gtk::EventBox();
   eventBox->add(*img);
   // ...
   m_iconBox.pack_start(*eventBox, false, false);
   m_iconImages.push_back(img);  // Only tracks the image, not the eventbox!
   ```

4. When clearing, it removes images from the vector and deletes them, but the EventBox containers remain in `m_iconBox`!

## The Bug

**Location**: `src/modules/hyprland/workspace.cpp:509-517` (updateWindowIcons)

**Issue**: Icon cleanup only removes/deletes `Gtk::Image` widgets but leaves `Gtk::EventBox` containers attached to `m_iconBox`. These orphaned EventBoxes accumulate and cause spacing growth.

## Additional Mystery

The 1x26 vs 1x1 sizes suggest GTK is trying to size these containers differently:
- 1x26: EventBox with no child (takes vertical space from parent)
- 1x1: Completely collapsed EventBox

This confirms orphaned EventBoxes remain in the widget tree.

## Fix Plan

### Option A: Track and Remove EventBoxes (Recommended)
1. Change `m_iconImages` to track EventBox containers instead of just images
2. Or add separate tracking: `std::vector<Gtk::EventBox*> m_iconEventBoxes`
3. In cleanup, remove and delete EventBoxes (which will auto-cleanup child images)

### Option B: Clear All Children
1. Instead of tracking, just remove all children from m_iconBox:
   ```cpp
   for (auto* child : m_iconBox.get_children()) {
     m_iconBox.remove(*child);
     delete child;
   }
   ```

## Questions to Verify (Need More Logging)

Before implementing fix, let's verify:

1. **Are EventBoxes really accumulating?**
   - Log `m_iconBox.get_children().size()` before/after cleanup
   - Log child widget types

2. **Why do some icons become 1x26 vs 1x1?**
   - May not matter if we fix the root cause, but curious

3. **Is m_iconImages vector accurate?**
   - Log `m_iconImages.size()` vs actual IconBox children count

## Recommended Next Steps

1. **Add diagnostic logging** to confirm EventBox accumulation hypothesis
2. **Implement Option B** (simpler, more robust - clears ALL children)
3. **Test** with window open/close actions
4. **Remove diagnostic logging** once confirmed fixed

## Code Location Reference

- `src/modules/hyprland/workspace.cpp:509-632` - updateWindowIcons()
- Line 509-517: Cleanup code (BUGGY)
- Line 620-629: Icon addition code (creates EventBoxes)
