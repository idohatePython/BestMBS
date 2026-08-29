# -*- coding: utf-8 -*-
"""Runtime metadata helpers compatible with normal and Abaqus Python."""

from __future__ import print_function

import csv
import json
import os
from datetime import datetime


MESH_METADATA_FILENAME = "mesh_metadata.json"
RUN_MANIFEST_FILENAME = "run_manifest.json"


def mesh_metadata_path(mesh_dir):
    return os.path.join(str(mesh_dir), MESH_METADATA_FILENAME)


def write_json_atomic(path, payload):
    path = str(path)
    parent = os.path.dirname(path)
    if parent and not os.path.isdir(parent):
        os.makedirs(parent)
    temp_path = path + ".tmp"
    with open(temp_path, "w") as file_obj:
        json.dump(payload, file_obj, indent=2, sort_keys=True)
        file_obj.write("\n")
    if os.path.exists(path):
        os.remove(path)
    os.rename(temp_path, path)


def write_mesh_metadata(mesh_dir, params, random_offset, design=None, files=None):
    """Write all information required to reproduce and consume a mesh."""
    if random_offset is None:
        return None
    rnd_x, rnd_y = random_offset
    lmd, mu, kpa, bta = params
    metadata = {
        "schema_version": 2,
        "created_at": datetime.now().isoformat(timespec="seconds"),
        "lmd": float(lmd), "mu": float(mu), "kpa": float(kpa), "bta": float(bta),
        "rnd_x": float(rnd_x), "rnd_y": float(rnd_y),
    }
    if design:
        metadata.update(dict(design))
    if files:
        metadata["files"] = dict(files)
    write_json_atomic(mesh_metadata_path(mesh_dir), metadata)
    return metadata


def write_run_manifest(run_dir, payload):
    manifest = dict(payload)
    manifest.setdefault("schema_version", 1)
    manifest.setdefault("updated_at", datetime.now().isoformat(timespec="seconds"))
    path = os.path.join(str(run_dir), RUN_MANIFEST_FILENAME)
    write_json_atomic(path, manifest)
    return path


def read_json(path):
    if not os.path.exists(str(path)):
        return None
    try:
        with open(str(path), "r") as file_obj:
            return json.load(file_obj)
    except Exception:
        return None


def read_mesh_metadata(mesh_dir):
    metadata = read_json(mesh_metadata_path(mesh_dir))
    if not metadata or "rnd_x" not in metadata or "rnd_y" not in metadata:
        return None
    return metadata


def read_run_manifest(run_dir):
    return read_json(os.path.join(str(run_dir), RUN_MANIFEST_FILENAME))


def _is_blank(value):
    return value is None or str(value).strip() == ""


def fill_latest_csv_offset_from_metadata(csv_path, mesh_dir, progress=None):
    """Legacy helper retained for the command-line workflow."""
    metadata = read_mesh_metadata(mesh_dir)
    if metadata is None or not os.path.exists(str(csv_path)):
        return False
    with open(str(csv_path), "r", newline="") as file_obj:
        reader = csv.DictReader(file_obj)
        fieldnames = list(reader.fieldnames or [])
        rows = list(reader)
    if not rows:
        return False
    changed = False
    for column in ("rnd_x", "rnd_y"):
        if column not in fieldnames:
            fieldnames.append(column)
            changed = True
        if _is_blank(rows[-1].get(column)):
            rows[-1][column] = "%.4f" % float(metadata[column])
            changed = True
    if not changed:
        return False
    with open(str(csv_path), "w", newline="") as file_obj:
        writer = csv.DictWriter(file_obj, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    if progress:
        progress(
            "Mesh metadata offsets: rnd_x=%.4f, rnd_y=%.4f"
            % (float(metadata["rnd_x"]), float(metadata["rnd_y"]))
        )
    return True
