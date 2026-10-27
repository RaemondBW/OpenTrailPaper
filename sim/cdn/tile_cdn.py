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

Cycling-POI sets, <dir>/pois/<set>/ (built by build_poi_fixture.py):
overpass.json, the area's POIs as Overpass returns them, and <id>.poi for each
cell. While a set is in use (/_ctl/pois) the CDN serves its non-empty .poi
files instead of the fixture's, with their sizes and hashes in the group
indexes (an empty one is poiSize 0 and no file, as the CDN does), and
Overpass answers the apps' POI query (and only that) with the set's elements
inside the query's bbox.

Prints "tile_cdn listening on 127.0.0.1:<port>" once it accepts requests.

Test controls, under /_ctl/ (POST, query parameters):

    /_ctl/log                 every request so far, as JSON (GET)
    /_ctl/file?path=/v1/...   what is served at that path now, unlogged (GET)
    /_ctl/area                the fixture's area.json (centre, box, cells), if any (GET)
    /_ctl/reset               forget bumps, faults, the POI set and the log
    /_ctl/pois?set=<name>     serve that POI set (see above); no name: the
                              fixture's own .poi files again, no Overpass POIs
    /_ctl/bump?id=<h3>[&ext=ebm|poi]
                              publish a new version of that file: the same bytes
                              plus an unknown extension section (the firmware
                              skips it by length, map_tiles.cpp), with its new
                              size and hash (sha256[:16]) in the group index
    /_ctl/fault?match=<glob>&kind=truncate|status:<code>|close
    /_ctl/fault?match=/api/interpreter&kind=...
                              misbehave on matching paths (fnmatch on the path
                              without the query, e.g. "*.ebm"): a 200 with half
                              the body, an HTTP error, or a dropped connection;
                              also kind=garble (a 200 of the same length with
                              every byte flipped; for Overpass, an HTML error
                              page) and, for Overpass, kind=remark (a 200 with no
                              elements and the "runtime error" remark Overpass
                              sends when a query times out)
