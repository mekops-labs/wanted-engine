#!/usr/bin/env python3
"""Refuse a launch config that carries deployment configuration.

A device receives its identity and its control-plane and registry addresses
from a provisioning blob, so an image ships with neither. A config that names
one is a template someone will copy, which is how a placeholder identity once
reached a deployed board.

A network address may still be pinned by hand at deployment time — on OpenWRT
through UCI, which is rendered into the launch config before the engine starts.
That is a deployment's choice and lives outside this repository.
"""

import json
import pathlib
import sys

# Env vars carrying device identity. Substring match: the key half of
# SHERIFF_STATE_KEY_<id> varies per device.
IDENTITY_ENVS = ("SHERIFF_DEVICE_ID", "SHERIFF_STATE_KEY_", "SHERIFF_MARSHAL_KEY_")

# Socket names a provisioning blob supplies.
PROVISIONED_SOCKETS = ("manager", "registry")

# A scheme naming a physical port rather than a network endpoint. A blob cannot
# supply a UART, so a config is the only place such a manager can be named.
HARDWARE_SCHEMES = ("serial",)


def violations(path):
    try:
        doc = json.loads(path.read_text())
    except (OSError, ValueError) as err:
        yield f"unreadable: {err}"
        return

    params = doc.get("supervisor", {}).get("params", {})

    for env in params.get("envs", []):
        for ident in IDENTITY_ENVS:
            if isinstance(env, str) and env.startswith(ident):
                yield (
                    f"envs[] names {env.split('=')[0]} — identity comes from the "
                    f"provisioning blob, never from an image"
                )

    for sock in params.get("sockets", []):
        name = sock.get("name", "")
        address = sock.get("address", "")
        if name not in PROVISIONED_SOCKETS:
            continue
        scheme = address.split("://")[0] if "://" in address else ""
        if scheme in HARDWARE_SCHEMES:
            continue
        yield (
            f"sockets[] names {name!r} at {address!r} — a blob supplies this "
            f"address, so an image that hardcodes one deploys to a single site"
        )


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    failed = 0

    for path in sorted((root / "configs").glob("*.json")):
        for problem in violations(path):
            print(f"{path}: {problem}", file=sys.stderr)
            failed += 1

    if failed:
        print(
            f"\n{failed} launch-config violation(s). "
            f"See docs/platform-guide.md, 'Selecting the compiled-in launch config'.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
