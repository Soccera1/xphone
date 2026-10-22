#!/usr/bin/env python3
"""Exercise actual C clients/servers over localhost TLS, HTTP/3 and HTTP/2."""
import json
import heapq
import math
import os
from pathlib import Path
import queue
import socket
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import shutil

SERVER, CLIENT = map(str, sys.argv[1:3])


class Process:
    def __init__(self, args):
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, bufsize=1)
        self.events = []
        self.queue = queue.Queue()
        self.errors = []
        threading.Thread(target=self._stdout, daemon=True).start()
        threading.Thread(target=self._stderr, daemon=True).start()

    def _stdout(self):
        for line in self.p.stdout:
            try:
                self.queue.put(json.loads(line))
            except ValueError:
                self.errors.append(line)

    def _stderr(self):
        for line in self.p.stderr:
            self.errors.append(line.rstrip())

    def wait(self, kind, timeout=10, predicate=lambda e: True):
        deadline = time.monotonic() + timeout
        for event in self.events:
            if event.get('type') == kind and predicate(event):
                return event
        while time.monotonic() < deadline:
            try:
                event = self.queue.get(timeout=min(.1, max(.001, deadline - time.monotonic())))
                self.events.append(event)
                if event.get('type') == kind and predicate(event):
                    return event
            except queue.Empty:
                if self.p.poll() is not None:
                    break
        raise AssertionError(f'Waiting for {kind}: events={self.events}, errors={self.errors}, rc={self.p.poll()}')

    def command(self, action, **kwargs):
        self.p.stdin.write(json.dumps(dict(action=action, **kwargs)) + '\n')
        self.p.stdin.flush()

    def close(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait(timeout=5)
        if self.p.returncode not in (0, -15):
            raise AssertionError(f'Process failed {self.p.returncode}: {self.errors}')


class LossyRelay:
    """An unprivileged, one-client QUIC router; never changes host networking."""
    def __init__(self, server_port):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(('127.0.0.1', 0))
        self.socket.settimeout(.005)
        self.port = self.socket.getsockname()[1]
        self.server = ('127.0.0.1', server_port)
        self.client = None
        self.stop = False
        self.count = 0
        self.pending = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.stop:
            now = time.monotonic()
            while self.pending and self.pending[0][0] <= now:
                _, _, data, destination = heapq.heappop(self.pending)
                self.socket.sendto(data, destination)
            try:
                data, source = self.socket.recvfrom(65536)
            except socket.timeout:
                continue
            self.count += 1
            if source == self.server:
                destination = self.client
            else:
                self.client = source
                destination = self.server
            if not destination or self.count % 13 == 0:
                continue
            # Corrupt occasional encrypted packets after initial setup as well.
            if self.count > 30 and self.count % 29 == 0:
                data = data[:-1] + bytes([data[-1] ^ 0x80])
            delay = .04 if self.count % 7 == 0 else .002
            heapq.heappush(self.pending, (now + delay, self.count, data, destination))

    def close(self):
        self.stop = True
        self.thread.join(timeout=2)
        self.socket.close()


def resources(process):
    fields = Path(f'/proc/{process.p.pid}/stat').read_text().split()
    ticks = int(fields[13]) + int(fields[14])
    status = Path(f'/proc/{process.p.pid}/status').read_text().splitlines()
    rss = next(int(line.split()[1]) for line in status if line.startswith('VmRSS:'))
    return ticks / os.sysconf('SC_CLK_TCK'), rss


def write_json(path, obj):
    path.write_text(json.dumps(obj))
    path.chmod(0o600)
    return str(path)


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


with tempfile.TemporaryDirectory(prefix='xphone-test-') as tmp:
    root = Path(tmp)
    cert, key = root/'cert.pem', root/'key.pem'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-keyout', str(key), '-out', str(cert), '-days', '1',
                    '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost'],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    users = root/'users.json'
    credentials = {}
    for name in ('alice', 'bob', 'charlie'):
        result = subprocess.run([SERVER, 'add-user', name, '--users', str(users)],
                                check=True, capture_output=True, text=True)
        credentials[name] = json.loads(result.stdout)
    duplicate = subprocess.run([SERVER, 'add-user', 'alice', '--users', str(users)], capture_output=True)
    assert duplicate.returncode != 0
    assert users.stat().st_mode & 0o777 == 0o600

    measurements = []

    def scenario(a_transport, b_transport, http3=True, mono=False, loss=False, stall=False, real_audio=False):
        if os.environ.get('XPHONE_TEST_ONLY_AUDIO') and not real_audio:
            return
        port = free_port()
        config = write_json(root/'server.json', dict(bind='127.0.0.1', port=port,
                            certificate=str(cert), private_key=str(key), users_file=str(users), http3=http3))
        server = Process([SERVER, '--config', config])
        clients = []
        relays = []
        try:
            time.sleep(.15)
            for name, transport in [('alice', a_transport), ('bob', b_transport)]:
                client_port = port
                if loss:
                    relay = LossyRelay(port)
                    relays.append(relay)
                    client_port = relay.port
                cfg = dict(credentials[name], server=f'https://localhost:{client_port}', ca_file=str(cert), transport=transport)
                if mono and name == 'bob':
                    cfg['audio_channels'] = 'mono'
                filename = write_json(root/f'{name}.json', cfg)
                clients.append(Process([CLIENT, '--config', filename, '--headless'] + ([] if real_audio else ['--synthetic'])))
            alice, bob = clients
            alice.wait('presence', predicate=lambda e: any(u['name'] == 'bob' for u in e['users']))
            bob.wait('presence', predicate=lambda e: any(u['name'] == 'alice' for u in e['users']))
            expected_transports = [('HTTP/3', 'datagrams') if transport == 'auto' and http3 else ('HTTP/2', 'capsules')
                                   for transport in (a_transport, b_transport)]
            for client, (http, media) in zip(clients, expected_transports):
                connected = client.wait('connection')
                assert connected['http'] == http and connected['media'] == media, connected
            if real_audio:
                assert not subprocess.check_output(['pactl', 'list', 'short', 'source-outputs'], text=True).strip(), 'Microphone opened before acceptance'
            idle_start, _ = resources(alice)
            time.sleep(.5)
            idle_end, _ = resources(alice)
            alice.command('call', target='bob')
            bob.wait('incoming')
            alice.wait('ringing')
            alice.command('accept')
            alice.wait('error', predicate=lambda e: e['message'] == 'Call cannot be accepted')
            bob.command('accept')
            active_a = alice.wait('active')
            active_b = bob.wait('active')
            expected = 1 if mono else 2
            assert active_a['tx_channels'] == active_a['rx_channels'] == expected
            assert active_b['tx_channels'] == active_b['rx_channels'] == expected
            alice.wait('status', predicate=lambda e: e.get('message') == 'Connected')
            bob.wait('status', predicate=lambda e: e.get('message') == 'Connected')
            active_start, rss_before = resources(alice)
            measured_start = time.monotonic()
            time.sleep(1.5 if loss else .8)
            if real_audio:
                subprocess.check_output(['pactl', 'load-module', 'module-null-sink',
                                         'sink_name=xphone_mono', 'channels=1', 'rate=48000'])
                subprocess.run(['pactl', 'set-default-sink', 'xphone_mono'], check=True)
                streams = subprocess.check_output(['pactl', 'list', 'short', 'sink-inputs'], text=True)
                for line in streams.splitlines():
                    subprocess.run(['pactl', 'move-sink-input', line.split()[0], 'xphone_mono'], check=True)
                for client in clients:
                    client.wait('active', predicate=lambda e: e['generation'] > active_a['generation'] and
                                e['tx_channels'] == e['rx_channels'] == 1)
                time.sleep(.4)
            if stall:
                os.kill(bob.p.pid, signal.SIGSTOP)
                time.sleep(.8)
                os.kill(bob.p.pid, signal.SIGCONT)
                time.sleep(.4)
                _, rss_after = resources(server)
                assert rss_after < 200000, 'Relay memory exceeded bounded test budget'
            active_end, rss_after = resources(alice)
            measurements.append(dict(transport=expected_transports[0][0], loss=loss, real_audio=real_audio,
                                     idle_cpu_percent=round((idle_end-idle_start)/.5*100, 2),
                                     call_cpu_percent=round((active_end-active_start)/(time.monotonic()-measured_start)*100, 2),
                                     rss_kib=rss_after))
            alice.command('mute')
            time.sleep(.3)
            alice.command('hangup')
            alice.wait('ended')
            bob.wait('ended')
            for client in clients:
                stats = client.wait('audio_stats')
                assert stats['sent'] > 10 and stats['received'] > 5, stats
                assert stats['left_energy'] > 0, stats
                if not mono:
                    assert stats['right_energy'] > 0, stats
            if real_audio:
                time.sleep(.1)
                assert not subprocess.check_output(['pactl', 'list', 'short', 'source-outputs'], text=True).strip(), 'Microphone remained open after hangup'
            expected_a = 'HTTP/3' if a_transport == 'auto' and http3 else 'HTTP/2'
            assert any(expected_a in line for line in server.errors), server.errors
            # Same connections can place another call and reject it cleanly.
            alice.command('call', target='bob')
            bob.wait('incoming', predicate=lambda e: e['call'] != active_a['call'])
            bob.command('reject')
            bob.wait('ended', predicate=lambda e: e.get('reason') == 'Call rejected')
            if a_transport == b_transport == 'auto' and http3 and not loss:
                # Exceed initial QUIC stream credit, proving it is renewed
                # correctly and that closed media streams are reclaimed.
                previous = active_a['call']
                seen_calls = {event['call'] for event in bob.events if event.get('type') == 'incoming'}
                for _ in range(9):
                    alice.command('call', target='bob')
                    incoming = bob.wait('incoming', predicate=lambda e: e['call'] not in seen_calls)
                    previous = incoming['call']
                    seen_calls.add(previous)
                    bob.command('accept')
                    alice.wait('active', predicate=lambda e: e['call'] == previous)
                    bob.wait('active', predicate=lambda e: e['call'] == previous)
                    alice.wait('status', predicate=lambda e: e.get('call') == previous and e['message'] == 'Connected')
                    bob.wait('status', predicate=lambda e: e.get('call') == previous and e['message'] == 'Connected')
                    alice.command('hangup')
                    alice.wait('ended', predicate=lambda e: e['call'] == previous)
                    bob.wait('ended', predicate=lambda e: e['call'] == previous)
            print(f'call passed: {a_transport}/{b_transport}, server H3={http3}, channels={expected}, loss={loss}, real_audio={real_audio}')
        finally:
            for client in clients:
                client.close()
            server.close()
            for relay in relays:
                relay.close()

    scenario('http2', 'http2')
    scenario('auto', 'auto')
    scenario('auto', 'http2')
    scenario('auto', 'auto', http3=False, mono=True)
    scenario('auto', 'auto', loss=True)
    scenario('http2', 'http2', stall=True)

    port = free_port()
    server_config = write_json(root/'server.json', dict(bind='127.0.0.1', port=port,
        certificate=str(cert), private_key=str(key), users_file=str(users)))
    server = Process([SERVER, '--config', server_config])
    time.sleep(.15)
    try:
        for transport in (() if os.environ.get('XPHONE_TEST_ONLY_AUDIO') else ('auto', 'http2')):
            for failure in ('token', 'hostname', 'trust'):
                cfg = dict(credentials['alice'], server=f'https://localhost:{port}', ca_file=str(cert), transport=transport)
                if failure == 'token': cfg['token'] = '0' * 64
                elif failure == 'hostname': cfg['server'] = f'https://127.0.0.1:{port}'
                else: del cfg['ca_file']
                filename = write_json(root/'bad.json', cfg)
                client = Process([CLIENT, '--config', filename, '--headless', '--synthetic'])
                try:
                    reason = 'Authentication' if failure == 'token' else 'Certificate'
                    client.wait('status', predicate=lambda e: reason in e.get('message', ''))
                    assert not any(e.get('type') == 'welcome' for e in client.events)
                finally:
                    client.close()
        print('negative TLS/authentication checks passed over both transports')
        if not os.environ.get('XPHONE_TEST_ONLY_AUDIO'):
            state_clients = []
            try:
                for name in ('alice', 'bob', 'charlie'):
                    cfg = dict(credentials[name], server=f'https://localhost:{port}', ca_file=str(cert))
                    filename = write_json(root/f'state-{name}.json', cfg)
                    state_clients.append(Process([CLIENT, '--config', filename, '--headless', '--synthetic']))
                alice, bob, charlie = state_clients
                alice.wait('presence', predicate=lambda e: len(e['users']) == 3)
                bob.wait('presence', predicate=lambda e: len(e['users']) == 3)
                charlie.wait('presence', predicate=lambda e: len(e['users']) == 3)
                duplicate = Process([CLIENT, '--config', str(root/'state-alice.json'), '--headless', '--synthetic'])
                try:
                    duplicate.wait('status', predicate=lambda e: e['message'] == 'Username already connected')
                finally:
                    duplicate.close()
                charlie.command('call', target='charlie')
                charlie.wait('error', predicate=lambda e: e['message'] == 'Cannot call yourself')
                charlie.command('call', target='nobody')
                charlie.wait('error', predicate=lambda e: e['message'] == 'User is offline')
                alice.command('call', target='bob')
                bob.wait('incoming')
                charlie.command('call', target='bob')
                charlie.wait('error', predicate=lambda e: e['message'] == 'User is busy')
                # Verify the real configured ringing deadline, rather than a mock.
                timed_out = alice.wait('ended', timeout=34, predicate=lambda e: e['reason'] == 'No answer')
                bob.wait('ended', predicate=lambda e: e['call'] == timed_out['call'])
                alice.command('call', target='bob')
                bob.wait('incoming', predicate=lambda e: e['call'] != timed_out['call'])
                bob.command('accept')
                alice.wait('active')
                bob.wait('active')
                time.sleep(.2)
                bob.close()
                alice.wait('ended', predicate=lambda e: e['reason'] == 'Peer disconnected')
                print('duplicate, self/offline/busy calls, 30-second timeout, and disconnect cleanup passed')
            finally:
                for client in state_clients:
                    client.close()
    finally:
        server.close()

    if shutil.which('pulseaudio') and shutil.which('pactl') and shutil.which('dbus-run-session'):
        # A separate daemon with generated input and a null sink. No physical
        # microphone, speakers, defaults, or modules in the user's daemon change.
        pulse_socket = root/'pulse.sock'
        fifo = root/'input.pcm'
        runtime = root/'pulse-runtime'
        runtime.mkdir(mode=0o700)
        pulse_env = dict(os.environ, PULSE_RUNTIME_PATH=str(runtime), PULSE_STATE_PATH=str(root/'pulse-state'))
        pulse_log = open(root/'pulse.log', 'w')
        pulse = subprocess.Popen(['dbus-run-session', '--', 'pulseaudio', '-n', '--daemonize=no', '--use-pid-file=no', '--exit-idle-time=-1',
            '--disable-shm=yes', '--log-level=warning',
            f'--load=module-native-protocol-unix socket={pulse_socket} auth-anonymous=1',
            '--load=module-null-sink sink_name=xphone_output channels=2 rate=48000',
            f'--load=module-pipe-source source_name=xphone_input file={fifo} format=s16le channels=2 rate=48000'],
            env=pulse_env, stdout=pulse_log, stderr=pulse_log, start_new_session=True)
        writer_stop = threading.Event()
        previous_server = os.environ.get('PULSE_SERVER')
        os.environ['PULSE_SERVER'] = f'unix:{pulse_socket}'

        def write_tones():
            fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
            sample = 0
            try:
                while not writer_stop.is_set():
                    pcm = b''.join(struct.pack('<hh', int(6000*math.sin(2*math.pi*440*(sample+i)/48000)),
                                                       int(6000*math.sin(2*math.pi*880*(sample+i)/48000))) for i in range(960))
                    try:
                        os.write(fd, pcm)
                    except BlockingIOError:
                        pass
                    sample += 960
                    writer_stop.wait(.02)
            finally:
                os.close(fd)

        writer = None
        try:
            deadline = time.monotonic() + 5
            while (not pulse_socket.exists() or not fifo.exists()) and pulse.poll() is None and time.monotonic() < deadline:
                time.sleep(.02)
            assert pulse.poll() is None and pulse_socket.exists(), (root/'pulse.log').read_text()
            subprocess.run(['pactl', 'set-default-source', 'xphone_input'], check=True)
            subprocess.run(['pactl', 'set-default-sink', 'xphone_output'], check=True)
            writer = threading.Thread(target=write_tones, daemon=True)
            writer.start()
            scenario('auto', 'http2', real_audio=True)
        finally:
            writer_stop.set()
            if writer:
                writer.join(timeout=2)
            os.killpg(pulse.pid, signal.SIGTERM)
            pulse.wait(timeout=5)
            pulse_log.close()
            if previous_server is None:
                os.environ.pop('PULSE_SERVER', None)
            else:
                os.environ['PULSE_SERVER'] = previous_server
    print('resource samples (one CPU core = 100%): ' + json.dumps(measurements))
