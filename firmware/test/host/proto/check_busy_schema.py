"""Validate only the new busy fixture and actual C builder output with the real schema."""
import argparse
import json
from pathlib import Path
import runpy

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source-root", type=Path, required=True)
parser.add_argument("--actual-ack", type=Path, required=True)
args = parser.parse_args()
root = args.source_root.resolve()
validation = runpy.run_path(str(root / "protocol/validate.py"))
schemas, registry = validation["load_registry"]()
validator = validation["Draft202012Validator"](schemas["ack.schema.json"], registry=registry,
                                               format_checker=validation["FormatChecker"]())
for path in (root / "protocol/examples/valid/ack-rejected-busy.json", args.actual_ack):
    message = json.loads(path.read_text())
    assert message["status"] == "rejected" and message["reason"] == "busy", path
    errors = list(validator.iter_errors(message))
    assert not errors, [error.message for error in errors]
    print("PASS busy ack real schema:", path)
malformed = json.loads(args.actual_ack.read_text())
malformed["extra"] = True
assert list(validator.iter_errors(malformed)), "additionalProperties must remain closed"
print("PASS busy ack still rejects unknown fields")
