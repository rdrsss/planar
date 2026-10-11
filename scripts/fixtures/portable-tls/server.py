import http.server, ssl
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body=b'{"key":"TLS-1","fields":{"summary":"TLS fixture","status":{"name":"To Do"},"updated":"2026-10-06T00:00:00Z"}}'
        self.send_response(200)
        self.send_header('Content-Length',str(len(body)))
        self.end_headers()
        self.wfile.write(body)
class Server(http.server.HTTPServer):
    def get_request(self):
        try:
            return super().get_request()
        except ssl.SSLError as error:
            print('TLS_HANDSHAKE_REJECTION='+error.reason,flush=True)
            raise
server=Server(('0.0.0.0',4443),Handler)
ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain('/fixture/server.pem','/fixture/server.key')
server.socket=ctx.wrap_socket(server.socket,server_side=True)
server.serve_forever()
