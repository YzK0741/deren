# RmlUi native integration

This optional frontend uses RmlUi 6.3 and the existing RHI. It does not require the ImGui plugin and may run alongside it. Scene ownership and the managed scripting ABI are unchanged.

## Build

Install RmlUi 6.3 and FreeType using the same compiler and C++ runtime as deren. Set the installed prefix when configuring:

```powershell
cmake -S . -B build -DDEREN_ENABLE_RMLUI=ON -DCMAKE_PREFIX_PATH=C:/deps/rmlui
cmake --build build --target deren test_rml_document
ctest --test-dir build -R test_rml_document --output-on-failure
```

The default option is OFF. The two standalone UI shaders are compiled in both modes so the canonical shader list and standalone compiler scripts remain identical; no RmlUi library is needed in OFF mode.

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

## Boundaries

- `deren.ui.rml_document`: one active document/context per process, main-thread lifecycle, text/value access and owned click/change events. Supports text, generated font atlases, PNG/common raster images, rectangular clipping, keyboard/text, mouse and scrolling.
- `deren.ui.rml_overlay`: final-output RHI rendering, content-addressed texture reuse and per-frame-slot geometry. Preparation occurs after that frame slot's fence has completed. There is no per-frame GPU wait_idle.
- The foreground debug panel owns overlapping pointer input. RmlUi retains an active drag until release, even outside its panel. Clicking the debug panel moves keyboard ownership away from RmlUi.
- Game input receives unconsumed events; releases still reach it so dragging across the UI cannot leave buttons held. Text focus releases held game actions. Pointer coordinates account for framebuffer scaling.
- This first backend does not implement CSS transforms, non-rectangular clip masks, layer filters or backdrop effects. IME composition, clipboard integration, C# document bindings and editor tooling are future work.

The bundled LatoLatin font retains its OFL license in demo/FONT-LICENSE.txt. RmlUi's MIT license is included beside this file.
