#!/usr/bin/env python3
"""Regression tests for malformed UTF-8 output-buffer accounting."""

import os
import re
import subprocess
import sys
import tempfile

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")

CASES = {
    "orphan continuation bytes": b"\x80\x81\xbf\n",
    "alternating orphan continuations and ASCII": (b"\x80a" * 8192) + b"\n",
    "invalid lead bytes": b"\xc0\xc1\xf5\xfe\xff\n",
    "truncated sequences": b"\xe2\n\xf0\x9f\n\xed\xa0\n",
}


def check_output(name, source, expected, output):
    plain = ANSI.sub(b"", output)
    if plain != expected:
        raise AssertionError(
            f"{name} ({source}) was not preserved: expected {expected[:40]!r}, "
            f"got {plain[:40]!r}"
        )


def main():
    binary = os.path.abspath(sys.argv[1])
    env = os.environ.copy()
    env.update({"LOLCAT_OFFSET": "123.456", "LOLCAT_THREADS": "4", "LOLCAT_CHUNK": "1024"})

    with tempfile.TemporaryDirectory() as tmp:
        for name, data in CASES.items():
            path = os.path.join(tmp, name.replace(" ", "-") + ".bin")
            with open(path, "wb") as stream:
                stream.write(data)

            file_output = subprocess.run(
                [binary, "-F", path], capture_output=True, check=True, env=env
            ).stdout
            check_output(name, "file", data, file_output)

            stdin_output = subprocess.run(
                [binary, "-F"], input=data, capture_output=True, check=True, env=env
            ).stdout
            check_output(name, "stdin", data, stdin_output)
            print(f"ok   {name}")


if __name__ == "__main__":
    main()
