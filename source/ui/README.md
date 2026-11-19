# RmlUi native integration

This frontend uses the vendored RmlUi 6.3 sources and the existing RHI. It does not require the ImGui plugin and may run alongside it. Scene ownership and the managed scripting ABI are unchanged.

## Build

RmlUi and FreeType are vendored under `source/third_party/` and built with the engine, so no installed prefix and no configure option are needed:

```powershell
cmake -S . -B build
cmake --build build --target deren test_rml_document
ctest --test-dir build -R test_rml_document --output-on-failure
```

FreeType is configured without external image/compression dependencies, and RmlUi without samples, the shell, Lottie, SVG, Lua or the debugger, so the build stays offline. The two standalone UI shaders are compiled like every other shader.

## Try the demo

Copy this directory's `demo` folder into a managed project's `ui` directory. Add these fields to its entry scene TOML:

```toml
[gui]
show = true
rml_document = "ui/hud.rml"
rml_font = "ui/LatoLatin-Regular.ttf"
# rml_fallback_font = "fonts/your-fallback.ttf"
```

Paths resolve relative to the project directory. Launch normally with `deren run <project-directory>`. Type in the field and press Apply. The native integration demo updates its status text and logs click/change events. `show` controls the existing debug overlay independently.

## Sizing

RmlUi receives the framebuffer size and the window's content scale, so author lengths in `dp` (density-independent pixels) instead of `px`: `width: 320dp` stays 320 logical pixels at 100%, 150% and 200% scaling, while `px` is a physical pixel and shrinks the layout on a scaled window.

Give the document an explicit root size with `body { width: 100%; height: 100%; }`. RmlUi resolves `position: absolute` against the containing block, and an unsized root leaves that box zero wide: elements anchored with `right:` or `bottom:` then land off the left or top edge and the whole layer stays invisible even though it renders correctly.

## Boundaries

- `deren.ui.rml_document`: one active document/context per process, main-thread lifecycle, text/value access and owned click/change events. Supports text, generated font atlases, PNG/common raster images, rectangular clipping, keyboard/text, mouse and scrolling.
- `deren.ui.rml_overlay`: final-output RHI rendering, content-addressed texture reuse and per-frame-slot geometry. Preparation occurs after that frame slot's fence has completed. There is no per-frame GPU wait_idle.
- The foreground debug panel owns overlapping pointer input. RmlUi retains an active drag until release, even outside its panel. Clicking the debug panel moves keyboard ownership away from RmlUi.
- Only text entry (`input`, `textarea`, `select`) takes the keyboard from the game; a focused button does not. Only a real widget under the pointer takes the mouse, so empty space over a full-screen document does not; decorate non-interactive layers with `pointer-events: none`.
- Game input receives unconsumed events; releases still reach it so dragging across the UI cannot leave buttons held. Text focus releases held game actions. Pointer coordinates account for framebuffer scaling.
- This first backend does not implement CSS transforms, non-rectangular clip masks, layer filters or backdrop effects. IME composition, clipboard integration and editor tooling are future work.

The bundled LatoLatin font retains its OFL license in demo/FONT-LICENSE.txt. RmlUi's MIT license is included beside this file.

## Managed documents and audio panel

C# can use `deren.rml_document.try_open(markup, font, out document)` with project-relative
paths, update element text/value/visibility, and poll copied click/change events with
`try_poll_event`. Dispose the document before opening another one. Calls belong to the
render thread, and old handles are invalidated when the script session ends. The existing
debug widget API remains separate. The optional module is named `rml_ui` (id 4); its table
is size-checked without changing the core scripting ABI.

The test_game audio panel is opt-in: pass `--audio-panel` to open it in the top right.
Without this flag, a configured document is left in place. Enter a path and press Load, then use
Play/Pause/Stop and the volume/position sliders. Loading does not autoplay. A command-line
path can also be supplied:

```powershell
.\build\deren.exe run .\build\source\scripting\test_game --audio-panel "E:/Music/song.flac"
```

The build copies the panel markup and the bundled font/license beside Game.dll. The panel's
behavior lives in `source/scripting/test_game/demo_audio_panel.cs`.
