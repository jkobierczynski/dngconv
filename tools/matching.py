# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Point matching between two pictures of the same scene, shared by the lens
tools: finds corresponding points with SIFT and fits how one picture maps
onto the other."""
import numpy as np
import cv2
from scipy.optimize import least_squares


def load(path, quarter_turns=0):
    image = cv2.imread(path, cv2.IMREAD_COLOR)
    if image is None:
        raise SystemExit("cannot read " + path)
    return np.rot90(image, quarter_turns).copy() if quarter_turns else image


def matches(reference, render):
    """Corresponding points (reference xy, render xy), each in its own pixels.

    The render is first brought to the reference's scale, where features
    match best; contrast is equalised because the two come from different
    tone curves."""
    href, wref = reference.shape[:2]
    hren, wren = render.shape[:2]
    scale = wref / wren
    small = cv2.resize(render, (int(round(wren * scale)), int(round(hren * scale))), interpolation=cv2.INTER_AREA)
    equalise = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))
    grey_a = equalise.apply(cv2.cvtColor(reference, cv2.COLOR_BGR2GRAY))
    grey_b = equalise.apply(cv2.cvtColor(small, cv2.COLOR_BGR2GRAY))
    sift = cv2.SIFT_create(nfeatures=40000, contrastThreshold=0.02)
    keys_a, desc_a = sift.detectAndCompute(grey_a, None)
    keys_b, desc_b = sift.detectAndCompute(grey_b, None)
    if desc_a is None or desc_b is None:
        return np.zeros((0, 2)), np.zeros((0, 2))
    points_a, points_b = [], []
    for pair in cv2.BFMatcher(cv2.NORM_L2).knnMatch(desc_a, desc_b, k=2):
        if len(pair) < 2:
            continue
        best, second = pair
        if best.distance < 0.7 * second.distance:
            a = np.array(keys_a[best.queryIdx].pt)
            b = np.array(keys_b[best.trainIdx].pt)
            if np.linalg.norm(a - b) < 0.08 * wref:  # the same place, roughly
                points_a.append(a)
                points_b.append(b / scale)
    return np.array(points_a).reshape(-1, 2), np.array(points_b).reshape(-1, 2)


def fit_scale_and_shift(points_a, points_b, size_a, size_b):
    """Fits b = centre_b + (a - centre_a) * (kx, ky) + (tx, ty), robustly.

    Camera previews are often squeezed slightly in one direction, hence the
    two scales. Returns a dict with the scales relative to the nominal ones,
    the shift, the median residual in pixels of b, and the median radial part
    of the residual in five zones from the centre to the corner."""
    wa, ha = size_a
    wb, hb = size_b
    centre_a = np.array([wa / 2, ha / 2])
    centre_b = np.array([wb / 2, hb / 2])
    half_diagonal = np.hypot(wb / 2, hb / 2)

    def model(q):
        return centre_b + (points_a - centre_a) * q[:2] + q[2:4]

    q = np.array([wb / wa, hb / ha, 0.0, 0.0])
    keep = np.ones(len(points_a), bool)
    for _ in range(4):
        q = least_squares(lambda p: (model(p) - points_b)[keep].ravel(), q, loss="soft_l1", f_scale=2.0).x
        error = np.linalg.norm(model(q) - points_b, axis=1)
        keep = error < max(4.5 * np.median(error[keep]), 2.0)
    residual = (model(q) - points_b)[keep]
    offset = points_b[keep] - centre_b
    radius = np.linalg.norm(offset, axis=1)
    # Positive: the point lies further from the centre than the fit predicts.
    radial = -np.sum(residual * offset, axis=1) / np.maximum(radius, 1e-9)
    zones = []
    edges = np.linspace(0, 1, 6)
    for low, high in zip(edges[:-1], edges[1:]):
        inside = (radius / half_diagonal >= low) & (radius / half_diagonal < high)
        zones.append(float(np.median(radial[inside])) if inside.sum() > 8 else None)
    return dict(matches=len(points_a), used=int(keep.sum()), scale_x=q[0] / (wb / wa), scale_y=q[1] / (hb / ha),
                shift=(q[2], q[3]), median=float(np.median(np.linalg.norm(residual, axis=1))), zones=zones)
