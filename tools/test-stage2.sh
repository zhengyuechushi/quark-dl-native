#!/usr/bin/env bash
# Stage 2 acceptance on a host: the download engine and the TLS path, proved
# against local servers instead of guessed at.
#
# This is the same code the console runs, minus two things that cannot exist
# off-hardware (the kernel thread name and the notifications). So if resume,
# chunked decoding and certificate verification behave here, a failure on the
# console is about the console's network, not about the engine.
set -euo pipefail

BIN=${BIN:-./quark-dl-linux}
WEB_PORT=${WEB_PORT:-8765}
PLAIN_PORT=${PLAIN_PORT:-9101}
TLS_PORT=${TLS_PORT:-9443}
OTHER_PORT=${OTHER_PORT:-9444}
BASE="http://127.0.0.1:${WEB_PORT}"

WORK=$(mktemp -d)
DEST=$(mktemp -d)
DATA=$(mktemp -d)

PIDS=()
MOCK_PID=
APP_PID=
cleanup() {
  [[ -n "$APP_PID" ]] && kill "$APP_PID" 2>/dev/null || true
  for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
  [[ -n "$MOCK_PID" ]] && kill "$MOCK_PID" 2>/dev/null || true
  wait 2>/dev/null || true
  echo "--- server log ---"
  cat "$WORK/server.log" 2>/dev/null || true
  echo "--- mock log (tail) ---"
  tail -50 "$WORK/mock.log" 2>/dev/null || true
}
trap cleanup EXIT

