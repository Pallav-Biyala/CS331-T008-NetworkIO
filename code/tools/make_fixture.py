#!/usr/bin/env python3
"""Write a deterministic benchmark fixture: make_fixture.py PATH SIZE_BYTES.

The content is a SHA-256 counter stream, so every checkout produces the same
bytes (and the same checksum) without storing a large binary in Git, and the
data does not compress or repeat in a way that could flatter any backend.
"""
from __future__ import annotations

import hashlib
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    size = int(sys.argv[2])
    data = bytearray()
    counter = 0
    while len(data) < size:
        data += hashlib.sha256(b"cs331-filesrv-fixture-%d" % counter).digest()
        counter += 1
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_bytes(bytes(data[:size]))
    temp.replace(path)
    print(f"{path}: {size} bytes, sha256 {hashlib.sha256(data[:size]).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
