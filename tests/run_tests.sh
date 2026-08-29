#!/usr/bin/env bash
# Correctness suite for lolcat-c.
#
# Three kinds of check:
#   1. Text preservation — stripping the ANSI sequences must give back the
#      input (with tabs expanded and a trailing newline guaranteed).
#   2. Invariance — output must not depend on thread count, chunk size, or
#      whether the input arrived via mmap or a pipe.
#   3. Conformance — byte-for-byte agreement with lolcat-ultra, if it's built.
set -uo pipefail
cd "$(dirname "$0")/.."

BIN=${BIN:-./lolcat-c}
ULTRA=${ULTRA:-../lolcat-ultra/target/release/lolcat-ultra}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0 fail=0
ok()   { printf 'ok   %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf 'FAIL %s\n' "$1"; fail=$((fail+1)); }

# Strip ANSI escape sequences, leaving plain text.
strip_ansi() { perl -pe 's/\e\[[0-9;]*[A-Za-z]//g'; }

# --- 1. text preservation ---------------------------------------------------

check_text() {
  local name=$1 input=$2 expected=$3
  shift 3
  local got
  got=$(printf '%s' "$input" | $BIN -F "$@" | strip_ansi)
  # command substitution eats trailing newlines on both sides, so this
  # compares the meaningful content
  if [ "$got" = "$expected" ]; then ok "$name"; else
    bad "$name"
    printf '     want: %q\n     got : %q\n' "$expected" "$got"
  fi
}

check_text "ascii preserved"          $'Hello, world!\nThe quick brown fox.\n' $'Hello, world!\nThe quick brown fox.'
check_text "no trailing newline"      'no newline at end'                       'no newline at end'
check_text "unicode preserved"        $'Hello 世界 🌈 Привет مرحبا café naïve\n' $'Hello 世界 🌈 Привет مرحبا café naïve'
check_text "curly quotes"             $'I’ve got “curly quotes”\n' $'I’ve got “curly quotes”'
check_text "emoji preserved"          $'Emojis: 😀 🎉 ✨ 🚀 👨‍👩‍👧‍👦\n'          $'Emojis: 😀 🎉 ✨ 🚀 👨‍👩‍👧‍👦'
check_text "tabs expanded"            $'col1\tcol2\tcol3\n'                     'col1        col2        col3'
check_text "blank lines"              $'\n\na\n\n'                              $'\n\na'
check_text "crlf stripped"            $'dos line\r\nsecond\r\n'                 $'dos line\nsecond'
check_text "lone cr kept"             $'a\rb\n'                                 $'a\rb'
check_text "256 color mode"           $'Hello, 世界!\n'                          'Hello, 世界!' -f 0.04
check_text "low frequency"            $'batched color runs here\n'              'batched color runs here' -f 0.0001 -s 100

# All printable ASCII on one line
printable=$(python3 -c 'print("".join(chr(c) for c in range(0x20,0x7f)))')
check_text "all printable ascii" "$printable"$'\n' "$printable"

# Long mixed line that crosses internal buffer sizes
long=$(python3 -c 'print("abcédef’ghi世jkl\U0001F308mno " * 4000)')
check_text "long mixed line" "$long"$'\n' "$long"

# Embedded ANSI passthrough: the sequence must survive intact
esc_out=$(printf 'a\e[1mb\e[0mc\n' | $BIN -F | grep -c $'\e\[1m')
[ "$esc_out" = "1" ] && ok "embedded ansi passthrough" || bad "embedded ansi passthrough"

# --- 2. invariance ----------------------------------------------------------

# A corpus big enough to span many chunks, with a mix of content types.
python3 - "$TMP" <<'PY'
import sys, random
random.seed(7)
lines = []
pool = ["test line", "naïve café résumé — ünïcödé", "こんにちは世界 rainbow 🌈",
        "a\tb\tc", "plain ascii", "", "Ω≈ç√∫˜µ≤≥÷ 🎉", "x" * 300]
for i in range(60000):
    lines.append(random.choice(pool))
open(sys.argv[1] + "/mixed.txt", "w").write("\n".join(lines) + "\n")
open(sys.argv[1] + "/ascii.txt", "w").write("test line\n" * 200000)
PY

export LOLCAT_OFFSET=123.456

for corpus in ascii mixed; do
  f="$TMP/$corpus.txt"
  LOLCAT_THREADS=1 $BIN -F "$f" > "$TMP/ref"
  for cfg in "LOLCAT_THREADS=8 LOLCAT_CHUNK=8192" \
             "LOLCAT_THREADS=3 LOLCAT_CHUNK=65536" \
             "LOLCAT_THREADS=8 LOLCAT_CHUNK=1000000"; do
    env $cfg $BIN -F "$f" > "$TMP/out"
    if cmp -s "$TMP/ref" "$TMP/out"; then ok "$corpus invariant: $cfg"
    else bad "$corpus invariant: $cfg"; fi
  done
  # stdin (streaming path) must agree with mmap
  LOLCAT_THREADS=4 LOLCAT_CHUNK=8192 $BIN -F < "$f" > "$TMP/out"
  if cmp -s "$TMP/ref" "$TMP/out"; then ok "$corpus invariant: stdin"
  else bad "$corpus invariant: stdin"; fi
done

# Lines far longer than any internal buffer, via both input paths. The stream
# reader has to grow its buffer, and the pread workers have to extend their
# window past the nominal chunk end.
python3 -c "
import sys
seg = 'abcédef’ghi世jkl🌈mno '
open(sys.argv[1] + '/longlines.txt', 'w').write(''.join(seg * 30000 + chr(10) for _ in range(4)))
" "$TMP"
LOLCAT_THREADS=1 LOLCAT_CHUNK=1024 $BIN -F "$TMP/longlines.txt" > "$TMP/ref-long"
for cfg in "LOLCAT_THREADS=8 LOLCAT_CHUNK=1024" "LOLCAT_THREADS=8 LOLCAT_CHUNK=65536"; do
  env $cfg $BIN -F "$TMP/longlines.txt" > "$TMP/out-long"
  if cmp -s "$TMP/ref-long" "$TMP/out-long"; then ok "long lines (file): $cfg"
  else bad "long lines (file): $cfg"; fi
done
LOLCAT_THREADS=4 LOLCAT_CHUNK=1024 $BIN -F < "$TMP/longlines.txt" > "$TMP/out-long"
if cmp -s "$TMP/ref-long" "$TMP/out-long"; then ok "long lines (stdin)"
else bad "long lines (stdin)"; fi
if [ "$(strip_ansi < "$TMP/ref-long" | wc -c | tr -d ' ')" = "$(wc -c < "$TMP/longlines.txt" | tr -d ' ')" ]; then
  ok "long lines round-trip byte count"
else bad "long lines round-trip byte count"; fi

# 256-color mode invariance (exercises the non-fused writer)
f="$TMP/mixed.txt"
FORCE_COLOR=2 LOLCAT_THREADS=1 $BIN "$f" > "$TMP/ref256"
FORCE_COLOR=2 LOLCAT_THREADS=8 LOLCAT_CHUNK=8192 $BIN "$f" > "$TMP/out256"
if cmp -s "$TMP/ref256" "$TMP/out256"; then ok "256-color invariant"
else bad "256-color invariant"; fi
grep -q $'\e\[38;5;' "$TMP/ref256" && ok "256-color emits palette codes" || bad "256-color emits palette codes"

unset LOLCAT_OFFSET

if ./tests/malformed.py "$BIN"; then ok "malformed UTF-8"
else bad "malformed UTF-8"; fi

# NO_COLOR is a pure passthrough
NO_COLOR=1 $BIN "$TMP/mixed.txt" > "$TMP/plain"
if cmp -s "$TMP/mixed.txt" "$TMP/plain"; then ok "NO_COLOR passthrough"
else bad "NO_COLOR passthrough"; fi

# Empty input
: > "$TMP/empty.txt"
$BIN -F "$TMP/empty.txt" > "$TMP/emptyout"
if [ "$(wc -c < "$TMP/emptyout")" -eq 14 ]; then ok "empty input"; else bad "empty input"; fi

# Error handling: commands must fail and preserve their user-facing diagnostics.
check_error() {
  local name=$1 expected=$2
  shift 2
  if "$@" >"$TMP/error.out" 2>"$TMP/error.err"; then
    bad "$name (exits zero)"
  elif [ "$(cat "$TMP/error.err")" = "$expected" ]; then
    ok "$name"
  else
    bad "$name"
    printf '     want: %q\n     got : %q\n' "$expected" "$(cat "$TMP/error.err")"
  fi
}
check_error "missing file rejected" \
  "$BIN: /nonexistent-file-xyz: No such file or directory" \
  "$BIN" /nonexistent-file-xyz
check_error "bad frequency rejected" \
  "$BIN: invalid value 'abc' for '-f': expected a floating point number" \
  "$BIN" -f abc
check_error "zero frequency rejected" "$BIN: invalid frequency: 0" "$BIN" -f 0
check_error "zero spread rejected" "$BIN: invalid spread: 0" "$BIN" -s 0
check_error "unknown option rejected" "$BIN: unknown option: --wat" "$BIN" --wat
check_error "missing option value rejected" "$BIN: missing value for '-f'" "$BIN" -f

# --- 3. conformance with lolcat-ultra --------------------------------------

if [ -x "$ULTRA" ]; then
  head -n 5000 "$TMP/mixed.txt" > "$TMP/conf-mixed.txt"
  head -n 5000 "$TMP/ascii.txt" > "$TMP/conf-ascii.txt"
  printf 'tabs\there\tand\ttabs\n\e[1mbold\e[0m normal\ncrlf\r\n' > "$TMP/conf-odd.txt"
  if python3 tests/conformance.py "$BIN" "$ULTRA" \
       "$TMP/conf-ascii.txt" "$TMP/conf-mixed.txt" "$TMP/conf-odd.txt"; then
    ok "conformance vs lolcat-ultra"
  else
    bad "conformance vs lolcat-ultra"
  fi
else
  printf 'skip conformance vs lolcat-ultra (build it at %s)\n' "$ULTRA"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