say() { printf '\n=== %s ===\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
payload() { python3 -c "import sys; sys.path.insert(0,'tools'); import mockdl; sys.stdout.buffer.write(mockdl.payload($1))"; }

# ---------------------------------------------------------------- certs ----

say "create two CAs: one the payload trusts, one it must reject"
mkcert() {
  local name=$1
  openssl req -x509 -newkey rsa:2048 -nodes -keyout "$WORK/$name-ca.key" \
    -out "$WORK/$name-ca.pem" -days 3 -subj "/CN=$name test CA" >/dev/null 2>&1
  openssl req -newkey rsa:2048 -nodes -keyout "$WORK/$name.key" \
    -out "$WORK/$name.csr" -subj "/CN=localhost" >/dev/null 2>&1
  cat >"$WORK/$name.ext" <<'EOF'
subjectAltName=DNS:localhost,IP:127.0.0.1
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
EOF
  openssl x509 -req -in "$WORK/$name.csr" -CA "$WORK/$name-ca.pem" \
    -CAkey "$WORK/$name-ca.key" -CAcreateserial -days 2 -out "$WORK/$name.pem" \
    -extfile "$WORK/$name.ext" >/dev/null 2>&1
}
mkcert good
mkcert bad

# -------------------------------------------------------------- servers ----

say "start mock servers"
python3 tools/mockdl.py --port "$PLAIN_PORT" >>"$WORK/mock.log" 2>&1 &
MOCK_PID=$!
python3 tools/mockdl.py --port "$TLS_PORT" --tls --cert "$WORK/good.pem" \
  --key "$WORK/good.key" >>"$WORK/mock.log" 2>&1 &
PIDS+=($!)
python3 tools/mockdl.py --port "$OTHER_PORT" --tls --cert "$WORK/bad.pem" \
  --key "$WORK/bad.key" >>"$WORK/mock.log" 2>&1 &
PIDS+=($!)

for _ in $(seq 1 40); do
  curl -fsS "http://127.0.0.1:${PLAIN_PORT}/echo" >/dev/null 2>&1 && break
  sleep 0.25
done
curl -fsS "http://127.0.0.1:${PLAIN_PORT}/echo" >/dev/null || fail "plain mock did not start"

# ------------------------------------------------------------------ app ----

say "start quark-dl"
# Nothing in this script can log in to quark, so the file the login would have
# written is planted here. It is what /hotlink below checks for: the download
# engine only ever sees a cookie through the boot path or dl_set_cookie(), and
# planting it before start is what proves the boot path publishes it.
printf '__pus=test-session; __puus=test-puus; _uid=1000; __kp=test-kp' \
  >"$DATA/cookie.txt"
QDL_DATA_DIR="$DATA" QDL_DEFAULT_DIR="$DEST" QDL_CA_FILE="$WORK/good-ca.pem" \
  "$BIN" >"$WORK/server.log" 2>&1 &
APP_PID=$!

for _ in $(seq 1 40); do
  curl -fsS "$BASE/api/health" >/dev/null 2>&1 && break
  sleep 0.25
done
curl -fsS "$BASE/api/health" >/dev/null || fail "app did not start"

say "diag reports the TLS layer and the root store"
curl -fsS "$BASE/api/diag?format=json" >"$WORK/diag.json"
python3 - "$WORK/diag.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
tls = d.get('net', {}).get('tls', '')
print('tls =', tls)
assert 'mbedtls' in tls.lower(), 'TLS layer not reported: %r' % tls
assert '根证书' in tls, 'root store not loaded: %r' % tls
assert d['dl']['dir'].startswith('/'), 'download dir missing'
PY

# ------------------------------------------------------------ self-test ----

say "self-test over https, trusting only the generated CA"
curl -fsS "$BASE/api/net/selftest?format=json&url=https%3A%2F%2Flocalhost%3A${TLS_PORT}%2Fecho" >"$WORK/probe.json"
cat "$WORK/probe.json"
echo
python3 - "$WORK/probe.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d['ok'] is True, d
assert d['status'] == 200, d
assert d['tls'].startswith('TLS'), d['tls']
assert 'localhost' in d['cert'], d['cert']
print('https ok, certificate =', d['cert'])
PY

say "self-test rejects a certificate signed by an unknown CA"
curl -fsS "$BASE/api/net/selftest?format=json&url=https%3A%2F%2Flocalhost%3A${OTHER_PORT}%2Fecho" >"$WORK/bad.json"
cat "$WORK/bad.json"
echo
python3 - "$WORK/bad.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d['ok'] is False, d
msg = d.get('error', '')
print('rejected as expected:', msg[:100])
assert '证书' in msg or 'certificate' in msg.lower(), msg
assert '0x' in msg, 'the flag bits are missing from the report: %s' % msg
assert '信任库' in msg, 'the untrusted-CA bit was not named: %s' % msg
# The whole point of reading the verdict after the handshake: a refused
# certificate can still be named, which is what tells a wrong clock apart from
# something answering in the server's place.
assert 'localhost' in d.get('cert', ''), d
print('refused certificate was', d['cert'], 'issued by', d.get('cert_issuer', ''))
PY

# ------------------------------------------------------------ downloads ----

wait_state() {
  local id=$1 want=$2 i state
  for i in $(seq 1 140); do
    state=$(python3 - "$BASE" "$id" <<'PY'
import json, sys, urllib.request
d = json.load(urllib.request.urlopen(sys.argv[1] + '/api/dl/list', timeout=10))
for t in d['tasks']:
    if t['id'] == int(sys.argv[2]):
        print(t['state'], '|', t.get('err', ''))
        break
else:
    print('gone')
PY
)
    case "$state" in
      "$want"*) return 0;;
      failed*|canceled*) fail "task $id ended as $state";;
      gone*) fail "task $id vanished from the queue";;
    esac
    sleep 0.5
  done
  fail "task $id never reached $want (last: $state)"
}

say "download 300000 bytes over plain http"
expected="$WORK/expected.bin"
payload 300000 >"$expected"
curl -fsS -X POST --data-urlencode "url=http://127.0.0.1:${PLAIN_PORT}/file?size=300000" \
  --data-urlencode "dir=$DEST" "$BASE/api/dl/add" >"$WORK/add.json"
