#!/usr/bin/env python3
"""Exercise production memory evaluation over HTTP with deterministic model replies.

This verifies wiring and persistence, not real Qwen decision quality.
"""
import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

requests = []
errors = []
class Model(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        text = next(m['content'] for m in request['messages'] if m['role'] == 'user')
        index = len(requests)
        requests.append(text)
        expected = [
            ('LATEST USER MESSAGE:', 'CHAT_EVIDENCE_TAIL', 'CHAT_RESPONSE_TAIL', 'TOOL_OUTCOME_TAIL', 'Evaluate only this exchange.'),
            ('RECORDED REFLECTION CONTEXT', 'SENSORY_EVIDENCE_TAIL', 'REFLECTION_RESPONSE_TAIL', 'Evaluate only this recorded experience and reflection.'),
            ('RECORDED REFLECTION CONTEXT', 'NOTHING_NEW', 'No new conclusion.', 'Evaluate only this recorded experience and reflection.'),
        ]
        if index >= len(expected) or not all(x in text for x in expected[index]):
            errors.append(f'Unexpected evaluation payload {index}')
        result = ['preference | I prefer quiet music.',
                  '  knowledge  | I noticed the displayed flower opened.\n', 'NONE'][min(index, 2)]
        body = json.dumps({'message': {'role': 'assistant', 'content': result}, 'done': True}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

server = HTTPServer(('127.0.0.1', 11435), Model)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    env = dict(os.environ, R2_OLLAMA_BASE_URL='http://127.0.0.1:11435', R2_OLLAMA_NUM_CTX='16384')
    subprocess.run([sys.argv[1]], env=env, check=True, timeout=60)
    assert len(requests) == 3, len(requests)
    assert not errors, errors
finally:
    server.shutdown()
    thread.join()
    server.server_close()
