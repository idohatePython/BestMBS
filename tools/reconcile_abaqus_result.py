"""Register a completed Abaqus result that predates the task/run lifecycle fix."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sqlite3
from datetime import datetime, timezone
from pathlib import Path


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def stable_id(prefix: str, sample_id: str, sample_dir: Path) -> str:
    digest = hashlib.sha1(f"{sample_id}|{sample_dir}".encode("utf-8")).hexdigest()[:12]
    return f"{prefix}-{digest}"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument("--sample-id", required=True)
    parser.add_argument("--sample-dir", required=True, type=Path)
    parser.add_argument("--backup", required=True, type=Path)
    args = parser.parse_args()

    sample_dir = args.sample_dir.resolve()
    proof_path = sample_dir / "proof-result.json"
    odb_path = sample_dir / "job-intlck-tpms.odb"
    manifest_path = sample_dir / "animation" / "animation.json"
    config_path = sample_dir / "simulation-config.json"
    for path in (proof_path, odb_path, manifest_path):
        if not path.is_file():
            raise SystemExit(f"missing completed Abaqus artifact: {path}")

    proof = json.loads(proof_path.read_text(encoding="utf-8"))
    proof_stress = float(proof["proof_stress"])
    if proof.get("status") != "ok" or not math.isfinite(proof_stress) or proof_stress < 0:
        raise SystemExit("proof-result.json is not a valid successful result")
    request_json = config_path.read_text(encoding="utf-8") if config_path.is_file() else "{}"
    json.loads(request_json)

    args.backup.parent.mkdir(parents=True, exist_ok=True)
    source = sqlite3.connect(args.database)
    backup = sqlite3.connect(args.backup)
    try:
        source.backup(backup)
    finally:
        backup.close()
        source.close()

    connection = sqlite3.connect(args.database, timeout=30)
    connection.execute("PRAGMA foreign_keys=ON")
    row = connection.execute(
        "SELECT project_id,lmd,mu,kpa,bta FROM samples WHERE sample_id=?", (args.sample_id,)
    ).fetchone()
    if row is None:
        connection.close()
        raise SystemExit(f"sample not found: {args.sample_id}")
    project_id, lmd, mu, kpa, bta = row
    timestamp = utc_now()
    run_id = stable_id("run-recovered", args.sample_id, sample_dir)
    task_id = f"{run_id}-workflow"

    artifacts = (
        ("abaqus_odb", odb_path),
        ("postprocess_result", proof_path),
        ("animation_manifest", manifest_path),
    )
    try:
        with connection:
            connection.execute(
                """INSERT INTO runs(run_id,project_id,sample_id,kind,status,request_json,error,
                   created_at,started_at,finished_at) VALUES(?,?,?,?,?,?,?,?,?,?)
                   ON CONFLICT(run_id) DO UPDATE SET status=excluded.status,
                   request_json=excluded.request_json,error='',finished_at=excluded.finished_at""",
                (run_id, project_id, args.sample_id, "abaqus_workflow_recovered", "succeeded",
                 request_json, "", timestamp, timestamp, timestamp),
            )
            connection.execute(
                """INSERT INTO tasks(task_id,project_id,run_id,sample_id,kind,status,progress,error,
                   created_at,started_at,finished_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)
                   ON CONFLICT(task_id) DO UPDATE SET status='succeeded',progress=1,error='',
                   finished_at=excluded.finished_at,updated_at=excluded.updated_at""",
                (task_id, project_id, run_id, args.sample_id, "abaqus_workflow", "succeeded", 1.0,
                 "", timestamp, timestamp, timestamp, timestamp),
            )
            for kind, path in artifacts:
                artifact_id = stable_id(f"artifact-{kind}", args.sample_id, sample_dir)
                connection.execute(
                    """INSERT INTO artifacts(artifact_id,project_id,sample_id,run_id,kind,uri,
                       size_bytes,exists_flag,created_at) VALUES(?,?,?,?,?,?,?,?,?)
                       ON CONFLICT(run_id,kind,uri) DO UPDATE SET size_bytes=excluded.size_bytes,
                       exists_flag=1""",
                    (artifact_id, project_id, args.sample_id, run_id, kind, str(path),
                     path.stat().st_size, 1, timestamp),
                )
            metric_id = stable_id("metric-proof-stress", args.sample_id, sample_dir)
            connection.execute(
                """INSERT INTO metrics(metric_id,project_id,sample_id,run_id,name,value,unit,
                   details_json,created_at) VALUES(?,?,?,?,?,?,?,?,?)
                   ON CONFLICT(metric_id) DO UPDATE SET value=excluded.value,
                   details_json=excluded.details_json""",
                (metric_id, project_id, args.sample_id, run_id, "proof_stress", proof_stress,
                 "MPa", json.dumps(proof, ensure_ascii=False, separators=(",", ":")), timestamp),
            )
            connection.execute(
                """UPDATE samples SET status='simulation_succeeded',result=?,
                   last_simulation_dir=?,last_error='',updated_at=? WHERE sample_id=?""",
                (-proof_stress, str(sample_dir), timestamp, args.sample_id),
            )
            connection.execute(
                """INSERT INTO optimization_observations(observation_id,project_id,sample_id,
                   lmd,mu,kpa,bta,objective,created_at) VALUES(?,?,?,?,?,?,?,?,?)
                   ON CONFLICT(sample_id) DO UPDATE SET lmd=excluded.lmd,mu=excluded.mu,
                   kpa=excluded.kpa,bta=excluded.bta,objective=excluded.objective""",
                (f"obs-{args.sample_id}", project_id, args.sample_id, lmd, mu, kpa, bta,
                 -proof_stress, timestamp),
            )
    finally:
        connection.close()

    print(json.dumps({
        "sample_id": args.sample_id,
        "run_id": run_id,
        "task_id": task_id,
        "proof_stress_mpa": proof_stress,
        "artifacts": len(artifacts),
        "backup": str(args.backup),
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
