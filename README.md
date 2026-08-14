# hypr-tab-drag

A [Hyprland](https://hyprland.org) plugin that lets you reorder grouped windows by
dragging their groupbar tabs with the mouse — the way browser tabs work.

Hyprland can already reorder tabs with `movegroupwindow`, but only from the keyboard.
Dragging a tab with `SUPER` held moves the whole window out of the group instead. This
plugin adds the missing gesture: press a tab, drag sideways, drop it where you want it.

No modifier key. Just press and drag, like Chrome.

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

The plugin hooks two compositor functions:

- `CInputManager::onMouseButton` — after Hyprland has handled a left press, the plugin
  checks whether the cursor landed on a groupbar tab and arms the gesture.
- `CInputManager::mouseMoveUnified` — once the pointer has travelled more than 4px, each
  motion maps the cursor onto a tab slot and steps the dragged window toward it with
  `CGroup::swapWithNext` / `swapWithLast`.

Hooking motion globally is what makes the drag keep working after the pointer leaves the
groupbar — decoration input is otherwise only delivered while the cursor is inside the
decoration's box.

A press that never travels 4px stays an ordinary click, so tab switching is unchanged.

## Behaviour and limitations

- **Tabs snap, they don't slide.** The tab jumps a slot at a time as the cursor crosses
  each boundary, rather than sliding smoothly under the cursor. Smooth sliding needs
  control of the groupbar's `draw()`, which a plugin can only get by replacing the
  decoration wholesale.
- **`SUPER` + drag is untouched.** Dragging a tab with a modifier still moves the window
  out of the group, exactly as before.
- **Middle-click-close is untouched.**
- The gesture is dropped if the group shrinks below two windows, if the group is
  destroyed, or if another mouse button arrives mid-drag. It does not attempt to cover
  every way the compositor can take the pointer away — session lock, forced button
  release and input capture are all handled from inside the compositor, which a plugin
  cannot hook without pinning itself to many more internal symbols.

## Compatibility

Built against Hyprland's plugin API, which is tied to the exact compositor build. After
a Hyprland update, run:

```sh
hyprpm update
```

The two hooked functions are exported in Hyprland's dynamic symbol table. If upstream
renames or changes the signature of either, the plugin will refuse to load with a
notification rather than misbehave.

## License

BSD 3-Clause. See [LICENSE](LICENSE).
