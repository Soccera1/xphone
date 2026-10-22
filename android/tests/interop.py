#!/usr/bin/env python3
"""Exercise the production Java/JNI bridge against xphoned and the X11 client's synthetic audio."""
import argparse
import json
import os
from pathlib import Path
import queue
import socket
import subprocess
import tempfile
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument('--server', type=Path, required=True)
parser.add_argument('--client', type=Path, required=True)
parser.add_argument('--build', type=Path, required=True)
args = parser.parse_args()
SERVER, CLIENT, BUILD = args.server.resolve(), args.client.resolve(), args.build.resolve()
JAVA = str(Path(os.environ['JAVA_HOME']) / 'bin/java') if 'JAVA_HOME' in os.environ else 'java'


class Process:
    def __init__(self, command):
        self.p = subprocess.Popen(list(map(str, command)), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, bufsize=1)
        self.queue = queue.Queue()
        self.events, self.errors = [], []
        threading.Thread(target=self.read, daemon=True).start()
        threading.Thread(target=self.read_errors, daemon=True).start()

    def read(self):
        for line in self.p.stdout:
            try: self.queue.put(json.loads(line))
            except ValueError: self.errors.append(line)

    def read_errors(self):
        self.errors.extend(self.p.stderr)

    def send(self, value):
        self.p.stdin.write((value if isinstance(value, str) else json.dumps(value)) + '\n')
        self.p.stdin.flush()

    def wait(self, kind, predicate=lambda e: True, timeout=10):
        deadline = time.monotonic() + timeout
        while True:
            for index, event in enumerate(self.events):
                if event.get('type') == kind and predicate(event): return self.events.pop(index)
            if time.monotonic() >= deadline or self.p.poll() is not None:
                raise AssertionError((kind, self.events, self.errors, self.p.poll()))
            try: self.events.append(self.queue.get(timeout=.1))
            except queue.Empty: pass

    def close(self):
        if self.p.poll() is None:
            self.p.terminate()
            try: self.p.wait(timeout=5)
            except subprocess.TimeoutExpired: self.p.kill(); self.p.wait(timeout=5)


def write(path, value):
    path.write_text(json.dumps(value)); path.chmod(0o600); return path


with tempfile.TemporaryDirectory(prefix='xphone-java-') as directory:
    root = Path(directory)
    cert, key, users = root/'cert.pem', root/'key.pem', root/'users.json'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                    '-keyout', str(key), '-out', str(cert), '-subj', '/CN=localhost',
                    '-addext', 'subjectAltName=DNS:localhost'], check=True, capture_output=True)
    credentials = {name: json.loads(subprocess.check_output([SERVER, 'add-user', name, '--users', users]))
                   for name in ('alice', 'bob')}

    def java(url, token, h2, ca=cert):
        return Process([JAVA, '-Djava.library.path=' + str(BUILD), '-cp', BUILD/'classes',
                        'org.xphone.client.BridgeHarness', url, 'alice', token, ca, str(h2).lower()])

    for mode, http3, force_h2, repeats in [('quic', True, False, 10), ('mixed', True, True, 1),
                                          ('fallback', False, False, 1)]:
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
        config = write(root/'server.json', dict(bind='127.0.0.1', port=port,
                       certificate=str(cert), private_key=str(key), users_file=str(users), http3=http3))
        server = Process([SERVER, '--config', config]); clients = []
        try:
            time.sleep(.15)
            url = f'https://localhost:{port}'
            alice = java(url, credentials['alice']['token'], force_h2); clients.append(alice)
            alice.wait('welcome'); alice.send(dict(type='hello', version=1, capture=2, playback=2))
            transport = alice.wait('_transport')
            assert transport['http'] == ('HTTP/3' if http3 and not force_h2 else 'HTTP/2'), transport
            assert transport['datagrams'] == (http3 and not force_h2), transport
            desktop_config = write(root/'bob.json', dict(credentials['bob'], server=url, ca_file=str(cert), transport='auto'))
            bob = Process([CLIENT, '--config', desktop_config, '--headless', '--synthetic']); clients.append(bob)
            bob.wait('presence', lambda e: any(u['name'] == 'alice' for u in e['users']))
            alice.wait('presence', lambda e: any(u['name'] == 'bob' for u in e['users']))
            for index in range(repeats):
                if index % 2:
                    bob.send(dict(action='call', target='alice'))
                    invitation = alice.wait('incoming'); bob.wait('ringing')
                    alice.send(dict(type='accept', call=invitation['call']))
                else:
                    alice.send(dict(type='call', to='bob'))
                    invitation = alice.wait('ringing'); bob.wait('incoming'); bob.send(dict(action='accept'))
                active = alice.wait('active'); bob.wait('active')
                assert active['tx_channels'] == active['rx_channels'] == 2
                alice.send(f"MEDIA {active['call']} {active['generation']}")
                alice.wait('_media'); alice.wait('received')
                time.sleep(.25)
                alice.send('STOP'); alice.send(dict(type='hangup', call=active['call']))
                alice.wait('ended'); bob.wait('ended')
                stats = bob.wait('audio_stats')
                assert stats['sent'] > 0 and stats['received'] > 0 and stats['right_energy'] > 0, stats
            # Check authentication, hostname validation, and duplicate login failures.
            for token, host, expected in [('0'*64, 'localhost', 'Authentication'),
                    (credentials['alice']['token'], '127.0.0.1', 'Certificate'),
                    (credentials['alice']['token'], 'localhost', 'already connected')]:
                other = java(f'https://{host}:{port}', token, True); clients.append(other)
                failure = other.wait('_closed')
                assert expected in failure['reason'], failure
                other.send('QUIT'); other.p.wait(timeout=5)
            alice.send('QUIT'); assert alice.p.wait(timeout=5) == 0
            print(f'{mode}: Java ↔ native stereo calls, transport, and rejection checks passed', flush=True)
        finally:
            for process in clients: process.close()
            server.close()
