# Deren logo

Approved direction: a lowercase d shaped by a doorway and its projected light, paired with a forward-leaning deren wordmark. The project name comes from filmmaker Maya Deren, as described by the project author. The selected long projection joins the d's left foot; its base is intended to be horizontal, and the doorway lintel retains a modest slope.

## Assets

- `deren-logo-light.png`: red horizontal wordmark on a light background.
- `deren-logo-dark.png`: white horizontal wordmark on a dark background.
- `deren-mark.png`: compact red d symbol on a light background, with a shortened projection.
- `deren-mark-dark.png`: compact white d on a dark background, selected for the Windows executable icon.

Windows embeds `source/app/resources/deren.ico` through `deren.rc`. The ICO is converted from `deren-mark-dark.png` with Pillow and contains 16, 24, 32, 48, 64, 128 and 256 pixel images. CMake includes the resource only on Windows and tracks the ICO as a resource compilation dependency.

Windows interactive startup also displays the dark wordmark in a separate, responsive splash window until the first successful scene presentation. `source/app/resources/deren-splash.bmp` is a 720 × 243 RGB conversion of `deren-logo-dark.png`, embedded as bitmap resource 102. Regenerate it with Pillow using RGB conversion and proportional Lanczos resizing to width 720; CMake tracks it as a resource dependency. Captures, offscreen runs and `--version` skip the splash. Other platforms retain their existing startup behavior.

These are generated raster PNG assets with baked backgrounds, not transparent or vector masters. Keep their aspect ratios. The compact mark is intended for square placements. Final raster exports may contain small edge, color and alignment variations; mathematically exact geometry requires a subsequent vector master.

Created with the built-in image_gen tool on 2026-10-23. No external image-generation API or CLI was used. The failed transparent export was excluded.

## Final prompts

### Light wordmark

Production cleanup of the attached approved deren logo. ONE exact red logo on perfectly solid pure WHITE background. Preserve the reference's original d doorway symbol and 'eren' lettering, original generous clear white gap between the long underline and the letters, the filled left foot connecting the d to underline, and slightly slanted doorway lintel. Surgical correction: make the underline BOTTOM boundary absolutely horizontal at the same lowest y as the d bowl, ending in a pointed long taper beyond n. Top of underline slopes downward gently toward tip. Keep letter spacing and silhouette unchanged. Extremely clean sharp smooth geometric edges, uniform solid vivid red #ED151B throughout the logo with no texture or noise or gradients. Door and counters pure white. This is final flat logo artwork, no effects or simulated printed material. One horizontal logo, no labels or panels, comfortable white margins. Exact only text 'deren'.

### Dark wordmark

Create the DARK BACKGROUND VERSION of the attached final deren logo. Strict recolor only, preserve the logo geometry and spacing exactly: d doorway with moderately slanted lintel and connected left base, long thin taper under eren with horizontal bottom, exact same lowercase italic 'deren' lettering. Solid pure WHITE logo on uniform solid charcoal #17191D background. Same wide horizontal composition, same proportions, same clear margins. No gradient, texture, gloss, shadow, grain, glow, extra symbols or text. This must look like the exact white reversed counterpart of the supplied red logo, not a new design.

### Standalone mark

Create a compact STANDALONE LOWERCASE d SYMBOL derived faithfully from the attached final deren wordmark. Remove ALL eren letters. Keep the exact distinctive red d shape: broad rounded bowl on left flowing to tall sharply forward leaning ascender on right; doorway negative space with vertical jambs and a moderately down-right sloping lintel; solid connected red lower-left threshold with NO slit. Retain the ground light projection as a SHORT integrated rightward triangular taper, extending just a little beyond the d, not the long wordmark underline. Its bottom is one perfectly horizontal baseline shared with lowest d bowl, upper edge slopes toward right point. Keep the d silhouette dominant and the icon balanced in a square canvas, generous equal margins. Single solid vivid red symbol on perfectly pure white background. Flat crisp vector-like design with smooth edges. No letters except the d symbol, no text, no border, no rounded-square app tile, no gradients, shadows, texture or effects. This is the official compact mark counterpart of the supplied approved logo, not a redesign.
