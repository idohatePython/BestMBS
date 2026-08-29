"""Atomically rename a sample record and its artifact directory."""

from __future__ import annotations

import argparse
import json
import os
import sqlite3
from pathlib import Path


def quote_identifier(value: str) -> str:
    return '"' + value.replace('"', '""') + '"'


def replace_json_references(root: Path, replacements: tuple[tuple[str, str], ...]) -> int:
    changed = 0
    for path in root.rglob("*.json"):
        try:
            original = path.read_text(encoding="utf-8")
            json.loads(original)
        except (OSError, UnicodeError, json.JSONDecodeError):
            continue
        updated = original
        for old, new in replacements:
            updated = updated.replace(old, new)
        if updated == original:
            continue
        temporary = path.with_suffix(path.suffix + ".rename-tmp")
        temporary.write_text(updated, encoding="utf-8")
        os.replace(temporary, path)
        changed += 1
    return changed


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument("--old-id", required=True)
    parser.add_argument("--new-id", required=True)
    parser.add_argument("--new-serial", required=True, type=int)
    parser.add_argument("--old-dir", required=True, type=Path)
    parser.add_argument("--new-dir", required=True, type=Path)
    parser.add_argument("--backup", required=True, type=Path)
    args = parser.parse_args()

    if not args.old_dir.is_dir():
        raise SystemExit(f"source sample directory is missing: {args.old_dir}")
    if args.new_dir.exists():
        raise SystemExit(f"target sample directory already exists: {args.new_dir}")

    source = sqlite3.connect(args.database)
    source.execute("PRAGMA foreign_keys=ON")
    if source.execute("SELECT 1 FROM samples WHERE sample_id=?", (args.old_id,)).fetchone() is None:
        source.close()
        raise SystemExit(f"source sample is missing: {args.old_id}")
    if source.execute("SELECT 1 FROM samples WHERE sample_id=?", (args.new_id,)).fetchone():
        source.close()
        raise SystemExit(f"target sample already exists: {args.new_id}")

    args.backup.parent.mkdir(parents=True, exist_ok=True)
    backup = sqlite3.connect(args.backup)
    try:
        source.backup(backup)
    finally:
        backup.close()
        source.close()

    args.old_dir.rename(args.new_dir)
    connection = sqlite3.connect(args.database, timeout=30)
    connection.execute("PRAGMA foreign_keys=ON")
    old_backslash = str(args.old_dir)
    new_backslash = str(args.new_dir)
    old_forward = old_backslash.replace("\\", "/")
    new_forward = new_backslash.replace("\\", "/")
    try:
        with connection:
            columns = [row[1] for row in connection.execute("PRAGMA table_info(samples)")
                       if row[1] != "id"]
            expressions = []
            values: list[object] = []
            for column in columns:
                if column == "sample_id":
                    expressions.append("?")
                    values.append(args.new_id)
                elif column == "serial":
                    expressions.append("?")
                    values.append(args.new_serial)
                elif column in ("artifact_dir", "last_simulation_dir"):
                    expressions.append(
                        f"replace(replace({quote_identifier(column)}, ?, ?), ?, ?)"
                    )
                    values.extend((old_backslash, new_backslash, old_forward, new_forward))
                else:
                    expressions.append(quote_identifier(column))
            sql = (
                f"INSERT INTO samples({','.join(map(quote_identifier, columns))}) "
                f"SELECT {','.join(expressions)} FROM samples WHERE sample_id=?"
            )
            values.append(args.old_id)
            connection.execute(sql, values)

            tables = [row[0] for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"
            )]
            referencing_tables: list[str] = []
            for table in tables:
                foreign_keys = connection.execute(
                    f"PRAGMA foreign_key_list({quote_identifier(table)})"
                ).fetchall()
                if any(row[2] == "samples" and row[3] == "sample_id" for row in foreign_keys):
                    connection.execute(
                        f"UPDATE {quote_identifier(table)} SET sample_id=? WHERE sample_id=?",
                        (args.new_id, args.old_id),
                    )
                    referencing_tables.append(table)

            connection.execute(
                "UPDATE optimization_observations SET observation_id=? WHERE observation_id=?",
                (f"obs-{args.new_id}", f"obs-{args.old_id}"),
            )
            connection.execute(
                "UPDATE artifacts SET uri=replace(replace(uri,?,?),?,?) WHERE sample_id=?",
                (old_backslash, new_backslash, old_forward, new_forward, args.new_id),
            )
            connection.execute(
                "UPDATE runs SET request_json=replace(replace(replace(request_json,?,?),?,?),?,?) "
                "WHERE sample_id=?",
                (args.old_id, args.new_id, old_backslash.replace("\\", "\\\\"),
                 new_backslash.replace("\\", "\\\\"), old_forward, new_forward, args.new_id),
            )
            connection.execute(
                "UPDATE metrics SET details_json=replace(replace(replace(details_json,?,?),?,?),?,?) "
                "WHERE sample_id=?",
                (args.old_id, args.new_id, old_backslash.replace("\\", "\\\\"),
                 new_backslash.replace("\\", "\\\\"), old_forward, new_forward, args.new_id),
            )
            connection.execute("DELETE FROM samples WHERE sample_id=?", (args.old_id,))
    except Exception:
        connection.close()
        if args.new_dir.exists() and not args.old_dir.exists():
            args.new_dir.rename(args.old_dir)
        raise
    finally:
        connection.close()

    replacements = (
        (args.old_id, args.new_id),
        (old_backslash.replace("\\", "\\\\"), new_backslash.replace("\\", "\\\\")),
        (old_backslash, new_backslash),
        (old_forward, new_forward),
    )
    json_count = replace_json_references(args.new_dir, replacements)
    print(json.dumps({
        "old_id": args.old_id,
        "new_id": args.new_id,
        "new_serial": args.new_serial,
        "directory": str(args.new_dir),
        "json_files_updated": json_count,
        "backup": str(args.backup),
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
