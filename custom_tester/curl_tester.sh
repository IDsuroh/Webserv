#!/usr/bin/env bash
set -euo pipefail
# -e            Exit immediately if a command fails (non-zero exit)
# -u            Error if using an undefined variable
# -o pipefail   In pipelines, fail if ANY command fails (not just the last)

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# ${BASH_SOURCE[0]}
#   path to the current script file.
# dirname -- "${BASH_SOURCE[0]}"
#   dirname returns the directory part of a path.
# -- means “stop parsing options”
# cd -- "<that directory>"
#   Change directory into the script’s directory.
LARGE_FILE="$SCRIPT_DIR/resource/large.bin"
TEST_ROOT="${SCRIPT_DIR}/curl_www"
UPLOAD_DIR="${TEST_ROOT}/upload"
TRASH_DIR="${TEST_ROOT}/trash"


BASE="${BASE:-http://127.0.0.1:8080}"   # If BASE is unset/empty, use default
COMMON=(-sS --http1.1 --max-time 2 --connect-timeout 1)
# -s                    silent (no progress meter)
# -S                    show errors even when -s is used
# --http1.1             force HTTP/1.1
# --max-time 2          max total time per request (seconds)
# --connect-timeout 1   max time to connect (seconds)

# Optional: a second set of flags to test HTTP/1.0 explicitly
COMMON10=(-sS --http1.0 --max-time 2 --connect-timeout 1)

# Optional knobs:
#   VERBOSE=1      show response headers for each test
#   DEBUG_BODY=1   show a short body preview for each test (if any)
VERBOSE="${VERBOSE:-0}"


pass()  {
  echo
  printf "RESULT: ✅ %s\n" "$1";
}

fail()  {
  echo
  printf "RESULT: ❌ %s\n" "$1"; exit 1;
}

banner() {
  # banner "Test title" "What it checks" "The request we send"
  echo
  echo
  echo "🌟 Testing => $1"
  echo
  echo "  Checks:     $2"
  echo "  Request:    $3"
}

cleanup() {
  rm -f "${TRASH_DIR}/file.txt"
  rm -f "${UPLOAD_DIR}/small.txt"
  rm -f "${UPLOAD_DIR}/copy_large.bin"
  rm -f "${UPLOAD_DIR}/conflict.txt"
  rm -f "${UPLOAD_DIR}/chunked.txt"
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

# Full response (headers + body) for debugging
full() {
  curl "${COMMON[@]}" -i "$@"
}

# Extract last HTTP status code from a curl -i output (handles 100 Continue + final)
status_from_full() {
  grep -Eo '^HTTP/1\.[01] [0-9]{3}' | tail -n1 | awk '{print $2}'
}

# Print ALL header blocks from curl -i output, with NO trailing newline.
all_header_blocks() {
  awk '
    BEGIN { RS="\r\n\r\n|\n\n"; ORS=""; first=1 }
    $0 ~ /^HTTP\// {
      # separate header blocks with ONE blank line, but not after the last one
      if (!first) printf "\n\n"
      first=0
      sub(/\r$/, "", $0)       # defensive: strip stray CR
      printf "%s", $0
    }
  '
}

# Print headers ONLY if VERBOSE=1.
# IMPORTANT: this function also must NOT end by printing extra newlines.
print_headers_if_verbose() {
  local title="$1"
  local full_out="$2"
  local mode="${3:-all}"   # all | first

  [[ "$VERBOSE" != "1" ]] && return 0

  printf "\n[%s]\n" "$title"

  if [[ "$mode" == "first" ]]; then
    awk 'BEGIN{RS="\r\n\r\n|\n\n"; ORS=""} { sub(/\r$/, "", $0); printf "%s", $0; exit }' <<<"$full_out"
  else
    printf "%s" "$full_out" | all_header_blocks
  fi
}


