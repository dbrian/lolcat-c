#!/usr/bin/env python3
"""
Byte-for-byte conformance check against lolcat-ultra.

Both tools derive their starting hue from their own pid, so the same input
paints a different rainbow on every run and the outputs can't be diffed
directly. To pin it down we launch lolcat-ultra through `sh -c 'echo $$; exec
...'` — exec keeps the pid — and then re-run lolcat-c with LOLCAT_OFFSET set to
the hue that pid implies, so the two runs are directly comparable.

Usage: conformance.py <lolcat-c> <lolcat-ultra> <input-file>...
"""
import os
import subprocess
import sys


def offset_for_pid(pid):
    """lolcat-ultra's starting hue: Knuth multiplicative hash of the pid."""
    h = (pid * 2654435769) & 0xFFFFFFFF
    return h / 4294967295.0 * 1000.0


def run_ultra(ultra, path):
    """Run `ultra -F path`, returning (pid, stdout)."""
    r, w = os.pipe()
    os.set_inheritable(w, True)
    proc = subprocess.Popen(
        ["sh", "-c", f'echo $$ >&{w}; exec "$0" -F "$1"', ultra, path],
        stdout=subprocess.PIPE,
        stdin=subprocess.DEVNULL,
        pass_fds=(w,),
    )
    os.close(w)
    pid = int(os.read(r, 32).strip())
    os.close(r)
    out = proc.stdout.read()
    proc.stdout.close()
    return pid, out, proc.wait()


def main():
    lolcat_c, ultra = sys.argv[1], sys.argv[2]
    failures = 0

    for path in sys.argv[3:]:
        name = path.rsplit("/", 1)[-1]
        pid, out_u, rc = run_ultra(ultra, path)
        if rc != 0:
            print(f"     FAIL {name}: lolcat-ultra exited {rc}")
            failures += 1
            continue

        out_c = subprocess.run(
            [lolcat_c, "-F", path],
            capture_output=True,
            check=True,
            env={"LOLCAT_OFFSET": repr(offset_for_pid(pid)), "PATH": "/usr/bin:/bin"},
        ).stdout

        if out_c == out_u:
            print(f"     ok {name}: {len(out_u)} bytes identical (pid {pid})")
        else:
            failures += 1
            n = min(len(out_c), len(out_u))
            at = next((i for i in range(n) if out_c[i] != out_u[i]), n)
            print(f"     FAIL {name}: differs at byte {at} (len {len(out_c)} vs {len(out_u)})")
            print(f"       lolcat-c    : {out_c[max(0, at - 20):at + 40]!r}")
            print(f"       lolcat-ultra: {out_u[max(0, at - 20):at + 40]!r}")

    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
