# -*- coding: utf-8 -*-
"""Export read-only ODB deformation/Mises frames for the Qt player."""

from __future__ import print_function

import json
import os
import sys

from abaqus import *
from abaqusConstants import *
from caeModules import *


def runtime_args():
    if "--" in sys.argv:
        return sys.argv[sys.argv.index("--") + 1:]
    return [str(arg) for arg in sys.argv[1:] if not str(arg).lower().endswith(".py")]


def progress(message):
    print(">>> " + str(message))
    sys.stdout.flush()


def write_json(path, payload):
    temp = path + ".tmp"
    with open(temp, "w") as file_obj:
        json.dump(payload, file_obj, indent=2, sort_keys=True)
        file_obj.write("\n")
    if os.path.exists(path):
        os.remove(path)
    os.rename(temp, path)


def ensure_viewport():
    name = "Viewport: 1"
    if name not in session.viewports:
        session.Viewport(name=name)
    viewport = session.viewports[name]
    viewport.makeCurrent()
    try:
        viewport.maximize()
    except Exception:
        pass
    return viewport


def main():
    config_path = next((value for value in runtime_args() if value.lower().endswith(".json")), None)
    if not config_path:
        raise RuntimeError("A JSON export config is required.")
    with open(config_path, "r") as file_obj:
        config = json.load(file_obj)
    odb_path = os.path.abspath(config["odb_path"])
    output_dir = os.path.abspath(config["output_dir"])
    manifest_path = os.path.abspath(config.get("manifest_path", os.path.join(output_dir, "animation.json")))
    if not os.path.isfile(odb_path):
        raise RuntimeError("ODB file not found: %s" % odb_path)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    for name in os.listdir(output_dir):
        if name.startswith("frame_") and name.lower().endswith(".png"):
            os.remove(os.path.join(output_dir, name))

    progress("Opening ODB read-only for PNG frame export: %s" % odb_path)
    odb = session.openOdb(name=odb_path.replace("\\", "/"), readOnly=True)
    viewport = ensure_viewport()
    viewport.setValues(displayedObject=odb)
    try:
        viewport.view.setValues(session.views["Iso"])
    except Exception:
        pass
    viewport.odbDisplay.setPrimaryVariable(
        variableLabel="S", outputPosition=INTEGRATION_POINT,
        refinement=(INVARIANT, "Mises"),
    )
    viewport.odbDisplay.display.setValues(plotState=(CONTOURS_ON_DEF,))
    viewport.viewportAnnotationOptions.setValues(
        title=OFF, state=OFF, legend=ON, triad=ON, compass=ON,
    )
    try:
        session.pngOptions.setValues(imageSize=(int(config.get("width", 1280)), int(config.get("height", 720))))
    except Exception:
        pass

    step_name = config.get("step_name")
    if not step_name:
        step_name = "Step-1" if "Step-1" in odb.steps else list(odb.steps.keys())[0]
    frames = odb.steps[step_name].frames
    max_frames = max(1, int(config.get("max_frames", 240)))
    stride = max(1, int(config.get("stride", 1)))
    indices = list(range(0, len(frames), stride))
    if len(indices) > max_frames:
        scale = float(len(indices) - 1) / float(max_frames - 1) if max_frames > 1 else 0.0
        indices = sorted(set(int(round(i * scale)) for i in range(max_frames)))
    if frames and (not indices or indices[-1] != len(frames) - 1):
        indices.append(len(frames) - 1)

    exported = []
    for output_index, frame_index in enumerate(indices):
        viewport.odbDisplay.setFrame(step=step_name, frame=frame_index)
        base_name = "frame_%05d" % output_index
        base_path = os.path.join(output_dir, base_name)
        session.printToFile(fileName=base_path, format=PNG, canvasObjects=(viewport,))
        file_path = base_path + ".png"
        exported.append({
            "index": output_index,
            "odb_frame": frame_index,
            "time": float(frames[frame_index].frameValue),
            "file": os.path.basename(file_path),
        })
        progress("Exported frame %d/%d" % (output_index + 1, len(indices)))

    payload = {
        "schema_version": 1,
        "odb_path": odb_path,
        "step_name": step_name,
        "fps": int(config.get("fps", 12)),
        "frames": exported,
    }
    write_json(manifest_path, payload)
    odb.close()
    progress("PNG frame export complete: %s" % manifest_path)


if __name__ == "__main__":
    main()
