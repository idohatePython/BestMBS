# -*- coding: utf-8 -*-
"""Read an existing ODB and calculate the 0.2% offset proof point.

GUI mode receives a JSON config and writes a JSON result without touching CSV.
Legacy mode keeps the historical command-line behavior.
"""

from __future__ import print_function

import csv
import json
import os
import sys


def _script_dir():
    """Resolve the source directory under both Abaqus and normal argv forms."""
    for arg in sys.argv:
        text = str(arg)
        if text.lower().startswith("nogui="):
            text = text.split("=", 1)[1]
        if text.lower().endswith(".py"):
            return os.path.dirname(os.path.abspath(text))
    return os.getcwd()


SCRIPT_DIR = _script_dir()
SRC_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, '..', '..', '..', '..'))
if SRC_ROOT not in sys.path:
    sys.path.insert(0, SRC_ROOT)

# ODB post-processing deliberately runs under ``abaqus python`` rather than
# the CAE kernel.  Importing the ``abaqus`` session module here would force a
# CAE license and fails in the lightweight ODB runtime; odbAccess is sufficient.
import odbAccess

from mbs.infrastructure.abaqus.proof import calculate_proof_curve as calculate_proof_curve_shared


def progress(message):
    print(">>> " + str(message))
    sys.stdout.flush()


def runtime_args():
    if "--" in sys.argv:
        return sys.argv[sys.argv.index("--") + 1:]
    values = []
    for arg in sys.argv[1:]:
        text = str(arg)
        if text.lower().endswith(".py") or text.lower().startswith("nogui="):
            continue
        values.append(text)
    return values


def get_history_outputs(step):
    preferred = "Node ASSEMBLY.2"
    regions = []
    if preferred in step.historyRegions:
        regions.append(step.historyRegions[preferred])
    regions.extend(step.historyRegions.values())
    for region in regions:
        outputs = region.historyOutputs
        if all(name in outputs for name in ("RF3", "U1", "U2", "U3")):
            return outputs
    raise RuntimeError("Cannot find RP history outputs RF3/U1/U2/U3 in ODB.")


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


def process_odb(odb_path, wth=10.0, rep_z=3):
    odb_path = os.path.abspath(str(odb_path))
    if not os.path.isfile(odb_path):
        raise RuntimeError("ODB file not found: %s" % odb_path)
    progress("Post-processing ODB read-only: %s" % odb_path)
    odb = odbAccess.openOdb(odb_path, readOnly=True)
    try:
        step = odb.steps["Step-1"] if "Step-1" in odb.steps else odb.steps[list(odb.steps.keys())[0]]
        outputs = get_history_outputs(step)
        result = calculate_proof_curve_shared(
            [row[1] for row in outputs["RF3"].data],
            [row[1] for row in outputs["U1"].data],
            [row[1] for row in outputs["U2"].data],
            [row[1] for row in outputs["U3"].data],
            float(wth), int(rep_z),
        )
        result["odb_path"] = odb_path
        return result
    finally:
        odb.close()


def write_json(path, payload):
    path = os.path.abspath(str(path))
    parent = os.path.dirname(path)
    if parent and not os.path.isdir(parent):
        os.makedirs(parent)
    temp = path + ".tmp"
    with open(temp, "w") as file_obj:
        json.dump(payload, file_obj, indent=2, sort_keys=True)
        file_obj.write("\n")
    if os.path.exists(path):
        os.remove(path)
    os.rename(temp, path)


def legacy_write_csv(csv_path, result):
    with open(csv_path, "r") as file_obj:
        rows = list(csv.reader(file_obj))
    rows[-1][4] = "{:.3f}".format(-float(result["proof_stress"]))
    rows[-1][5] = (
        "nan" if result["proof_strain_percent"] is None
        else "{:.3f}".format(float(result["proof_strain_percent"]))
    )
    with open(csv_path, "w", newline="") as file_obj:
        csv.writer(file_obj).writerows(rows)


def main():
    args = runtime_args()
    config_path = next((value for value in args if str(value).lower().endswith(".json")), None)
    if config_path:
        with open(config_path, "r") as file_obj:
            config = json.load(file_obj)
        result = process_odb(config["odb_path"], config.get("wth", 10.0), config.get("rep_z", 3))
        result["sample_id"] = config.get("sample_id", "")
        write_json(config["output_json"], result)
        progress("Structured postprocess result: %s" % config["output_json"])
        return

    odb_path = r"C:\temp\job-intlck-tpms.odb"
    csv_path = next((value for value in args if str(value).lower().endswith(".csv")), None)
    if csv_path is None:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        csv_path = os.path.abspath(os.path.join(script_dir, "..", "data", "mbs_guess.csv"))
    result = process_odb(odb_path, 10.0, 3)
    legacy_write_csv(csv_path, result)
    progress("Legacy result written to %s" % csv_path)


if __name__ == "__main__":
    main()