cat "$WORK/add.json"
echo
ID=$(python3 -c "import json;print(json.load(open('$WORK/add.json'))['ids'][0])")
wait_state "$ID" done
cmp "$expected" "$DEST/file" || fail "downloaded file differs"
echo "file ok: $(wc -c <"$DEST/file") bytes"

say "a quark CDN link is refused without UA, Referer and cookie"
# The mock's negative control first: without the three headers it must answer
# 412, or the positive case below would pass for the wrong reason.
code=$(curl -s -o /dev/null -w '%{http_code}' \
  "http://127.0.0.1:${PLAIN_PORT}/hotlink?size=1000")
[[ "$code" == 412 ]] || fail "hotlink mock not enforcing (got $code, want 412)"
curl -fsS -X POST --data-urlencode "url=http://127.0.0.1:${PLAIN_PORT}/hotlink?size=40000" \
  --data-urlencode "dir=$DEST" "$BASE/api/dl/add" >"$WORK/add-hot.json"
IDH=$(python3 -c "import json;print(json.load(open('$WORK/add-hot.json'))['ids'][0])")
wait_state "$IDH" done
payload 40000 >"$WORK/expected-hot.bin"
cmp "$WORK/expected-hot.bin" "$DEST/hotlink" || fail "hotlink file differs"
grep -q "HOTLINK referer='https://pan.quark.cn/'" "$WORK/mock.log" \
  || fail "download did not send the quark Referer"
grep -q "HOTLINK.*__puus=test-puus" "$WORK/mock.log" \
  || fail "download did not send the session cookie"
grep -q "HOTLINK.*quark-cloud-drive" "$WORK/mock.log" \
  || fail "download did not send the client UA"
echo "412 without the headers, 200 with them, 40000 bytes matched"

say "resume: a pre-seeded .part must produce a Range request"head -c 120000 "$expected" >"$DEST/slow.part"
curl -fsS -X POST --data-urlencode "url=http://127.0.0.1:${PLAIN_PORT}/slow?size=300000" \
  --data-urlencode "dir=$DEST" "$BASE/api/dl/add" >"$WORK/add2.json"
ID2=$(python3 -c "import json;print(json.load(open('$WORK/add2.json'))['ids'][0])")
wait_state "$ID2" done
cmp "$expected" "$DEST/slow" || fail "resumed file differs"
grep -q 'RANGE /slow bytes=120000-' "$WORK/mock.log" || fail "no ranged request was made"
echo "resumed from byte 120000 and matched byte for byte"

say "chunked body with no Content-Length"
curl -fsS -X POST --data-urlencode "url=http://127.0.0.1:${PLAIN_PORT}/chunked?size=90000" \
  --data-urlencode "dir=$DEST" "$BASE/api/dl/add" >"$WORK/add3.json"
ID3=$(python3 -c "import json;print(json.load(open('$WORK/add3.json'))['ids'][0])")
wait_state "$ID3" done
payload 90000 >"$WORK/expected3.bin"
cmp "$WORK/expected3.bin" "$DEST/chunked" || fail "chunked file differs"

say "an expired link is reported, not retried forever"
curl -fsS -X POST --data-urlencode "url=http://127.0.0.1:${PLAIN_PORT}/missing" \
  --data-urlencode "dir=$DEST" "$BASE/api/dl/add" >"$WORK/add4.json"
ID4=$(python3 -c "import json;print(json.load(open('$WORK/add4.json'))['ids'][0])")
wait_state "$ID4" failed
python3 - "$BASE" "$ID4" <<'PY'
import json, sys, urllib.request
d = json.load(urllib.request.urlopen(sys.argv[1] + '/api/dl/list', timeout=10))
t = [x for x in d['tasks'] if x['id'] == int(sys.argv[2])][0]
assert '404' in t['err'], t
print('reported:', t['err'])
PY

say "download over TLS"
curl -fsS -X POST --data-urlencode "url=https://localhost:${TLS_PORT}/file?size=70000" \
  --data-urlencode "name=tls.bin" --data-urlencode "dir=$DEST" \
  "$BASE/api/dl/add" >"$WORK/add5.json"