# Expect exactly one status code.
# Usage: expect_code "test name" 200 <curl args...>
expect_code() {
  local name="$1" expected="$2"     # name gets first arguement, expected gets second argument
  shift 2                           # removes the first two arguments from $@
  local got out               
  if [[ "$VERBOSE" == "1" ]]; then
    # Get headers (and status line) so we can display them
    out="$(curl "${COMMON[@]}" -i "$@" || true)"
    got="$(printf "%s" "$out" | status_from_full)"

    if [[ "$got" == "$expected" ]]; then
      pass "$name ($got)"
      echo
      echo "[Response headers]"
      awk 'BEGIN{RS="\r\n\r\n|\n\n"} {print $0; exit}' <<<"$out"
    else
      echo
      echo "[FAIL DETAILS] expected $expected got ${got:-"(could not parse)"}"
      echo "[Full response]"
      echo "$out"
      fail "$name"
    fi
  else
    # Fast path: only status code
    got="$(code "$@")"              # runs code function [$(...) is command substitution]
    [[ "$got" == "$expected" ]] && pass "$name ($got)" || fail "$name expected $expected got $got"
  fi
}

# Expect response headers contain a pattern (regex).
# Usage: expect_header_contains "test name" '^Allow:' <curl args...>
expect_header_contains() {
  local name="$1" pattern="$2"
  shift 2

  # Always fetch headers only
  local out
  out="$(headers "$@")"

  if echo "$out" | grep -qiE "$pattern"; then
    pass "$name"
    # Print the headers we actually checked (only if VERBOSE=1)
    print_headers_if_verbose "Response headers" "$out" first
  else
    echo
    echo "[FAIL DETAILS] missing header pattern: $pattern"
    echo "[Got headers]"
    echo "$out"
    fail "$name (missing: $pattern)"
  fi
}


echo "BASE=$BASE"
echo "Tip: run VERBOSE=1 ./curl_tester.sh to print response headers."

# 1) Basic GET
banner "1) GET /" \
  "Server is reachable and serves the index/root resource." \
  "GET $BASE/"
expect_code "1) GET /" 200 "$BASE/"

# (Optional) HTTP/1.0 sanity test (only if you want to verify 1.0 support)
banner "1b) GET / over HTTP/1.0" \
  "Your server accepts HTTP/1.0 requests (optional feature)." \
  "GET(HTTP/1.0) $BASE/"
code10="$(curl "${COMMON10[@]}" -o /dev/null -w "%{http_code}" "$BASE/" || true)"
if [[ "$code10" == "200" ]]; then
  pass "1b) GET / over HTTP/1.0 ($code10)"
else
  printf "⚠️  1b) HTTP/1.0 returned %s (not treated as failure)\n" "$code10"
fi

# 2) GET non-existing -> 404
banner "2) GET /does-not-exist" \
  "Unknown paths return 404 (Not Found)." \
  "GET $BASE/does-not-exist"
expect_code "2) GET /does-not-exist" 404 "$BASE/does-not-exist"

# 3) Method not allowed -> 405 + Allow
banner "3) POST / -> 405" \
  "Method restriction: POST is not allowed on /, and server returns 405 + Allow." \
  "POST $BASE/"
expect_code "3) POST / -> 405" 405 -X POST "$BASE/" #   -X METHOD forces a method
expect_header_contains "3b) Allow header present" '^Allow:' -X POST "$BASE/"

# 4) Small POST upload
banner "4) POST /upload/small.txt" \
  "Upload endpoint stores request body as a file and returns 201 Created." \
  "POST $BASE/upload/small.txt  body='hello world'"
expect_code "4) POST /upload/small.txt small body" 201 -X POST "$BASE/upload/small.txt" \
  -H "Content-Type: text/plain" --data "hello world"        #   -H              Adds an HTTP header to the request
                                                            #   --data          same as -d, send data as the request body
                                                            #   --data-binary   send raw bytes exactly as-is

# 5) Large POST upload (accept 201 or 413)
banner "5) POST /upload/copy_large.bin (large body)" \
  "Large upload either succeeds (201) or is rejected by max-body-size (413)." \
  "POST $BASE/upload/copy_large.bin  body=@$LARGE_FILE"
large_out=""
if [[ "$VERBOSE" == "1" ]]; then
  large_out="$(full -X POST "$BASE/upload/copy_large.bin" --data-binary @"$LARGE_FILE" || true)"
  large_code="$(printf "%s" "$large_out" | status_from_full)"
else
  large_code="$(code -X POST "$BASE/upload/copy_large.bin" --data-binary @"$LARGE_FILE" || true)"
fi
if [[ "$large_code" == "201" || "$large_code" == "413" ]]; then
  pass "5) POST /upload/copy_large.bin large body ($large_code)"
  print_headers_if_verbose "Response headers (all responses)" "$large_out" all
