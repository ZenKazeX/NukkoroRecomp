"""Captures side by side: loading (PNG from the lifted renderer, PPM from the legacy one), bringing them to one size,
the difference image and its statistics.

The legacy renderer's capture is its EFB - the 4:3 guest frame it drew with a horizontally widened projection, which
its presenter stretches to the window's aspect - so "fit BEFORE to AFTER" resizes it to the other capture's size,
which is what that build showed on screen (docs/comparator/README.md "Captures").
"""
from __future__ import annotations

import math
from pathlib import Path

import numpy as np
from PIL import Image


def load(path) -> np.ndarray:
    with Image.open(Path(path)) as image:
        return np.asarray(image.convert('RGB'), dtype=np.uint8).copy()


def fit(image: np.ndarray, width: int, height: int) -> np.ndarray:
    if image.shape[1] == width and image.shape[0] == height:
        return image
    resized = Image.fromarray(image).resize((width, height), Image.Resampling.BILINEAR)
    return np.asarray(resized, dtype=np.uint8).copy()


def pair(before: np.ndarray, after: np.ndarray, fit_before: bool = True):
    """The two images at one size (the AFTER capture's when fitting, else the smaller common crop)."""
    if fit_before:
        return fit(before, after.shape[1], after.shape[0]), after
    h = min(before.shape[0], after.shape[0])
    w = min(before.shape[1], after.shape[1])
    return before[:h, :w], after[:h, :w]


def difference(a: np.ndarray, b: np.ndarray, threshold: int = 8, amplify: int = 4):
    """(difference image RGB, stats dict). The image shows max-channel |a-b| amplified, on a dark-blue-to-white
    ramp; pixels above the threshold get a red tint."""
    delta = np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2)
    pixels = int(delta.size)
    changed = int(np.count_nonzero(delta))
    over = int(np.count_nonzero(delta > threshold))
    stats = {
        'width': int(a.shape[1]), 'height': int(a.shape[0]), 'pixels': pixels,
        'identical': changed == 0,
        'max': int(delta.max()),                     # maximum channel error
        'mean': float(delta.mean()),                 # mean of the per-pixel maximum channel error
        'mean_abs': float(np.abs(a.astype(np.int16) - b.astype(np.int16)).mean()),  # mean over all channels
        'changed_pixels': changed,
        'changed_percent': changed * 100.0 / pixels if pixels else 0.0,
        'over_threshold_pixels': over,
        'over_threshold': over * 100.0 / pixels if pixels else 0.0,
        'threshold': threshold,
    }
    mse = float(((a.astype(np.float64) - b.astype(np.float64)) ** 2).mean())
    stats['psnr'] = float('inf') if mse == 0 else 10.0 * math.log10(255.0 ** 2 / mse)
    v = np.clip(delta.astype(np.int32) * amplify, 0, 255).astype(np.uint8)
    image = np.stack([v, v, np.maximum(v, 40)], axis=2)
    hot = delta > threshold
    image[hot, 0] = 255
    image[hot, 1] = (image[hot, 1] * 0.4).astype(np.uint8)
    image[hot, 2] = (image[hot, 2] * 0.4).astype(np.uint8)
    return image, stats


def blend(a: np.ndarray, b: np.ndarray, alpha: float) -> np.ndarray:
    """alpha 0 = a (BEFORE), 1 = b (AFTER)."""
    return (a.astype(np.float32) * (1.0 - alpha) + b.astype(np.float32) * alpha + 0.5).astype(np.uint8)


def side_by_side(a: np.ndarray, b: np.ndarray, gap: int = 8) -> np.ndarray:
    h = max(a.shape[0], b.shape[0])
    out = np.full((h, a.shape[1] + gap + b.shape[1], 3), 24, dtype=np.uint8)
    out[:a.shape[0], :a.shape[1]] = a
    out[:b.shape[0], a.shape[1] + gap:] = b
    return out


def save_png(image: np.ndarray, path):
    Image.fromarray(image).save(Path(path))


def describe(stats: dict) -> str:
    if stats.get('identical'):
        return f"IDENTICAL ({stats['width']}x{stats['height']}, 0 changed pixels)"
    psnr = stats['psnr']
    return (f"{stats['width']}x{stats['height']}: {stats['changed_pixels']} changed pixels "
            f"({stats['changed_percent']:.2f}%), {stats['over_threshold_pixels']} over {stats['threshold']} "
            f"({stats['over_threshold']:.2f}%); max channel error {stats['max']}, mean error "
            f"{stats['mean_abs']:.3f} (per-pixel max {stats['mean']:.3f}); PSNR {psnr:.2f} dB")