ID5=$(python3 -c "import json;print(json.load(open('$WORK/add5.json'))['ids'][0])")
wait_state "$ID5" done
payload 70000 >"$WORK/expected5.bin"
cmp "$WORK/expected5.bin" "$DEST/tls.bin" || fail "TLS download differs"
echo "tls download ok: $(wc -c <"$DEST/tls.bin") bytes"

say "re-adding a finished URL does not download it twice"
curl -fsS -X POST --data-urlencode "url=http://127.0.0.1:${PLAIN_PORT}/file?size=300000" \
  --data-urlencode "dir=$DEST" "$BASE/api/dl/add" >"$WORK/add6.json"
ID6=$(python3 -c "import json;print(json.load(open('$WORK/add6.json'))['ids'][0])")
test "$ID6" = "$ID" || fail "duplicate URL created a second task ($ID vs $ID6)"
echo "same task id returned"

say "re-adding a link whose file was deleted downloads it again"
rm -f "$DEST/file"
python3 - "$BASE" <<'PY'
import json, sys, urllib.request
d = json.load(urllib.request.urlopen(sys.argv[1] + '/api/dl/list', timeout=10))
t = [x for x in d['tasks'] if x['name'] == 'file' and x['state'] == 'done'][0]
assert t['exists'] is False, t
print('exists=false for the deleted file, so the page can offer a re-download')
PY
curl -fsS -X POST --data-urlencode "url=http://127.0.0.1:${PLAIN_PORT}/file?size=300000" \
  --data-urlencode "dir=$DEST" "$BASE/api/dl/add" >"$WORK/add7.json"
cat "$WORK/add7.json"
echo
ID7=$(python3 -c "import json;print(json.load(open('$WORK/add7.json'))['ids'][0])")
test "$ID7" = "$ID" || fail "re-adding the same url made a new task ($ID vs $ID7)"
wait_state "$ID" done
cmp "$expected" "$DEST/file" || fail "re-downloaded file differs"
echo "downloaded again: $(wc -c <"$DEST/file") bytes"

say "the retry endpoint requeues a finished task"
curl -fsS -X POST "$BASE/api/dl/retry?id=$ID4" >"$WORK/retry.json"
cat "$WORK/retry.json"
echo
python3 - "$WORK/retry.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d['queued'] == 1, d
PY
wait_state "$ID4" failed
echo "retried and failed again, as it must for a 404"

say "the queue is written to disk"
test -s "$DATA/downloads.txt" || fail "queue was not persisted"
grep -q 'http' "$DATA/downloads.txt" || fail "queue file has no url"
echo "persisted $(wc -l <"$DATA/downloads.txt") task(s)"

say "after a restart, a finished task whose file is gone reads as missing"
rm -f "$DEST/file"
kill "$APP_PID" 2>/dev/null || true
wait "$APP_PID" 2>/dev/null || true
APP_PID=
QDL_DATA_DIR="$DATA" QDL_DEFAULT_DIR="$DEST" QDL_CA_FILE="$WORK/good-ca.pem" \
  "$BIN" >>"$WORK/server.log" 2>&1 &
APP_PID=$!
for _ in $(seq 1 40); do
  curl -fsS "$BASE/api/health" >/dev/null 2>&1 && break
  sleep 0.25
done
python3 - "$BASE" "$ID" <<'PY'
import json, sys, urllib.request
d = json.load(urllib.request.urlopen(sys.argv[1] + '/api/dl/list', timeout=10))
t = [x for x in d['tasks'] if x['id'] == int(sys.argv[2])][0]
assert t['state'] == 'missing', t
assert t['exists'] is False, t
print('state =', t['state'], '| exists =', t['exists'], '| total =', t['total'])
PY
echo "a deleted file is 'missing', not 'failed': nothing failed, the bytes left"
echo
echo "all stage-2 checks passed"
