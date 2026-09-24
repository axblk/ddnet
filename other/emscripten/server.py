#!/usr/bin/env python3

# Simple web server that sends CORS headers to test the Emscripten build.
# From https://stackoverflow.com/a/21957017

from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer, test
import sys

class RequestHandler(SimpleHTTPRequestHandler):
	def end_headers(self):
		self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
		self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
		SimpleHTTPRequestHandler.end_headers(self)

if __name__ == '__main__':
	# One request at a time is not enough: the programs read their data over
	# HTTP from several threads at once, and a worker that waits for a file
	# holds the connection while it does. A server that answers one at a time
	# is then waited for by everybody, which looks exactly like a hung page.
	test(RequestHandler, ThreadingHTTPServer, port=int(sys.argv[1]) if len(sys.argv) > 1 else 8000)
