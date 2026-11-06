import http.server, socketserver, urllib.parse, sys, datetime
class H(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == '/result':
            with open('/tmp/bench-results.log', 'a') as f:
                f.write(datetime.datetime.now().isoformat() + ' ' + urllib.parse.unquote(u.query) + '\n')
            self.send_response(204); self.end_headers(); return
        super().do_GET()
    def log_message(self, *a): pass
socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(('0.0.0.0', 8000), H) as s:
    s.serve_forever()
