#!/usr/bin/env bash
# Throughput benchmark: lolcat-c vs lolcat-ultra (and the original Ruby lolcat).
#
# Uses the same corpora and the same measurement shape as lolcat-ultra's
# bench/compare.sh, so the numbers are directly comparable:
#   ascii — 10M short pure-ASCII lines (~100 MB), the fused path
#   utf8  — ~2.1M mixed multibyte lines, the general path
# Input comes from pre-generated files rather than a pipe, so the tool under
# test is the only bottleneck; `cat` gives the I/O floor.
#
# QUICK=1 skips the Ruby comparison. Requires hyperfine and python3.
set -euo pipefail
cd "$(dirname "$0")/.."

DATA=target/bench
CBIN=${CBIN:-./lolcat-c}
ULTRA=${ULTRA:-../lolcat-ultra/target/release/lolcat-ultra}
RUBY_LOLCAT=${RUBY_LOLCAT:-lolcat}
QUICK=${QUICK:-0}

ASCII_LINES=10000000
RUBY_LINES=50000

[ -x "$CBIN" ] || { echo "error: $CBIN not found — run 'make build' first" >&2; exit 1; }
command -v hyperfine >/dev/null || { echo "error: hyperfine not installed (brew install hyperfine)" >&2; exit 1; }

mkdir -p "$DATA"

# ---- corpora (identical to lolcat-ultra's) ---------------------------------
if [ ! -s "$DATA/ascii.txt" ]; then
  echo "generating $DATA/ascii.txt ..." >&2
  # `yes` dies of SIGPIPE when head stops reading; that is expected here.
  (set +o pipefail; yes "test line" | head -n "$ASCII_LINES" > "$DATA/ascii.txt")
fi

if [ ! -s "$DATA/utf8.txt" ]; then
  echo "generating $DATA/utf8.txt ..." >&2
  printf '%s\n' \
    'naïve café résumé — ünïcödé everywhere' \
    'こんにちは世界 rainbow 🌈 test' \
    'a plain ascii line mixed into the corpus' \
    'Ω≈ç√∫˜µ≤≥÷ symbols and emoji 🎉✨' > "$DATA/utf8.txt"
  while [ "$(wc -l < "$DATA/utf8.txt")" -lt 2000000 ]; do
    cat "$DATA/utf8.txt" "$DATA/utf8.txt" > "$DATA/utf8.tmp" && mv "$DATA/utf8.tmp" "$DATA/utf8.txt"
  done
fi

CMDS=(-n "cat/ascii" "cat $DATA/ascii.txt > /dev/null"
      -n "c/ascii"   "$CBIN -F $DATA/ascii.txt > /dev/null"
      -n "c/utf8"    "$CBIN -F $DATA/utf8.txt > /dev/null")
if [ -x "$ULTRA" ]; then
  CMDS+=(-n "ultra/ascii" "$ULTRA -F $DATA/ascii.txt > /dev/null"
         -n "ultra/utf8"  "$ULTRA -F $DATA/utf8.txt > /dev/null")
fi

hyperfine --warmup 2 --runs 10 --export-json "$DATA/main.json" "${CMDS[@]}"

RUBY_JSON=""
if [ "$QUICK" != "1" ] && command -v "$RUBY_LOLCAT" >/dev/null; then
  head -n "$RUBY_LINES" "$DATA/ascii.txt" > "$DATA/ascii-small.txt"
  head -n "$RUBY_LINES" "$DATA/utf8.txt" > "$DATA/utf8-small.txt"
  hyperfine --warmup 1 --runs 5 --export-json "$DATA/ruby.json" \
    -n "ruby/ascii" "$RUBY_LOLCAT -f $DATA/ascii-small.txt > /dev/null" \
    -n "ruby/utf8"  "$RUBY_LOLCAT -f $DATA/utf8-small.txt > /dev/null"
  RUBY_JSON="$DATA/ruby.json"
fi

# ---- report ----------------------------------------------------------------
python3 - "$DATA" "$RUBY_JSON" <<'PY'
import json, os, sys

data, ruby_json = sys.argv[1], sys.argv[2]
runs = {}
files = [os.path.join(data, "main.json")] + ([ruby_json] if ruby_json else [])
for f in files:
    for r in json.load(open(f))["results"]:
        runs[r["command"]] = r

def corpus(path):
    with open(path, "rb") as fh:
        lines = sum(chunk.count(b"\n") for chunk in iter(lambda: fh.read(1 << 22), b""))
    return {"lines": lines, "bytes": os.path.getsize(path)}

full = {k: corpus(f"{data}/{k}.txt") for k in ("ascii", "utf8")}
small = {k: corpus(f"{data}/{k}-small.txt") for k in ("ascii", "utf8")
         if os.path.exists(f"{data}/{k}-small.txt")}

rows = [("cat/ascii", full["ascii"]), ("c/ascii", full["ascii"]), ("c/utf8", full["utf8"]),
        ("ultra/ascii", full["ascii"]), ("ultra/utf8", full["utf8"])]
rows += [(f"ruby/{k}", v) for k, v in small.items()]

print(f"\n{'command':<14}{'input':>10}{'mean':>10}{'stddev':>9}{'Mlines/s':>10}{'MB/s':>9}")
lps = {}
for name, c in rows:
    r = runs.get(name)
    if not r:
        continue
    lps[name] = c["lines"] / r["mean"]
    print(f"{name:<14}{c['lines']/1e6:>8.2g}M{r['mean']:>9.3f}s{r['stddev']:>8.3f}s"
          f"{lps[name]/1e6:>10.2f}{c['bytes']/r['mean']/1e6:>9.0f}")

print()
for kind in ("ascii", "utf8"):
    c, u, rb = lps.get(f"c/{kind}"), lps.get(f"ultra/{kind}"), lps.get(f"ruby/{kind}")
    if c and u:
        print(f"{kind:<6} lolcat-c vs lolcat-ultra: {c/u:.2f}x")
    if c and rb:
        print(f"{kind:<6} lolcat-c vs Ruby lolcat : {c/rb:,.0f}x")
PY
