#!/usr/bin/env python3
"""A stand-in for the tile CDN (and Overpass) for the app sim tests.

    sim/cdn/tile_cdn.py --dir sim/fixtures/appmaps [--port 0]

Serves the paths the apps ask https://tiles.opentrailpaper.com/v1/ for
(docs/prebuilt-tiles.md) from a directory laid out like the CDN:

    /v1/meta.json                  <dir>/v1/meta.json (or <dir>/meta.json)
    /v1/<g>/index.json             <dir>/v1/<g>/index.json
    /v1/<g>/<id>.ebm?v=<hash>      <dir>/v1/<g>/<id>.ebm   (?v= is ignored, as the CDN does)

Without a v1/ directory, <dir> itself is the v1/ root. The files are read at
start and never written. Overpass (POST/GET /api/interpreter) answers 503 by
default, or a canned body (--overpass-file), so a test whose selection falls
back to Overpass fails visibly instead of reaching the internet.

Prints "tile_cdn listening on 127.0.0.1:<port>" once it accepts requests.

Test controls, under /_ctl/ (POST, query parameters):

    /_ctl/log                 every request so far, as JSON (GET)
    /_ctl/file?path=/v1/...   what is served at that path now, unlogged (GET)
    /_ctl/reset               forget bumps, faults and the log
    /_ctl/bump?id=<h3>[&ext=ebm|poi]
                              publish a new version of that file: the same bytes
                              plus an unknown extension section (the firmware
                              skips it by length, map_tiles.cpp), with its new
                              size and hash (sha256[:16]) in the group index
    /_ctl/fault?match=<glob>&kind=truncate|status:<code>|close
                              misbehave on matching paths (fnmatch on the path
                              without the query, e.g. "*.ebm"): a 200 with half
                              the body, an HTTP error, or a dropped connection
"""
import argparse
import fnmatch
import hashlib
import json
import os
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


class Cdn:
    def __init__(self, root, overpass_status=503, overpass_body=None):
        v1 = os.path.join(root, "v1")
        self.base = v1 if os.path.isdir(v1) else root
        self.files = {}   # "/v1/<rel>" -> bytes
        for d, _, names in os.walk(self.base):
            for n in names:
                p = os.path.join(d, n)
                rel = os.path.relpath(p, self.base).replace(os.sep, "/")
                with open(p, "rb") as f:
                    self.files["/v1/" + rel] = f.read()
        self.overpass_status = overpass_status
        self.overpass_body = overpass_body
        self.lock = threading.Lock()
        self.reset()

    def reset(self):
        with self.lock:
            self.over = {}     # path -> bytes (bumped files and their indexes)
            self.faults = []   # (glob, kind)
            self.log = []

    def body(self, path):
        with self.lock:
            if path in self.over:
                return self.over[path]
        return self.files.get(path)

    def bump(self, cell, ext):
        g = cell[:6]
        path = f"/v1/{g}/{cell}.{ext}"
        old = self.body(path)
        ipath = f"/v1/{g}/index.json"
        index = self.body(ipath)
        if old is None or index is None:
            raise KeyError(path)
        # An extension section: <magic><u32 length><payload>.
        n = sum(1 for p in self.over if p.startswith(path))
        payload = f"sim-bump-{n + 1}".encode()
        new = old + b"XSIM" + struct.pack("<I", len(payload)) + payload
        h = hashlib.sha256(new).hexdigest()[:16]
        idx = json.loads(index)
        e = idx["cells"][cell]
        if ext == "ebm":
            e[0], e[1] = len(new), h
        else:
            e[2], e[3] = len(new), h
        with self.lock:
            self.over[path] = new
            self.over[ipath] = json.dumps(idx, separators=(",", ":")).encode()
        return {"path": path, "size": len(new), "hash": h}

    def fault_for(self, path):
        with self.lock:
            for glob, kind in self.faults:
                if fnmatch.fnmatch(path, glob):
                    return kind
        return None


def handler(cdn):
    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        def send(self, status, body=b"", ctype="application/octet-stream"):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def record(self, status, path, fault=None):
            with cdn.lock:
                cdn.log.append({"method": self.command, "path": path, "status": status, "fault": fault,
                                "ua": self.headers.get("User-Agent", "")})

        def ctl(self, path, q):
            arg = lambda k, d=None: q.get(k, [d])[0]
            if path == "/_ctl/log":
                with cdn.lock:
                    return self.send(200, json.dumps(cdn.log).encode(), "application/json")
            if path == "/_ctl/file":
                body = cdn.body(arg("path", ""))
                return self.send(200, body) if body is not None else self.send(404)
            if path == "/_ctl/reset":
                cdn.reset()
                return self.send(200, b"{}", "application/json")
            if path == "/_ctl/bump":
                try:
                    r = cdn.bump(arg("id", ""), arg("ext", "ebm"))
                except KeyError as e:
                    return self.send(404, json.dumps({"error": str(e)}).encode(), "application/json")
                return self.send(200, json.dumps(r).encode(), "application/json")
            if path == "/_ctl/fault":
                with cdn.lock:
                    cdn.faults.append((arg("match", "*"), arg("kind", "status:500")))
                return self.send(200, b"{}", "application/json")
            return self.send(404)

        def serve(self):
            u = urlsplit(self.path)
            path = u.path
            if path.startswith("/_ctl/"):
                return self.ctl(path, parse_qs(u.query))
            if self.command == "POST":
                n = int(self.headers.get("Content-Length") or 0)
                if n:
                    self.rfile.read(n)
            if path.endswith("/api/interpreter"):
                status = cdn.overpass_status if cdn.overpass_body is None else 200
                self.record(status, path)
                return self.send(status, cdn.overpass_body or b"stand-in CDN: no Overpass here",
                                 "application/json" if cdn.overpass_body else "text/plain")
            body = cdn.body(path)
            kind = cdn.fault_for(path) if body is not None else None
            if kind == "close":
                self.record(-1, path, kind)
                self.close_connection = True
                return
            if kind and kind.startswith("status:"):
                status = int(kind.split(":", 1)[1])
                self.record(status, path, kind)
                return self.send(status, b"stand-in CDN fault", "text/plain")
            if body is None:
                self.record(404, path)
                return self.send(404, b"not found", "text/plain")
            if kind == "truncate":
                body = body[: len(body) // 2]
            self.record(200, path, kind)
            ctype = "application/json" if path.endswith(".json") else "application/octet-stream"
            self.send(200, body, ctype)

        do_GET = do_POST = do_HEAD = serve

    return H


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", required=True, help="fixture directory (CDN layout)")
    ap.add_argument("--port", type=int, default=0, help="0 = any free port")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--overpass-status", type=int, default=503)
    ap.add_argument("--overpass-file", help="answer Overpass with this body (200) instead")
    a = ap.parse_args()
    body = open(a.overpass_file, "rb").read() if a.overpass_file else None
    cdn = Cdn(a.dir, a.overpass_status, body)
    if not cdn.files:
        sys.exit(f"no files under {a.dir}")
    srv = ThreadingHTTPServer((a.host, a.port), handler(cdn))
    srv.daemon_threads = True
    print(f"tile_cdn listening on {a.host}:{srv.server_address[1]} ({len(cdn.files)} files from {cdn.base})",
          flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
