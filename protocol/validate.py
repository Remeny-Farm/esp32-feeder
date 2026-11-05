#!/usr/bin/env python3
"""Validate every example against its schema.

The schema is chosen by the file name prefix: `cmd-scatter.json` is checked
against `schemas/cmd.schema.json`. Files in examples/valid must pass. Files in
examples/invalid must fail. Exit code is non-zero if any file does not behave.

Needs: pip install jsonschema rfc3339-validator
"""
import json
import sys
from datetime import datetime
from pathlib import Path

from jsonschema import Draft202012Validator, FormatChecker
from referencing import Registry, Resource

ROOT = Path(__file__).resolve().parent
SCHEMAS = ROOT / "schemas"
NAMES = ("cmd", "ack", "event", "state")
MAX_TTL_S = 300  # expires_at - issued_at
# A file named `deviceid-*.json` holds one JSON string, checked against the
# device id definition in common.schema.json.
DEVICE_ID = "deviceid"


def load_registry():
    schemas = {p.name: json.loads(p.read_text()) for p in SCHEMAS.glob("*.schema.json")}
    registry = Registry()
    for schema in schemas.values():
        registry = registry.with_resource(schema["$id"], Resource.from_contents(schema))
    return schemas, registry


def parse_ts(value):
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def duplicates(items):
    names = [i["hopper"] for i in items]
    return len(names) != len(set(names))


def semantic_errors(name, msg):
    """Rules that JSON Schema cannot express."""
    errors = []
    if name == "cmd":
        ttl = (parse_ts(msg["expires_at"]) - parse_ts(msg["issued_at"])).total_seconds()
        if ttl < 0:
            errors.append("issued_at must not be after expires_at")
        elif ttl > MAX_TTL_S:
            errors.append(f"expires_at - issued_at is {ttl:.0f} s, the maximum is {MAX_TTL_S} s")
        if "doses" in msg and duplicates(msg["doses"]):
            errors.append("doses: duplicate hopper")
    elif name == "event":
        if "doses" in msg and duplicates(msg["doses"]):
            errors.append("doses: duplicate hopper")
    elif name == "state":
        if "hoppers" in msg and duplicates(msg["hoppers"]):
            errors.append("hoppers: duplicate hopper")
    return errors


def errors_for(name, validator, path):
    try:
        msg = json.loads(path.read_text())
    except json.JSONDecodeError as exc:
        return [f"not JSON: {exc}"]
    errors = [e.message for e in validator.iter_errors(msg)]
    if not errors:  # semantic checks need a schema-valid message
        errors = semantic_errors(name, msg)
    return errors


def main():
    schemas, registry = load_registry()
    validators = {}
    for name in NAMES:
        schema = schemas[f"{name}.schema.json"]
        Draft202012Validator.check_schema(schema)
        validators[name] = Draft202012Validator(
            schema, registry=registry, format_checker=FormatChecker()
        )
    validators[DEVICE_ID] = Draft202012Validator(
        {"$ref": f"{schemas['common.schema.json']['$id']}#/$defs/device_id"},
        registry=registry,
        format_checker=FormatChecker(),
    )

    failures = 0
    checked = 0
    for kind, want_valid in (("valid", True), ("invalid", False)):
        for path in sorted((ROOT / "examples" / kind).glob("*.json")):
            name = path.name.split("-", 1)[0]
            rel = path.relative_to(ROOT)
            if name not in validators:
                print(f"FAIL {rel}: unknown schema prefix '{name}'")
                failures += 1
                continue
            checked += 1
            errors = errors_for(name, validators[name], path)
            if want_valid and errors:
                print(f"FAIL {rel}: should be valid but: {errors[0]}")
                failures += 1
            elif not want_valid and not errors:
                print(f"FAIL {rel}: should be invalid but passed")
                failures += 1
            else:
                print(f"ok   {rel}")

    print(f"\n{checked} examples checked, {failures} failure(s)")
    return 1 if failures or checked == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
