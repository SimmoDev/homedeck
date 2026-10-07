#!/usr/bin/env python3
"""A directory listing that takes 50 s to answer, for testing Kodi's slow-listing handling (see tools/README.md)."""
import http.server
import time

DELAY = 50

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        time.sleep(DELAY)
        body = b"<html><body><a href='file.mkv'>file.mkv</a></body></html>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    do_PROPFIND = do_GET
    do_HEAD = do_GET

http.server.ThreadingHTTPServer(("127.0.0.1", 8099), Handler).serve_forever()
