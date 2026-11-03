#!/usr/bin/env python3
"""Local web page for tuning the e-paper bench and running tests on the board.

    tuner.py [port]

Then open http://localhost:8765. Needs pyserial, and owns the serial port while it
runs, so stop it before `pio run -t upload` or console.py. Only listens on this
machine.
"""
import glob
import json
import re
import socketserver
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial

PAGE = Path(__file__).with_name('tuner.html')
KNOBS = ('fast', 'k', 'settle', 'period', 'topup', 'erase', 'paced', 'rowextra', 'strict', 'longgap', 'frames', 'hold')
COMMAND = re.compile(r'^(run [a-z]+ [a-z]+ \d+|video [a-z]+ \d+( [a-z,]+)?|clear)$')
MAX_LINES = 5000


class Board:
    """The serial link. Lines are numbered so the page can ask for what is new."""

    def __init__(self):
        self.lock = threading.Lock()
        self.port = None
        self.link = None
        self.lines = []
        self.first = 0            # number of lines[0]
        self.state = {}
        self.pending = 0          # commands sent and not yet answered with [end]
        self.queue = []
        self.looking_until = 0.0
        self.sent = 0             # tests of this run started so far
        self.total = 0
        self.cancel = threading.Event()
        threading.Thread(target=self.read_forever, daemon=True).start()
        threading.Thread(target=self.work_queue, daemon=True).start()

    def find_port(self):
        for p in sorted(glob.glob('/dev/cu.usbmodem*')):
            if 'NTFAAN' not in p:   # the EDIFIER speaker
                return p
        return None

    def add(self, text):
        with self.lock:
            self.lines.append(text)
            if len(self.lines) > MAX_LINES:
                del self.lines[:1000]
                self.first += 1000

    def read_forever(self):
        while True:
            port = self.find_port()
            if not port:
                time.sleep(1)
                continue
            try:
                with serial.Serial(port, 115200, timeout=0.5) as link:
                    with self.lock:
                        self.port, self.link, self.pending = port, link, 1
                    link.write(b'\nstate\n')
                    while True:
                        raw = link.readline()
                        if raw:
                            self.handle(raw.decode('utf-8', 'replace').rstrip())
            except (serial.SerialException, OSError):
                pass
            with self.lock:
                self.port, self.link, self.pending = None, None, 0
                self.queue.clear()
            self.add('[tuner] board disconnected')
            time.sleep(1)

    def handle(self, text):
        self.add(text)
        if text.startswith('[state] '):
            state = {k: int(v) for k, v in (kv.split('=') for kv in text.split()[1:])}
            with self.lock:
                self.state = state
        elif text == '[end]':
            with self.lock:
                self.pending = max(0, self.pending - 1)

    def send(self, command):
        with self.lock:
            if not self.link:
                return False
            self.pending += 1
            self.link.write(command.encode() + b'\n')
        return True

    def wait_idle(self):
        while not self.cancel.is_set():
            with self.lock:
                if not self.pending:
                    return True
            time.sleep(0.05)
        return False

    def work_queue(self):
        while True:
            with self.lock:
                job = self.queue.pop(0) if self.queue else None
            if not job:
                time.sleep(0.05)
                continue
            command, look = job
            if not self.wait_idle():
                continue
            with self.lock:
                self.sent += 1
            if not self.send(command) or not self.wait_idle():
                continue
            with self.lock:
                more = bool(self.queue)
            if more:
                # A last frame is worth nothing if the next run clears it at once.
                end = time.monotonic() + look
                self.looking_until = end
                while time.monotonic() < end and not self.cancel.is_set():
                    time.sleep(0.05)
                self.looking_until = 0.0

    def start(self, knobs, commands, look):
        with self.lock:
            if self.pending or self.queue:
                return 'busy'
            if not self.link:
                return 'no board'
        self.cancel.clear()
        for name, value in knobs.items():
            self.send(f'set {name} {int(value)}')
        with self.lock:
            self.queue = [(c, look) for c in commands]
            self.sent, self.total = 0, len(commands)
        return None

    def stop(self):
        self.cancel.set()
        with self.lock:
            self.queue.clear()
            # Only a run reads this. With the board idle it would land on the front
            # of the next command, and that command would be lost.
            if self.link and self.pending:
                self.link.write(b'x')

    def status(self, since):
        with self.lock:
            start = max(since, self.first)
            return {
                'connected': bool(self.link), 'port': self.port,
                'busy': bool(self.pending or self.queue), 'state': self.state,
                'test': self.sent, 'tests': self.total,
                'looking': max(0.0, self.looking_until - time.monotonic()),
                'lines': self.lines[start - self.first:],
                'next': self.first + len(self.lines),
            }


board = Board()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, code, body, kind='application/json'):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == '/':
            self.reply(200, PAGE.read_bytes(), 'text/html; charset=utf-8')
        elif self.path.startswith('/api/status'):
            match = re.search(r'since=(\d+)', self.path)
            self.reply(200, board.status(int(match[1]) if match else 0))
        else:
            self.reply(404, {'error': 'not found'})

    def do_POST(self):
        if self.path == '/api/stop':
            board.stop()
            return self.reply(200, {})
        if self.path != '/api/run':
            return self.reply(404, {'error': 'not found'})
        try:
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            knobs = {k: int(v) for k, v in body.get('knobs', {}).items()}
            commands = list(body['commands'])
            look = float(body.get('look', 6))
        except (KeyError, TypeError, ValueError):
            return self.reply(400, {'error': 'bad request'})
        if any(k not in KNOBS for k in knobs) or not commands or \
                not all(isinstance(c, str) and COMMAND.match(c) for c in commands):
            return self.reply(400, {'error': 'bad request'})
        error = board.start(knobs, commands, look)
        self.reply(409 if error else 200, {'error': error} if error else {})


class Server(ThreadingHTTPServer):
    def server_bind(self):
        # HTTPServer's own version looks up the machine's hostname here, which can
        # hang for good before the socket ever starts listening.
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    server = Server(('127.0.0.1', port), Handler)
    print(f'http://localhost:{port}', flush=True)
    server.serve_forever()


main()