else
  echo
  echo "[FAIL DETAILS] expected 201 or 413 got ${large_code:-"(empty)"}"
  echo "[Full response]"
  full -X POST "$BASE/upload/copy_large.bin" --data-binary @"$LARGE_FILE" || true
  fail "5) Large upload"
fi

# 6) Conflict (deterministic): ensure target already exists -> expect 409
banner "6) POST /upload/conflict.txt when exists -> 409" \
  "Conflict handling: server must not overwrite an existing upload target." \
  "POST $BASE/upload/conflict.txt (but file already exists)"

echo "existing" > "$UPLOAD_DIR/conflict.txt"
expect_code "6) POST /upload/conflict.txt when exists -> 409" 409 \
  -X POST "$BASE/upload/conflict.txt" -H "Content-Type: text/plain" --data "hello again"


# 7) DELETE existing file -> 204
banner "7) DELETE /trash/file.txt" \
  "DELETE removes an existing file and returns 204 No Content." \
  "DELETE $BASE/trash/file.txt"
echo "to delete" > "$TRASH_DIR/file.txt"
expect_code "7) DELETE /trash/file.txt" 204 -X DELETE "$BASE/trash/file.txt"

# 8) DELETE non-existing -> 404
banner "8) DELETE /trash/nope.txt" \
  "DELETE on missing file returns 404." \
  "DELETE $BASE/trash/nope.txt"
expect_code "8) DELETE /trash/nope.txt" 404 -X DELETE "$BASE/trash/nope.txt"

# 9) Chunked POST (HTTP/1.1 only)
banner "9) Chunked POST /upload/chunked.txt" \
  "HTTP/1.1 chunked transfer decoding works (server reads body correctly)." \
  "POST(chunks) $BASE/upload/chunked.txt  body='hello world'"
chunk_out=""
if [[ "$VERBOSE" == "1" ]]; then
  chunk_out="$(
    printf 'b\r\nhello world\r\n0\r\n\r\n' | \
    full -X POST "$BASE/upload/chunked.txt" -H "Transfer-Encoding: chunked" --data-binary @- || true
  )"
  chunk_code="$(printf "%s" "$chunk_out" | status_from_full)"
else
  chunk_code="$(
    printf 'b\r\nhello world\r\n0\r\n\r\n' | \
    curl "${COMMON[@]}" -o /dev/null -w "%{http_code}" \
      -X POST "$BASE/upload/chunked.txt" -H "Transfer-Encoding: chunked" --data-binary @- || true
  )"
fi
if [[ "$chunk_code" == "201" ]]; then
  pass "9) Chunked POST ($chunk_code)"
  print_headers_if_verbose "Response headers (all responses)" "$chunk_out" all
else
  echo
  echo "[Full response]"
  printf 'b\r\nhello world\r\n0\r\n\r\n' | full -X POST "$BASE/upload/chunked.txt" -H "Transfer-Encoding: chunked" --data-binary @- || true
  fail "9) Chunked POST expected 201 got $chunk_code"
fi

# 10) Connection close header
banner "10) Connection: close" \
  "When client requests Connection: close, server responds accordingly." \
  "GET $BASE/  with header 'Connection: close'"
expect_header_contains "10) Connection: close present" '^Connection:\s*close' \
  -H "Connection: close" "$BASE/"

# 11) Multiple requests in one curl invocation
banner "11) Multiple URLs in one curl invocation" \
  "curl performs two requests sequentially; output should include both 200 and 404 responses." \
  "GET $BASE/  then GET $BASE/does-not-exist"
multi_out="$(curl "${COMMON[@]}" -i "$BASE/" "$BASE/does-not-exist" || true)"
if grep -q "200 OK" <<<"$multi_out" && grep -q "404 Not Found" <<<"$multi_out"; then
  pass "11) Multiple requests in one invocation"
  if [[ "$VERBOSE" == "1" ]]; then
    echo
    echo "[Response headers (all responses)]"
    awk '
      BEGIN {RS="\r\n\r\n|\n\n"}
      $0 ~ /^HTTP\// { print $0 "\n" }
    ' <<<"$multi_out"
  fi
else
  echo
  echo "[FAIL DETAILS] Expected both a 200 and a 404 response in output."
  echo "$multi_out"
  fail "11) Multiple requests"
fi

echo
#cleanup
echo
echo "                              🌟🌟🌟All done🌟🌟🌟"
echo