#!/usr/bin/env bash
set -euo pipefail
# -e            Exit immediately if a command fails (non-zero exit)
# -u            Error if using an undefined variable
# -o pipefail   In pipelines, fail if ANY command fails (not just the last)

BASE="${BASE:-http://127.0.0.1:8080}"   # If BASE is unset/empty, use default
COMMON=(-sS --http1.1 --max-time 2 --connect-timeout 1)
# -s                    silent (no progress meter)
# -S                    show errors even when -s is used
# --http1.1             force HTTP/1.1
# --max-time 2          max total time per request (seconds)
# --connect-timeout 1   max time to connect (seconds)

# Optional: a second set of flags to test HTTP/1.0 explicitly
COMMON10=(-sS --http1.0 --max-time 2 --connect-timeout 1)

pass() { printf "✅ %s\n" "$1"; }
fail() { printf "❌ %s\n" "$1"; exit 1; }
cleanup() {
  rm -f ./www/trash/file.txt

  # remove files created by upload tests
  rm -f ./www/upload/small.txt
  rm -f ./www/upload/copy_large.bin
  rm -f ./www/upload/conflict.txt
  rm -f ./www/upload/chunked.txt
}
trap cleanup EXIT INT TERM
    #   EXIT    when scripts ends for any reason
    #   INT     when Ctrl-C
    #   TERM    when process is terminated
cleanup

# Return ONLY the HTTP status code.
# Usage: code <curl args...>
code() {
  curl "${COMMON[@]}" -o /dev/null -w "%{http_code}" "$@"
}
#   -o /dev/null        throw away the response body
#   -w "%{http_code}"   print only the HTTP status code
#   "$@"                all arguments passed to the function

# Return headers + status line only (no body).
# Usage: headers <curl args...>
headers() {
  curl "${COMMON[@]}" -D - -o /dev/null "$@"
}
#   -D -                write response headers to stdout (- means stdout)
#   -o /dev/null        ignores body

# Expect exactly one status code.
# Usage: expect_code "test name" 200 <curl args...>
expect_code() {
  local name="$1" expected="$2"     # name gets first arguement, expected gets second argument
  shift 2                           # removes the first two arguments from $@
  local got
  got="$(code "$@")"                # runs code function [$(...) is command substitution]
  [[ "$got" == "$expected" ]] && pass "$name ($got)" || fail "$name expected $expected got $got"
    # compact if/else:
    # if ($got == $expected)
    #   run pass
    # else
    #   run fail
}

# Expect response headers contain a pattern (regex).
# Usage: expect_header_contains "test name" '^Allow:' <curl args...>
expect_header_contains() {
  local name="$1" pattern="$2"
  shift 2
  local out
  out="$(headers "$@")"
  echo "$out" | grep -qiE "$pattern" && pass "$name" || { echo "$out"; fail "$name (missing: $pattern)"; }
    # -q    quiet (no output)
    # -i    case-insensitive
    # -E    extended regex
}

echo "BASE=$BASE"
echo

# 1) Basic GET
expect_code "1) GET /" 200 "$BASE/"

# (Optional) HTTP/1.0 sanity test (only if you want to verify 1.0 support)
code10="$(curl "${COMMON10[@]}" -o /dev/null -w "%{http_code}" "$BASE/" || true)"
if [[ "$code10" == "200" ]]; then
  pass "1b) GET / over HTTP/1.0 ($code10)"
else
  # Not failing the whole suite by default (because many webservs are 1.1-focused)
  printf "⚠️  1b) HTTP/1.0 returned %s (not treated as failure)\n" "$code10"
fi

# 2) GET non-existing -> 404
expect_code "2) GET /does-not-exist" 404 "$BASE/does-not-exist"

# 3) Method not allowed -> 405 + Allow
expect_code "3) POST / -> 405" 405 -X POST "$BASE/" #   -X METHOD forces a method
expect_header_contains "3b) Allow header present" '^Allow:' -X POST "$BASE/"

# 4) Small POST upload
expect_code "4) POST /upload/small.txt small body" 201 -X POST "$BASE/upload/small.txt" \
  -H "Content-Type: text/plain" --data "hello world"        #   -H              Adds an HTTP header to the request
                                                            #   --data          same as -d, send data as the request body
                                                            #   --data-binary   send raw bytes exactly as-is

# 5) Large POST upload (accept 201 or 413)
large_code="$(code -X POST "$BASE/upload/copy_large.bin" --data-binary @./www/stress/large.bin || true)"
if [[ "$large_code" == "201" || "$large_code" == "413" ]]; then
  pass "5) POST /upload/copy_large.bin large body ($large_code)"
else
  fail "5) POST /upload/copy_large.bin large body expected 201(upload success) or 413(too big to upload) but got $large_code"
fi

# 6) Conflict (deterministic): ensure target already exists -> expect 409
mkdir -p ./www/upload
echo "existing" > ./www/upload/conflict.txt

expect_code "6) POST /upload/conflict.txt when exists -> 409" 409 \
  -X POST "$BASE/upload/conflict.txt" -H "Content-Type: text/plain" --data "hello again"


# 7) DELETE existing file -> 204
mkdir -p ./www/trash
echo "to delete" > ./www/trash/file.txt
expect_code "7) DELETE /trash/file.txt" 204 -X DELETE "$BASE/trash/file.txt"

# 8) DELETE non-existing -> 404
expect_code "8) DELETE /trash/nope.txt" 404 -X DELETE "$BASE/trash/nope.txt"

# 9) Chunked POST (HTTP/1.1 only)
chunk_code="$(
  printf 'b\r\nhello world\r\n0\r\n\r\n' | \
  curl "${COMMON[@]}" -o /dev/null -w "%{http_code}" \
    -X POST "$BASE/upload/chunked.txt" -H "Transfer-Encoding: chunked" --data-binary @- || true
)"
[[ "$chunk_code" == "201" ]] && pass "9) Chunked POST ($chunk_code)" || fail "9) Chunked POST expected 201(upload success) got $chunk_code"

# 10) Connection close header
expect_header_contains "10) Connection: close present" '^Connection:\s*close' \
  -H "Connection: close" "$BASE/"

# 11) Multiple requests in one curl invocation
multi_out="$(curl "${COMMON[@]}" -i --next "$BASE/" --next "$BASE/does-not-exist" || true)"
if grep -q "200 OK" <<<"$multi_out" && grep -q "404 Not Found" <<<"$multi_out"; then
  pass "11) Multiple requests in one invocation"
else
  echo "$multi_out"
  fail "11) Expected both 200 and 404 in output"
fi
    # <<<"$multi_out"   a here-string: send the contents of multi_out to stdin.

    #   -i      inculde the response headers in the output
    #   --next  start a new request with new URL/options -> matters when multiple URLs in one command


echo
cleanup
pass "All done."