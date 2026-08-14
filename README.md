# hypr-tab-drag

A [Hyprland](https://hyprland.org) plugin that lets you reorder grouped windows by
dragging their groupbar tabs with the mouse — the way browser tabs work.

Hyprland can already reorder tabs with `movegroupwindow`, but only from the keyboard.
Dragging a tab with `SUPER` held moves the whole window out of the group instead. This
plugin adds the missing gesture: press a tab, drag sideways, drop it where you want it.

No modifier key. Just press and drag, like Chrome — and the tab slides under the cursor
as you go, rather than jumping between slots.

## Install

With [hyprpm](https://wiki.hypr.land/Plugins/Using-Plugins/):

```sh
hyprpm add https://github.com/zoan37/hypr-tab-drag
hyprpm enable tab-drag
```

To load it on every start, add to `hyprland.conf`:

```
exec-once = hyprpm reload -n
```

### Building manually

Requires the Hyprland headers (`/usr/include/hyprland`, shipped with the `hyprland`
package on Arch):

```sh
make
hyprctl plugin load "$PWD/tab-drag.so"
```

## Configuration

```ini
plugin {
    tab-drag {
        # set to false to disable the gesture without unloading the plugin
        enabled = true
    }
}
```

The gesture follows your existing groupbar layout, so `group:groupbar:stacked`,
`gaps_in`, `gaps_out` and `keep_upper_gap` are all respected automatically.

## How it works

No compositor code is vendored. The plugin hooks four functions — two to drive the
gesture, two to draw it.

**The gesture:**

- `CInputManager::onMouseButton` — after Hyprland has handled a left press, the plugin
  checks whether the cursor landed on a groupbar tab and arms the gesture.
- `CInputManager::mouseMoveUnified` — once the pointer has travelled more than 4px, each
  motion maps the cursor onto a tab slot and steps the dragged window toward it with
  `CGroup::swapWithNext` / `swapWithLast`.

Hooking motion globally is what makes the drag keep working after the pointer leaves the
groupbar — decoration input is otherwise only delivered while the cursor is inside the
decoration's box. A press that never travels 4px stays an ordinary click, so tab
switching is unchanged.

**The slide:**

The groupbar draws every tab in a single `draw()` call and its geometry is private, so
there is no seam to slide one tab through. But the drawing is not immediate: `draw()`
emits `CRectPassElement` / `CTexPassElement` into the render pass, and both expose a
mutable box.

- `CHyprGroupBarDecoration::draw` — marks which slot is being dragged, and by how far.
- `IHyprRenderer::addPassElement` — holds back the elements landing in that slot; they
  are re-emitted after the rest of the bar with their box translated to the cursor.

Re-emitting last is also what puts the dragged tab above its neighbours while it slides
over them. Elements are matched on the same box that is later translated, so the test
and the move cannot disagree about coordinate space.

The offset of the press within the tab is kept, so the tab tracks the cursor rather than
snapping an edge to it, and it is clamped to the bar so a tab cannot be dragged out of
its own groupbar.

## Behaviour and limitations

- **`SUPER` + drag is untouched.** Dragging a tab with a modifier still moves the window
  out of the group, exactly as before — and the plugin stands down for the whole gesture
  if a window drag is running.
- **Middle-click-close is untouched.**
- Both horizontal and `stacked` groupbars are supported.
- The gesture is dropped if the group shrinks below two windows, if the group is
  destroyed, or if another mouse button arrives mid-drag. It does not attempt to cover
  every way the compositor can take the pointer away — session lock, forced button
  release and input capture are all handled from inside the compositor, which a plugin
  cannot hook without pinning itself to many more internal symbols. In practice the
  worst case is a gesture that ends without a visible drop; the next click clears it.

## Compatibility

Built against Hyprland's plugin API, which is tied to the exact compositor build. After
a Hyprland update, run:

```sh
hyprpm update
```

All four hooked functions are exported in Hyprland's dynamic symbol table. If upstream
renames or changes the signature of any of them, the plugin refuses to load with a
notification rather than misbehave. It also checks the compositor's build hash against
the headers it was compiled with, and reports both if they differ.

## License

BSD 3-Clause. See [LICENSE](LICENSE).
