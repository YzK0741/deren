// Adapted from KhronosGroup/ToneMapping, Copyright 2024 The Khronos Group, Inc.
// SPDX-License-Identifier: Apache-2.0
// See source/third_party/khronos_tonemapping for original, license and pinned revision.
// Input color is non-negative and resides in the Linear Rec. 709 color space.
// Output color is also Linear Rec. 709, but in the [0, 1] range.

float3 pbr_neutral_tone_mapping( float3 color ) {
  const float startCompression = 0.8 - 0.04;
  const float desaturation = 0.15;

  float x = min(color.r, min(color.g, color.b));
  float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
  color -= offset;

  float peak = max(color.r, max(color.g, color.b));
  if (peak < startCompression) return color;

  const float d = 1. - startCompression;
  float newPeak = 1. - d * d / (peak + d - startCompression);
  color *= newPeak / peak;

  float g = 1. - 1. / (desaturation * (peak - newPeak) + 1.);
  return lerp(color, newPeak * float3(1, 1, 1), g);
}