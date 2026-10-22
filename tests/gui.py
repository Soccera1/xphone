#!/usr/bin/env python3
"""Run under xvfb-run: real X11 keyboard/mouse calls and responsive screenshots."""
import json
import os
from pathlib import Path
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import time

server_bin, client_bin, artifacts = map(str, sys.argv[1:4])
screens = Path(artifacts)
screens.mkdir(parents=True, exist_ok=True)


def config(path, value):
    path.write_text(json.dumps(value))
    path.chmod(0o600)
    return str(path)


def xdo(*args):
    return subprocess.check_output(['xdotool', *map(str, args)], text=True).strip()


with tempfile.TemporaryDirectory(prefix='xphone-gui-') as tmp:
    root = Path(tmp)
    cert, key = root/'cert.pem', root/'key.pem'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key),
                    '-out', str(cert), '-days', '1', '-subj', '/CN=localhost', '-addext',
                    'subjectAltName=DNS:localhost'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    users = root/'users.json'
    credentials = {}
    for name in ('alice', 'bob', 'charlie', 'dana', 'erin', 'frank', 'grace', 'helen'):
        credentials[name] = json.loads(subprocess.check_output(
            [server_bin, 'add-user', name, '--users', str(users)], text=True))
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    server_cfg = config(root/'server.json', dict(bind='127.0.0.1', port=port, users_file=str(users),
                        certificate=str(cert), private_key=str(key)))
    processes = []
    logs = open(screens/'gui.log', 'w')
    try:
        processes.append(subprocess.Popen([server_bin, '--config', server_cfg], stdout=logs, stderr=logs))
        time.sleep(.15)
        bob_cfg = config(root/'bob.json', dict(credentials['bob'], server=f'https://localhost:{port}', ca_file=str(cert)))
        bob = subprocess.Popen([client_bin, '--config', bob_cfg, '--headless', '--synthetic', '--auto-answer'],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=logs, text=True, bufsize=1)
        processes.append(bob)
        events = queue.Queue()

        def read_events():
            for line in bob.stdout:
                events.put(json.loads(line))

        threading.Thread(target=read_events, daemon=True).start()
        for name in ('charlie', 'dana', 'erin', 'frank', 'grace', 'helen'):
            extra_cfg = config(root/f'{name}.json', dict(credentials[name], server=f'https://localhost:{port}', ca_file=str(cert)))
            processes.append(subprocess.Popen([client_bin, '--config', extra_cfg, '--headless', '--synthetic'],
                                              stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=logs))

        def wait_event(kind, predicate=lambda e: True):
            until = time.monotonic() + 8
            while time.monotonic() < until:
                try:
                    event = events.get(timeout=.2)
                    if event.get('type') == kind and predicate(event):
                        return event
                except queue.Empty:
                    pass
            raise AssertionError(f'GUI did not trigger {kind}; inspect {screens}/gui.log')

        wait_event('welcome')
        for scale in (1, 2):
            alice_cfg = config(root/'alice.json', dict(credentials['alice'], server=f'https://localhost:{port}',
                                                      ca_file=str(cert), ui_scale=scale))
            gui = subprocess.Popen([client_bin, '--config', alice_cfg, '--synthetic', '--geometry',
                                    f'{360*scale}x{640*scale}', '--quit-after', '25'], stdout=logs, stderr=logs)
            processes.append(gui)
            window = xdo('search', '--sync', '--onlyvisible', '--name', '^XPhone$').splitlines()[-1]
            xdo('windowfocus', window)
            wait_event('presence', lambda e: any(u['name'] == 'alice' for u in e['users']))
            time.sleep(.2)

            def shot(name, width, height):
                xdo('windowsize', window, width*scale, height*scale)
                time.sleep(.15)
                subprocess.run(['import', '-window', window, str(screens/f'{name}-{scale}x.png')], check=True)

            shot('contacts-phone', 320, 480)
            # Keyboard action must work without a mouse or on-screen keyboard.
            xdo('key', 'Return')
            active = wait_event('active')
            assert active['tx_channels'] == active['rx_channels'] == 2
            time.sleep(.2)
            for name, width, height in [('call-phone', 320, 480), ('call-landscape', 480, 320),
                                        ('call-desktop', 900, 640)]:
                shot(name, width, height)
            # Rotate during the live call, mute by keyboard, then hang up by tap/click.
            xdo('key', 'm')
            shot('call-muted', 320, 480)
            xdo('mousemove', '--window', window, 160*scale, 438*scale)
            xdo('click', '1')
            wait_event('ended')
            time.sleep(.15)
            # A drag beginning on a person must scroll, never call or select
            # another person. The subsequent call must still go to bob.
            xdo('mousemove', '--window', window, 140*scale, 210*scale)
            xdo('mousedown', '1')
            xdo('mousemove', '--window', window, 140*scale, 90*scale)
            time.sleep(.1)
            xdo('mouseup', '1')
            time.sleep(.15)
            while not events.empty():
                assert events.get().get('type') != 'incoming', 'Scrolling placed a call'
            subprocess.run(['import', '-window', window, str(screens/f'contacts-scrolled-{scale}x.png')], check=True)
            xdo('mousemove', '--window', window, 160*scale, 438*scale)
            xdo('click', '1')
            wait_event('active')
            time.sleep(.15)
            xdo('click', '1')
            wait_event('ended')
            gui.terminate()
            gui.wait(timeout=5)
            assert gui.returncode in (0, -15), gui.returncode
            time.sleep(.2)
        print(f'X11 keyboard/mouse, phone/desktop resize, 1x/2x scaling passed; screenshots: {screens}')
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        logs.close()
