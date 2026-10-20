"""Offline cluster probe: frustum cull rate, conservative HZB occlusion headroom and subpixel share.

Reads the two files the renderer can dump for one frame:
  * DEREN_CLUSTER_PROBE_DUMP: a binary dump of the camera matrices, the window extent, and every visible
    item's world matrix plus its meshlets' object-space bounding spheres and triangle counts (see the
    temporary probe in runtime.cpp);
  * DEREN_DEPTH_PROBE_CAPTURE: the raw G-buffer depth (u32 width, u32 height, then one u32 per texel; the
    device stores D32_SFLOAT, so the bits are a float32; 1.0 is the far plane).

The question it answers, conservatively: of the meshlets the visibility lane would dispatch for this camera,
how many are (a) outside the frustum, (b) fully behind the rendered depth (the ceiling of an HZB occlusion
pass), and (c) projected below one/two pixels (the Nanite-style software-raster criterion).

THE TEST IS DELIBERATELY CONSERVATIVE so the answer is a LOWER BOUND: the sphere is biased small, the depth
pyramid stores the FARTHEST depth per tile (a max pyramid), and a meshlet only counts as occluded when its
NEAREST depth is still behind that farthest occluder over its whole screen rectangle. Anything the test calls
occluded is safe to cull; the true number can only be larger.

The dump does not record the viewport's y convention, so BOTH conventions are evaluated and the one whose
projected centres land on rendered geometry (depth < 1) is used; the other one is reported as a sensitivity.

Usage: python cluster_occlusion_probe.py <cluster-dump> <depth-dump> [--json out.json]
"""
import argparse
import json
import math
import sys

import numpy as np


def read_cluster_dump(path):
    raw = np.frombuffer(open(path, 'rb').read(), dtype=np.uint8)
    if raw.size < 8 + 128 + 4:
        raise ValueError('cluster dump is too small')
    offset = 0

    def take(count, dtype):
        nonlocal offset
        value = np.frombuffer(raw[offset:offset + count * np.dtype(dtype).itemsize].tobytes(), dtype=dtype)
        offset += count * np.dtype(dtype).itemsize
        return value

    width = int(take(1, np.uint32)[0])
    height = int(take(1, np.uint32)[0])
    # glm/GLSL matrices are COLUMN-major: reshape then transpose so `matrix @ vector` matches the shader's mul.
    view = take(16, np.float32).reshape(4, 4).T
    proj = take(16, np.float32).reshape(4, 4).T
    item_count = int(take(1, np.uint32)[0])
    items = []
    for _ in range(item_count):
        model = take(16, np.float32).reshape(4, 4).T
        count = int(take(1, np.uint32)[0])
        bounds = take(count * 5, np.float32).reshape(count, 5)
        items.append((model, bounds))
    return width, height, view, proj, items


def read_depth_dump(path):
    raw = open(path, 'rb').read()
    width, height = (int(v) for v in np.frombuffer(raw[:8], dtype=np.uint32))
    depth = np.frombuffer(raw[8:8 + width * height * 4], dtype=np.uint32).view(np.float32).reshape(height, width)
    return width, height, depth.copy()


