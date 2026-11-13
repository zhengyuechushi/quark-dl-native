#!/usr/bin/env python3
"""A deliberately boring file server for the download tests.

It exists to produce the four situations the download engine has to survive and
that a real CDN only produces at random: a ranged resume (206), a server that
ignores Range (200), a chunked response with no Content-Length, and an expired
link (404). `--tls` wraps it so the same tests can prove the certificate path.

Usage:
    python3 tools/mockdl.py --port 9001
    python3 tools/mockdl.py --port 9443 --tls --cert c.pem --key k.pem
"""

import argparse
import hashlib
import ssl
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs


def payload(size):
    """Deterministic bytes, so the test can rebuild them independently."""
    block = bytes((i * 31 + 7) & 0xFF for i in range(4096))
    out = bytearray()
    while len(out) < size:
        out.extend(block)
    return bytes(out[:size])


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        sys.stderr.write('%s %s\n' % (self.address_string(), fmt % args))

    def _params(self):
        parsed = urlparse(self.path)
        return parsed.path, parse_qs(parsed.query)

    def do_HEAD(self):
        self.do_GET(head_only=True)

    def do_GET(self, head_only=False):
        path, q = self._params()

        if path == '/missing':
            body = b'gone'
            self.send_response(404)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close')
            self.end_headers()
            if not head_only:
                self.wfile.write(body)
            return

        if path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/file?size=%s' % q.get('size', ['1000'])[0])
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
            return

        if path == '/echo':
            body = ('%s\n' % '\n'.join('%s: %s' % (k, v) for k, v in self.headers.items())).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close')
            self.end_headers()
            if not head_only:
                self.wfile.write(body)
            return

        if path not in ('/file', '/slow', '/chunked'):
            self.send_response(404)
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
            return

        size = int(q.get('size', ['1048576'])[0])
        data = payload(size)
        start = 0
        rng = self.headers.get('Range')
        if rng and rng.startswith('bytes='):
            try:
                start = int(rng.split('=', 1)[1].split('-', 1)[0])
            except ValueError:
                start = 0
        start = max(0, min(start, size))
        if rng:
            # The resume test asserts on this line: it is the only proof that a
            # pre-seeded .part produced a ranged request rather than a restart.
            sys.stderr.write('RANGE %s %s\n' % (path, rng))
            sys.stderr.flush()

        if path == '/chunked':
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Transfer-Encoding', 'chunked')
            self.send_header('Connection', 'close')
            self.end_headers()
            if not head_only:
                step = max(1, size // 7)
                for i in range(0, size, step):
                    chunk = data[i:i + step]
                    self.wfile.write(b'%x\r\n' % len(chunk))
                    self.wfile.write(chunk)
                    self.wfile.write(b'\r\n')
                self.wfile.write(b'0\r\n\r\n')
            return

        partial = rng is not None and start > 0
        if partial:
            self.send_response(206)
            self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, size - 1, size))
        else:
            self.send_response(200)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(size - start))
        self.send_header('Connection', 'close')
        self.end_headers()
        if head_only:
            return

        body = data[start:]
        if path == '/slow':
            step = max(1, len(body) // 20)
            for i in range(0, len(body), step):
                self.wfile.write(body[i:i + step])
                self.wfile.flush()
                time.sleep(0.05)
            return
        self.wfile.write(body)

    def checksum(self, size):
        return hashlib.sha256(payload(size)).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=9001)
    ap.add_argument('--tls', action='store_true')
    ap.add_argument('--cert')
    ap.add_argument('--key')
    args = ap.parse_args()

    httpd = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    if args.tls:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(args.cert, args.key)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    sys.stderr.write('mockdl listening on %d (tls=%s)\n' % (args.port, args.tls))
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
