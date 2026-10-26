#!/usr/bin/env python3
"""Serves desktop/ over http for the browser check of Story 9.9: src/index.html gets tests/support/mock_device.js in front of app.js (a paired fake unit instead of Tauri).
Usage: python3 desktop/tests/support/serve_mock.py [port]   then open http://127.0.0.1:<port>/src/index.html (default port 8765)."""
import http.server
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    def do_GET(self):
        if self.path.split("?")[0] in ("/src/index.html", "/src/"):
            with open(os.path.join(ROOT, "src", "index.html"), encoding="utf-8") as fh:
                html = fh.read().replace('<script type="module" src="./app.js"></script>',
                                         '<script type="module" src="../tests/support/mock_device.js"></script>\n  <script type="module" src="./app.js"></script>')
            body = html.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