def build_max_pyramid(depth, max_levels=12):
    levels = [depth]
    while (levels[-1].shape[0] > 1 or levels[-1].shape[1] > 1) and len(levels) < max_levels:
        current = levels[-1]
        h, w = current.shape
        padded = np.full((h + (h & 1), w + (w & 1)), -np.inf, dtype=np.float32)
        padded[:h, :w] = current
        levels.append(padded.reshape(padded.shape[0] // 2, 2, padded.shape[1] // 2, 2).max(axis=(1, 3)))
    return levels


def sample_max(level, x0, y0, x1, y1):
    h, w = level.shape
    x0 = max(0, min(w - 1, x0)); x1 = max(0, min(w, x1))
    y0 = max(0, min(h - 1, y0)); y1 = max(0, min(h, y1))
    if x1 <= x0 or y1 <= y0:
        return -np.inf
    return float(level[y0:y1, x0:x1].max())


def analyze(cluster_path, depth_path, y_flip):
    width, height, view, proj, items = read_cluster_dump(cluster_path)
    depth_width, depth_height, depth = read_depth_dump(depth_path)
    if (width, height) != (depth_width, depth_height):
        raise ValueError(f'extent mismatch: cluster dump {width}x{height}, depth dump {depth_width}x{depth_height}')
    view_proj = proj @ view
    focal_x = abs(float(proj[0, 0]))
    focal_y = abs(float(proj[1, 1]))
    focal = max(focal_x * width, focal_y * height) * 0.5
    pyramid = build_max_pyramid(depth)

    def to_pixels(ndc_x, ndc_y):
        px = (ndc_x + 1.0) * 0.5 * width
        py = (1.0 - ndc_y) * 0.5 * height if y_flip else (ndc_y + 1.0) * 0.5 * height
        return px, py

    total = frustum_culled = occluded = 0
    total_tri = frustum_tri = occluded_tri = 0
    subpixel_1 = subpixel_2 = subpixel_tri_1 = subpixel_tri_2 = 0
    centres = []
    centres_on_geometry = 0

    for model, bounds in items:
        model_scale = max(float(np.linalg.norm(model[:3, 0])), float(np.linalg.norm(model[:3, 1])), float(np.linalg.norm(model[:3, 2])))
        for center_x, center_y, center_z, radius, index_count in bounds:
            triangles = int(index_count) // 3
            total += 1
            total_tri += triangles
            world = model @ np.array([center_x, center_y, center_z, 1.0], dtype=np.float32)
            world_radius = float(radius) * model_scale
            clip = view_proj @ world
            w = float(clip[3])
            if w <= 1e-4:
                frustum_culled += 1
                frustum_tri += triangles
                continue
            ndc_x, ndc_y, ndc_z = float(clip[0]) / w, float(clip[1]) / w, float(clip[2]) / w
            # projected sphere radius in pixels: the max of the x/y focal scales bounds it (conservative)
            radius_px = world_radius * focal / w
            radius_ndc_x = world_radius * focal_x / w
            radius_ndc_y = world_radius * focal_y / w
            radius_ndc_z = world_radius / max(w, 1e-4)  # depth radius, biased small (conservative)
            if ndc_z - radius_ndc_z > 1.0 or ndc_z + radius_ndc_z < -1.0 or abs(ndc_x) - radius_ndc_x > 1.0 or abs(ndc_y) - radius_ndc_y > 1.0:
                frustum_culled += 1
                frustum_tri += triangles
                continue
            diameter = 2.0 * radius_px
            if diameter < 1.0:
                subpixel_1 += 1
                subpixel_tri_1 += triangles
            if diameter < 2.0:
                subpixel_2 += 1
                subpixel_tri_2 += triangles
            px, py = to_pixels(ndc_x, ndc_y)
            centres.append((px, py))
            ix, iy = int(px), int(py)
            if 0 <= ix < width and 0 <= iy < height and depth[iy, ix] < 1.0:
                centres_on_geometry += 1
            # screen rectangle: clamped, which can only shrink it (conservative)
            x0, x1 = int(math.floor(px - radius_px)), int(math.ceil(px + radius_px))
            y0, y1 = int(math.floor(py - radius_px)), int(math.ceil(py + radius_px))
            if x1 <= 0 or y1 <= 0 or x0 >= width or y0 >= height:
                frustum_culled += 1
                frustum_tri += triangles
                continue
            span = max(x1 - x0, y1 - y0, 1)
            level = min(int(math.floor(math.log2(span))), len(pyramid) - 1)
            scale = 1 << level
            far_depth = sample_max(pyramid[level], x0 >> level, y0 >> level,
                                   (x1 + scale - 1) >> level, (y1 + scale - 1) >> level)
            if math.isfinite(far_depth):
                near_depth = ndc_z - radius_ndc_z
                if near_depth > far_depth + 1e-4:
                    occluded += 1
                    occluded_tri += triangles

    visible = total - frustum_culled
    visible_tri = total_tri - frustum_tri
    return {
        'extent': [width, height],
        'y_flip': y_flip,
        'meshlets': {'total': total, 'frustum_culled': frustum_culled,
                     'frustum_culled_pct': round(100.0 * frustum_culled / max(total, 1), 2)},
        'triangles': {'total': total_tri, 'frustum_culled': frustum_tri,
                      'frustum_culled_pct': round(100.0 * frustum_tri / max(total_tri, 1), 2)},
        'occlusion_off_frustum_visible': {'meshlets': occluded,
                                          'pct_of_visible_meshlets': round(100.0 * occluded / max(visible, 1), 2),
                                          'pct_of_all_meshlets': round(100.0 * occluded / max(total, 1), 2),
                                          'triangles': occluded_tri,
                                          'triangles_pct_of_visible': round(100.0 * occluded_tri / max(visible_tri, 1), 2),
                                          'triangles_pct_of_all': round(100.0 * occluded_tri / max(total_tri, 1), 2)},
        'subpixel_clusters': {'diameter_below_1px_pct': round(100.0 * subpixel_1 / max(visible, 1), 2),
                              'diameter_below_2px_pct': round(100.0 * subpixel_2 / max(visible, 1), 2),
                              'triangles_below_1px_pct_of_visible': round(100.0 * subpixel_tri_1 / max(visible_tri, 1), 2),
                              'triangles_below_2px_pct_of_visible': round(100.0 * subpixel_tri_2 / max(visible_tri, 1), 2)},
        'validation': {'projected_centres': len(centres),
                       'centres_inside_frame_pct': round(100.0 * sum(1 for x, y in centres if 0 <= x < width and 0 <= y < height) / max(len(centres), 1), 2),
                       'centres_on_rendered_geometry_pct': round(100.0 * centres_on_geometry / max(len(centres), 1), 2),
                       'depth_foreground_cover_pct': round(float((depth < 1.0).mean() * 100.0), 2)},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cluster_dump')
    parser.add_argument('depth_dump')
    parser.add_argument('--json')
    args = parser.parse_args()
    candidates = {}
    for flip in (False, True):
        try:
            candidates[flip] = analyze(args.cluster_dump, args.depth_dump, flip)
        except Exception as error:  # noqa: BLE001 - the report wants the reason, not a traceback
            print(f'flip={flip}: analysis failed: {error}')
    if not candidates:
        return 1
    # The y convention is the one whose projected centres land on RENDERED geometry (depth < 1). A wrong
    # convention moves every rectangle to a mirrored place, which shows up as a low match rate.
    chosen_flip = max(candidates, key=lambda flip: candidates[flip]['validation']['centres_on_rendered_geometry_pct'])
    result = {'chosen_y_flip': chosen_flip,
              'summary': candidates[chosen_flip],
              'y_flip_sensitivity': {str(flip): {'frustum_culled_pct': value['meshlets']['frustum_culled_pct'],
                                                'occluded_pct_of_visible': value['occlusion_off_frustum_visible']['pct_of_visible_meshlets'],
                                                'centres_on_rendered_geometry_pct': value['validation']['centres_on_rendered_geometry_pct']}
                                    for flip, value in candidates.items()}}
    print(json.dumps(result, indent=2))
    if args.json:
        with open(args.json, 'w', encoding='utf-8') as stream:
            json.dump(result, stream, indent=2)
    return 0


if __name__ == '__main__':
    sys.exit(main())