"""
import argparse
import fnmatch
import hashlib
import json
import os
import re
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

# What Overpass sends, with a 200, when a query runs out of time.
OVERPASS_TIMEOUT = (b'{"version":0.6,"generator":"Overpass API 0.7.62.1 084b4234","osm3s":'
                    b'{"timestamp_osm_base":"2026-10-08T20:21:06Z"},"elements":[],'
                    b'"remark":"runtime error: Query timed out in \\"query\\" at line 3 after 61 seconds."}')
GATEWAY_PAGE = (b"<!DOCTYPE html><html><head><title>504 Gateway Time-out</title></head>"
                b"<body><h1>Gateway Time-out</h1></body></html>")
BBOX = re.compile(r"\((-?[0-9.]+),(-?[0-9.]+),(-?[0-9.]+),(-?[0-9.]+)\)")


def overpass_kind(query):
    """"poi", "coast" or "map": which of the apps' queries this is."""
    if "bicycle_repair_station" in query and "highway" not in query:
        return "poi"
    if "coastline" in query and "highway" not in query:
        return "coast"
    return "map"


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
        area = os.path.join(root, "area.json")
        self.area = open(area, "rb").read() if os.path.isfile(area) else None
        self.poi_sets = {}   # name -> (overpass elements, {id: .poi bytes})
        pois = os.path.join(root, "pois")
        for name in sorted(os.listdir(pois)) if os.path.isdir(pois) else []:
            d = os.path.join(pois, name)
            if not os.path.isfile(os.path.join(d, "overpass.json")):
                continue
            elements = json.load(open(os.path.join(d, "overpass.json")))["elements"]
            files = {n[:-4]: open(os.path.join(d, n), "rb").read() for n in os.listdir(d) if n.endswith(".poi")}
            self.poi_sets[name] = (elements, files)
        self.overpass_status = overpass_status
        self.overpass_body = overpass_body
        self.lock = threading.Lock()
        self.reset()

    def reset(self):
        with self.lock:
            self.over = {}     # path -> bytes, or None: gone (bumped files, a POI set, their indexes)
            self.faults = []   # (glob, kind)
            self.log = []
            self.poi_set = None

    def body(self, path):
        with self.lock:
            if path in self.over:
                return self.over[path]
        return self.files.get(path)

    def use_pois(self, name):
        """Serve POI set [name] (None: the fixture's own .poi files)."""
        if name is not None and name not in self.poi_sets:
            raise KeyError(name)
        files = self.poi_sets[name][1] if name else {}
        with self.lock:
            for p in [p for p in self.over if p.endswith(".poi")]:
                del self.over[p]
            self.poi_set = name
        out = {}
        for ipath in [p for p in self.files if p.endswith("/index.json")]:
            g = ipath.split("/")[2]
            idx = json.loads(self.body(ipath))         # with any bumped tiles
            base = json.loads(self.files[ipath])["cells"]
            for cid, e in idx["cells"].items():
                if name is None:                       # the fixture's own
                    e[2], e[3] = base[cid][2], base[cid][3]
                    continue
                body = files.get(cid)
                path = f"/v1/{g}/{cid}.poi"
                if body and struct.unpack_from("<H", body, 6)[0] > 0:
                    e[2], e[3] = len(body), hashlib.sha256(body).hexdigest()[:16]
                    new = body
                else:
                    e[2], e[3] = 0, ""
                    new = None
                with self.lock:
                    self.over[path] = new
                out[cid] = [e[2], e[3]]
            with self.lock:
                self.over[ipath] = json.dumps(idx, separators=(",", ":")).encode()
        return {"set": name, "cells": out}

    def overpass_pois(self, query):
        """The POI set's elements inside the query's bbox, as Overpass answers."""
        elements = self.poi_sets[self.poi_set][0]
        m = BBOX.search(query)
        if m:
            s, w, n, e = map(float, m.groups())

            def inside(el):
                c = el if "lat" in el else el.get("center", {})
                return s <= c.get("lat", 99) <= n and w <= c.get("lon", 999) <= e
            elements = [el for el in elements if inside(el)]
        return json.dumps({"version": 0.6, "generator": "tile_cdn.py stand-in",
                           "osm3s": {"timestamp_osm_base": "2026-10-08T20:21:06Z"},
                           "elements": elements}).encode()

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

        def record(self, status, path, fault=None, overpass=None):
            e = {"method": self.command, "path": path, "status": status, "fault": fault,
                 "ua": self.headers.get("User-Agent", "")}
            if overpass:
                e["overpass"] = overpass   # which query: "poi", "map" or "coast"
            with cdn.lock:
                cdn.log.append(e)

        def ctl(self, path, q):
            arg = lambda k, d=None: q.get(k, [d])[0]
            if path == "/_ctl/log":
                with cdn.lock:
                    return self.send(200, json.dumps(cdn.log).encode(), "application/json")
            if path == "/_ctl/area":
                return self.send(200, cdn.area, "application/json") if cdn.area else self.send(404)
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
            if path == "/_ctl/pois":
                try:
                    r = cdn.use_pois(arg("set") or None)
                except KeyError as e:
                    return self.send(404, json.dumps({"error": f"no POI set {e}", "sets": sorted(cdn.poi_sets)})
                                     .encode(), "application/json")
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
            form = b""
            if self.command == "POST":
                n = int(self.headers.get("Content-Length") or 0)
                if n:
                    form = self.rfile.read(n)
            if path.endswith("/api/interpreter"):
                return self.overpass(path, form, u.query)
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
            if kind == "garble":
                body = bytes(b ^ 0xFF for b in body)
            self.record(200, path, kind)
            ctype = "application/json" if path.endswith(".json") else "application/octet-stream"
            self.send(200, body, ctype)

        def overpass(self, path, form, query):
            q = parse_qs(form.decode("utf-8", "replace") or query).get("data", [""])[0]
            which = overpass_kind(q)
            if which == "poi" and cdn.poi_set:
                status, body, ctype = 200, cdn.overpass_pois(q), "application/json"
            elif cdn.overpass_body is not None:
                status, body, ctype = 200, cdn.overpass_body, "application/json"
            else:
                status, body, ctype = cdn.overpass_status, b"stand-in CDN: no Overpass here", "text/plain"
            kind = cdn.fault_for(path)
            if kind == "close":
                self.record(-1, path, kind, which)
                self.close_connection = True
                return
            if kind and kind.startswith("status:"):
                status, body, ctype = int(kind.split(":", 1)[1]), b"stand-in Overpass fault", "text/plain"
            elif kind == "truncate" and status == 200:
                body = body[: len(body) // 2]
            elif kind == "garble":
                status, body, ctype = 200, GATEWAY_PAGE, "text/html"
            elif kind == "remark":
                status, body, ctype = 200, OVERPASS_TIMEOUT, "application/json"
            self.record(status, path, kind, which)
            self.send(status, body, ctype)

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
