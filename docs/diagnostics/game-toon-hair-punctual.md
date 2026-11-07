# Hair punctual specular alignment

The captured viewer used the shared GGX punctual-light path for hair. Game
frame 1706, EID 975, pixel shader 37900 instead uses the primary strand lobe
for punctual specular lighting. References below are original DXBC instruction
numbers, also retained as comments in the exported `readable.hlsl`.

- 1191-1193: diffuse energy is scaled by 0.96; dielectric F0 is `0.04 * P.g`.
- 2037-2055: normalize `V + L`, evaluate the strand sine lobe with exponent
  200, multiply by `P.g`, sample the specular palette, and apply edge fade.
- 2056-2070: apply F0, anisotropy strength, gain 5, the light's specular scale,
  attenuation, `saturate(N.L)`, and the existing line gain.
- 2019 and 2035-2036: the rim branch retains unscaled base colour and skips
  this specular lobe.

The main and punctual lights share the existing palette/strand evaluator.
Only hair enables the optional punctual hair parameters; other families retain
their existing GGX path. Main-light broad highlights and manual diffuse tint
are unchanged. This aligns the local lobe, not the game's complete lighting,
screen-space shadows, or frame composition.

Run `python scripts/gates/game_toon_hair_response.py --build-dir build-release`.
The GPU probe checks material routing, wrap and specular modes, peak and
off-axis response, zero factors, back-facing light, and the non-hair fallback.
Its numerical lobe checks use the no-palette white fallback, so they do not
establish texture-level equivalence with the captured game's palette.
