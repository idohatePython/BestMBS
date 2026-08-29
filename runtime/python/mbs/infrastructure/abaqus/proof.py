# -*- coding: utf-8 -*-
"""Abaqus-independent 0.2% offset proof-stress calculations."""

from __future__ import division


def line_intersection(p1, p2, p3, p4):
    x1, y1 = p1
    x2, y2 = p2
    x3, y3 = p3
    x4, y4 = p4
    denom = (y4 - y3) * (x2 - x1) - (x4 - x3) * (y2 - y1)
    if abs(denom) < 1e-20:
        return None
    ua = ((x4 - x3) * (y1 - y3) - (y4 - y3) * (x1 - x3)) / denom
    ub = ((x2 - x1) * (y1 - y3) - (y2 - y1) * (x1 - x3)) / denom
    if 0 <= ua <= 1 and 0 <= ub <= 1:
        return (x1 + ua * (x2 - x1), y1 + ua * (y2 - y1))
    return None


def calculate_proof_curve(force3, disp1, disp2, disp3, wth, rep_z):
    n = min(len(force3), len(disp1), len(disp2), len(disp3))
    if n < 2:
        raise RuntimeError("Not enough history output data in ODB.")
    wth = float(wth)
    rep_z = int(rep_z)
    area_values = [(wth + disp1[i]) * (wth + disp2[i]) for i in range(n)]
    if any(abs(area) < 1e-20 for area in area_values):
        raise RuntimeError("Deformed cross-sectional area is zero.")
    stress = [force3[i] / area_values[i] for i in range(n)]
    strain = [disp3[i] / (wth * rep_z) for i in range(n)]
    modulus_index = next((i for i in range(1, n) if abs(strain[i]) > 1e-20), None)
    if modulus_index is None:
        raise RuntimeError("Cannot calculate elastic modulus from zero strain history.")
    e_eq = stress[modulus_index] / strain[modulus_index]
    offset = [e_eq * (value - 0.002) for value in strain]
    proof = None
    for i in range(n - 1, 0, -1):
        proof = line_intersection(
            (strain[i - 1], stress[i - 1]), (strain[i], stress[i]),
            (strain[i - 1], offset[i - 1]), (strain[i], offset[i]),
        )
        if proof:
            break
    return {
        "status": "ok" if proof else "no_intersection",
        "strain": strain,
        "stress": stress,
        "offset_stress": offset,
        "elastic_modulus": e_eq,
        "proof_strain": proof[0] if proof else None,
        "proof_strain_percent": proof[0] * 100.0 if proof else None,
        "proof_stress": proof[1] if proof else 0.0,
        "wth": wth,
        "rep_z": rep_z,
    }
