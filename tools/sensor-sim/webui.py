#!/usr/bin/env python3
"""Control panel for the sensor-sim boards — the page talks to the boards itself
over Web Serial (Chrome), including flashing with esptool-js. This server only:

  * serves index.html + vendor/ and the firmware in build/ (GET /fw/flash_args, /fw/<file>)
  * runs `idf.py build` on request (POST /api/build; progress in GET /api/job)
  * relays commands from the command line to the page, which executes them on
    the boards and posts the replies back:

        python3 webui.py                      # serve http://127.0.0.1:8931
        python3 webui.py state                # what the page last reported for every board
        python3 webui.py cmd hr "hr 150"      # run a console line on the board with that role
        python3 webui.py cmd 0 "status"       # ...or by board index shown in the page

The server never opens a serial port; the browser owns them. (With no page open,
`cmd`/`state` time out.)
"""
import json
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PORT = 8931
HERE = Path(__file__).parent
BUILD = HERE / "build"
IDF_PATH = Path.home() / "esp-idf"
IDF_PY_ENV = Path.home() / ".espressif/python_env/idf5.4_py3.13_env"


class Job:
    """One `idf.py build` at a time, output kept for the page."""

    def __init__(self):
        self.lock = threading.Lock()
        self.log = ""
        self.running = False
        self.ok = None
        self.started = False

    def start(self):
        with self.lock:
            if self.running:
                return False
            self.log, self.running, self.ok, self.started = "", True, None, True
        threading.Thread(target=self._run, daemon=True).start()
        return True

    def _run(self):
        cmd = ["bash", "-c",
               f"export IDF_PYTHON_ENV_PATH='{IDF_PY_ENV}'; . '{IDF_PATH}/export.sh' >/dev/null 2>&1 "
               f"&& cd '{HERE}' && idf.py build"]
        try:
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            for line in p.stdout:
                if line.startswith("[") and "Building" in line:
                    continue  # ninja progress spam
                self.log = (self.log + line)[-20000:]
            self.ok = p.wait() == 0
        except Exception as e:
            self.log += f"\n{e!r}\n"
            self.ok = False
        finally:
            self.running = False

    def snapshot(self):
        return {"running": self.running, "ok": self.ok, "log": self.log, "started": self.started}


job = Job()


class Relay:
    """Command queue for the page: CLI enqueues, page polls, executes, posts the reply."""

    def __init__(self):
        self.lock = threading.Lock()
        self.queue = []          # [{id, board, cmd}]
        self.results = {}        # id -> reply
        self.next_id = 1
        self.report = {"at": 0, "boards": []}

    def submit(self, board, cmd):
        with self.lock:
            i = self.next_id
            self.next_id += 1
            self.queue.append({"id": i, "board": board, "cmd": cmd})
        return i

    def take(self):
        with self.lock:
            q, self.queue = self.queue, []
        return q

    def finish(self, i, reply):
        with self.lock:
            self.results[i] = reply

    def wait(self, i, timeout=8):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if i in self.results:
                    return self.results.pop(i)
            time.sleep(0.1)
        return None


relay = Relay()


def firmware_info():
    b = BUILD / "sensor_sim.bin"
    if not b.exists():
        return None
    return {"built": time.strftime("%Y-%m-%d %H:%M", time.localtime(b.stat().st_mtime)),
            "bytes": b.stat().st_size}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _file(self, path, ctype):
        if not path.is_file():
            return self._send(404, {"error": "not found"})
        self._send(200, path.read_bytes(), ctype)

    def do_GET(self):
        p = self.path.split("?")[0]
        if p in ("/", "/index.html"):
            self._file(HERE / "index.html", "text/html; charset=utf-8")
        elif p.startswith("/vendor/") and ".." not in p:
            self._file(HERE / p.lstrip("/"), "text/javascript")
        elif p.startswith("/fw/") and ".." not in p:
            self._file(BUILD / p[4:], "application/octet-stream")
        elif p == "/api/job":
            self._send(200, {"job": job.snapshot(), "firmware": firmware_info()})
        elif p == "/api/queue":
            self._send(200, {"cmds": relay.take()})
        elif p == "/api/state":
            self._send(200, relay.report)
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        try:
            req = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return self._send(400, {"error": "bad json"})
        if self.path == "/api/build":
            self._send(200 if job.start() else 409, job.snapshot())
        elif self.path == "/api/result":            # page -> reply for a relayed command
            relay.finish(req.get("id"), req.get("reply", ""))
            self._send(200, {})
        elif self.path == "/api/report":            # page -> board states, for `webui.py state`
            relay.report = {"at": time.time(), "boards": req.get("boards", [])}
            self._send(200, {})
        elif self.path == "/api/cmd":               # CLI -> queue for the page
            i = relay.submit(req.get("board", ""), req.get("cmd", ""))
            reply = relay.wait(i)
            self._send(200 if reply is not None else 504,
                       {"reply": reply} if reply is not None else {"error": "no page connected"})
        else:
            self._send(404, {"error": "not found"})


def main():
    base = f"http://127.0.0.1:{PORT}"
    if len(sys.argv) > 1 and sys.argv[1] == "state":
        with urllib.request.urlopen(base + "/api/state", timeout=5) as r:
            d = json.load(r)
        age = time.time() - d["at"] if d["at"] else None
        print(json.dumps({"reported_s_ago": None if age is None else round(age, 1),
                          "boards": d["boards"]}, indent=1))
        return
    if len(sys.argv) > 3 and sys.argv[1] == "cmd":
        body = json.dumps({"board": sys.argv[2], "cmd": sys.argv[3]}).encode()
        req = urllib.request.Request(base + "/api/cmd", body, {"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=12) as r:
                print(json.load(r)["reply"])
        except urllib.error.HTTPError as e:
            sys.exit(json.load(e).get("error"))
        return
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print(f"sensor-sim control panel: {base}  (Chrome; boards connect via Web Serial)")
    srv.serve_forever()


if __name__ == "__main__":
    main()
