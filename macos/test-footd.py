#!/usr/bin/env python3
#
# footd (macos/footd.c), driven as foot's windows do: sessions are
# created, detached from, attached to again (getting their state), and
# ended; footd exits when there's nothing left. No windows involved.
#
# Then sessions on another machine: a second footd, behind footd
# --bridge, that a script stands in for ssh to ($FOOTD_SSH).
#
# Usage: test-footd.py <footd>

import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time

VERSION = 1

HELLO, CREATE, ATTACH, RESIZE, KILL, LIST, LAYOUT = range(1, 8)
WELCOME, STARTED, FAILED, DETACHED, SESSIONS = range(100, 105)
FEATURE_REMOTE = 1 << 0

TIMEOUT = 20


class Failure(Exception):
    pass


def u8(v): return struct.pack('<B', v)
def u32(v): return struct.pack('<I', v)
def i32(v): return struct.pack('<i', v)
def u64(v): return struct.pack('<Q', v)
def size(cols, rows, cw=8, ch=16): return struct.pack('<HHHH', cols, rows, cw, ch)
def string(s): b = s.encode(); return u32(len(b)) + b
def strv(v): return u32(len(v)) + b''.join(string(s) for s in v)


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise Failure('message too short')
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def u8(self): return struct.unpack('<B', self.take(1))[0]
    def u32(self): return struct.unpack('<I', self.take(4))[0]
    def i32(self): return struct.unpack('<i', self.take(4))[0]
    def u64(self): return struct.unpack('<Q', self.take(8))[0]
    def blob(self): return self.take(self.u32())
    def str(self): return self.blob().decode()


class Client:
    """A window's control connection"""

    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.buf = b''
        self.detached = []  # DETACHED notifications, in any order with replies

    def send(self, type, payload=b'', fd=None):
        msg = struct.pack('<II', type, len(payload)) + payload
        if fd is None:
            self.sock.sendall(msg)
        else:
            socket.send_fds(self.sock, [msg], [fd])

    def recv(self):
        deadline = time.time() + TIMEOUT
        while True:
            if len(self.buf) >= 8:
                type, n = struct.unpack('<II', self.buf[:8])
                if len(self.buf) >= 8 + n:
                    payload = self.buf[8:8 + n]
                    self.buf = self.buf[8 + n:]
                    return type, Reader(payload)
            if time.time() > deadline:
                raise Failure('no message from footd')
            r, _, _ = select.select([self.sock], [], [], 0.5)
            if r:
                data = self.sock.recv(65536)
                if not data:
                    raise Failure('footd closed the connection')
                self.buf += data

    def expect(self, type):
        if type == DETACHED and self.detached:
            return self.detached.pop(0)
        while True:
            t, r = self.recv()
            if t == DETACHED and type != DETACHED:
                self.detached.append(r)
                continue
            if t != type:
                raise Failure(f'expected message {type}, got {t}')
            return r

    def hello(self):
        self.send(HELLO, u32(VERSION) + i32(os.getpid()))
        r = self.expect(WELCOME)
        if r.u32() != VERSION:
            raise Failure('protocol version')
        r.i32()
        self.features = r.u32()

    def close(self):
        self.sock.close()


class Window:
    """A terminal's socket: what the PTY would be"""

    def __init__(self):
        self.ours, self.theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        self.output = b''

    def give(self):
        return self.theirs.fileno()

    def sent(self):
        """
        Only once footd has it: macOS' UNIX socket garbage collection may
        flush a socket in flight that nobody else has open
        """
        self.theirs.close()

    def read_until(self, needle):
        deadline = time.time() + TIMEOUT
        while needle not in self.output:
            if time.time() > deadline:
                raise Failure(f'{needle!r} not received; got {self.output[-500:]!r}')
            r, _, _ = select.select([self.ours], [], [], 0.5)
            if r:
                data = self.ours.recv(65536)
                if not data:
                    raise Failure(f'closed before {needle!r}; got {self.output[-500:]!r}')
                self.output += data
        return self.output

    def read_eof(self):
        deadline = time.time() + TIMEOUT
        while True:
            if time.time() > deadline:
                raise Failure('not closed')
            r, _, _ = select.select([self.ours], [], [], 0.5)
            if r:
                data = self.ours.recv(65536)
                if not data:
                    return self.output
                self.output += data

    def write(self, data):
        self.ours.sendall(data)

    def close(self):
        self.ours.close()


def create(client, id, token, argv, cols=80, rows=24, cwd='/', host=None):
    win = Window()
    payload = (u64(id) + u64(token) + size(cols, rows) + string(cwd) +
               string(CONFIG) + string('xterm-256color') + string('/bin/sh') + u8(0) +
               u32(1000) + strv(argv) + strv([f'{k}={v}' for k, v in ENV.items()]))
    if host is not None:
        payload += string(host)
    client.send(CREATE, payload, win.give())
    r = client.expect(STARTED)
    win.sent()
    if r.u64() != id or r.u64() != token:
        raise Failure('STARTED for another session')
    pid = r.i32()
    # On another machine: its PID isn't one here
    if (pid != 0) if host else (pid <= 0):
        raise Failure(f'PID {pid}')
    return win, pid


def attach(client, id, token, cols=80, rows=24):
    win = Window()
    client.send(ATTACH, u64(id) + u64(token) + size(cols, rows), win.give())
    while True:
        t, r = client.recv()
        if t != DETACHED:
            break
        client.detached.append(r)
    win.sent()
    if t == FAILED:
        r.u64()
        r.u64()
        raise Failure(f'attach failed: {r.str()}')
    if t != STARTED:
        raise Failure(f'expected STARTED, got {t}')
    return win


def list_sessions(client):
    client.send(LIST)
    r = client.expect(SESSIONS)
    sessions = []
    for _ in range(r.u32()):
        sessions.append({'id': r.u64(), 'attached': bool(r.u8()), 'pid': r.i32(),
                         'title': r.str(), 'cwd': r.str()})
    return sessions, r.blob()


def wait_exit(proc):
    try:
        return proc.wait(timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        raise Failure('footd did not exit')


def children(pid):
    out = subprocess.run(['pgrep', '-P', str(pid)], capture_output=True, text=True).stdout
    return [int(p) for p in out.split()]


def test_remote(footd, tmp):
    """Sessions on another machine: footd --bridge to a second footd, a script for ssh"""
    remote_path = os.path.join(tmp, 'r.sock')
    ssh = os.path.join(tmp, 'ssh')
    with open(ssh, 'w') as f:
        # ssh's arguments: -T -o BatchMode=yes -- HOST COMMAND
        f.write('#!/bin/sh\n'
                '[ "$5" = testhost ] || { echo "host: $5" >&2; exit 1; }\n'
                'exec "$TEST_FOOTD" --bridge --socket="$TEST_REMOTE" --log-to-stderr\n')
    os.chmod(ssh, 0o755)

    path = os.path.join(tmp, 'l.sock')
    env = dict(os.environ, FOOTD_SSH=ssh, TEST_FOOTD=os.path.abspath(footd),
               TEST_REMOTE=remote_path, REMOTE_MARK='there', HOME=tmp)
    proc = subprocess.Popen([footd, f'--socket={path}', '--log-to-stderr', '--log-level=warning'],
                            env=env)
    try:
        deadline = time.time() + TIMEOUT
        while not os.path.exists(path):
            if time.time() > deadline or proc.poll() is not None:
                raise Failure('footd did not start')
            time.sleep(.02)

        c = Client(path)
        c.hello()
        if not c.features & FEATURE_REMOTE:
            raise Failure('no remote sessions (features)')

        # There: the window's size; that machine's environment, not the window's
        global ENV
        ENV = dict(ENV, WINDOW_MARK='here')
        script = ('stty size; echo "w=${WINDOW_MARK:-} r=${REMOTE_MARK:-}"; echo ready; '
                  'read x; echo "got $x"; read y; echo bye')
        win, _ = create(c, 100, 1, ['/bin/sh', '-c', script], cols=90, rows=20, host='testhost')
        out = win.read_until(b'ready')
        if b'20 90' not in out:
            raise Failure(f'remote: stty size: {out!r}')
        if b'w= r=there' not in out:
            raise Failure(f'remote: environment: {out!r}')
        win.write(b'foo\n')
        win.read_until(b'got foo')

        # Detached: it runs on, there; attached again, it has its state
        win.close()
        time.sleep(.3)
        sessions, _ = list_sessions(c)
        if [(s['id'], s['attached']) for s in sessions] != [(100, False)]:
            raise Failure(f'remote: after detaching: {sessions}')
        win = attach(c, 100, 2, cols=90, rows=20)
        win.read_until(b'got foo')
        win.write(b'bar\n')
        win.read_eof()
        if b'bye' not in win.output:
            raise Failure(f'remote: the end: {win.output[-200:]!r}')
        time.sleep(.2)
        sessions, _ = list_sessions(c)
        if sessions:
            raise Failure(f'remote: an ended session is left: {sessions}')

        # The connection is lost: the window stays, is told, and typing connects again
        win, _ = create(c, 101, 3, ['/bin/sh', '-c', 'echo up; read x; echo "got $x"'],
                        host='testhost')
        win.read_until(b'up')
        for pid in children(proc.pid):
            os.kill(pid, 9)
        win.read_until(b'connection to testhost is lost')
        win.output = b''
        win.write(b'\n')
        win.read_until(b'up')       # Its state, anew
        c.expect(STARTED)
        win.write(b'again\n')
        win.read_until(b'got again')
        win.read_eof()

        # Closing a window (its socket, then KILL) ends its session, there too
        win, _ = create(c, 102, 4, ['/bin/sh', '-c', 'echo up; sleep 100'], host='testhost')
        win.read_until(b'up')
        win.close()
        c.send(KILL, u64(102) + u64(4))
        there = Client(remote_path)
        there.hello()
        deadline = time.time() + TIMEOUT
        while list_sessions(there)[0]:
            if time.time() > deadline:
                raise Failure(f'remote: not ended there: {list_sessions(there)[0]}')
            time.sleep(.05)
        there.close()

        # Without sessions and windows, both footds exit (the remote one: its bridge gone)
        c.close()
        if wait_exit(proc) != 0:
            raise Failure('remote: footd failed')
        deadline = time.time() + TIMEOUT
        while os.path.exists(remote_path):
            if time.time() > deadline:
                raise Failure('remote: the other footd did not exit')
            time.sleep(.05)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()


def main():
    global CONFIG, ENV

    footd = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix='footd-test-')
    path = os.path.join(tmp, 's.sock')

    CONFIG = os.path.join(tmp, 'foot.ini')
    open(CONFIG, 'w').close()
    ENV = {'PATH': '/usr/bin:/bin', 'HOME': tmp, 'LANG': 'en_US.UTF-8'}

    proc = subprocess.Popen([footd, f'--socket={path}', '--log-to-stderr', '--log-level=warning'])
    try:
        deadline = time.time() + TIMEOUT
        while not os.path.exists(path):
            if time.time() > deadline or proc.poll() is not None:
                raise Failure('footd did not start')
            time.sleep(.02)

        c = Client(path)
        c.hello()

        # A session: what runs in it gets the window's size, and its input
        script = 'stty size; printf "\\033]2;a title\\007"; echo ready; read x; echo "got $x"; read y; echo bye'
        win, pid = create(c, 1, 11, ['/bin/sh', '-c', script], cols=100, rows=30)
        win.read_until(b'ready')
        if b'30 100' not in win.output:
            raise Failure(f'stty size: {win.output!r}')

        # Detached (the window is gone): it keeps running
        win.close()
        time.sleep(.2)
        sessions, _ = list_sessions(c)
        if len(sessions) != 1 or sessions[0]['attached'] or sessions[0]['pid'] != pid:
            raise Failure(f'after detaching: {sessions}')
        if sessions[0]['title'] != 'a title':
            raise Failure(f'title: {sessions[0]["title"]!r}')
        os.kill(pid, 0)

        # Attached again (in another size): the state comes first
        win = attach(c, 1, 12, cols=90, rows=20)
        win.read_until(b'ready')
        if b'\x1b]2;a title' not in win.output:
            raise Failure('the snapshot has no title')
        win.write(b'foo\n')
        win.read_until(b'got foo')

        # A second window takes over: the first one's socket is closed
        win2 = attach(c, 1, 13)
        c.expect(DETACHED)
        win.read_eof()
        win2.read_until(b'got foo')

        # The program exits: the window gets the rest, then the end
        win2.write(b'bar\n')
        out = win2.read_eof()
        if b'bye' not in out:
            raise Failure(f'last output missing: {out[-200:]!r}')
        time.sleep(.2)
        sessions, _ = list_sessions(c)
        if sessions:
            raise Failure(f'session still there: {sessions}')

        # Closing a window ends its session (SIGHUP)
        win3, pid3 = create(c, 2, 21, ['/bin/sh', '-c', 'echo up; sleep 100'])
        win3.read_until(b'up')
        c.send(KILL, u64(2) + u64(21))
        win3.read_eof()
        deadline = time.time() + TIMEOUT
        while True:
            try:
                os.kill(pid3, 0)
            except ProcessLookupError:
                break
            if time.time() > deadline:
                raise Failure('killed session still running')
            time.sleep(.05)

        # Only the attached window may end it
        win4, _ = create(c, 3, 31, ['/bin/sh', '-c', 'echo up; read x; echo "got $x"'])
        win4.read_until(b'up')
        c.send(KILL, u64(3) + u64(99))
        win4.write(b'still\n')
        win4.read_until(b'got still')
        win4.read_eof()

        # A large output, and a window that's slow to read it
        win5, _ = create(c, 4, 41, ['/bin/sh', '-c', 'yes 0123456789 | head -n 200000; echo done; read x'])
        time.sleep(1)
        out = win5.read_until(b'done')
        if out.count(b'0123456789') < 200000:
            raise Failure(f'output lost: {out.count(b"0123456789")} lines')
        win5.write(b'\n')
        win5.read_eof()

        # Output ending in an incomplete sequence: a window that attaches
        # gets it after the snapshot. A UTF-8 character after a sequence,
        # one within an OSC (the OSC is what's incomplete), and a CSI.
        script = ('stty -echo; printf "A\\033[1m\\303"; read x; '
                  'printf "\\244\\033]2;t\\303"; read x; '
                  'printf "\\244\\007\\033[3"; read x; printf "1mZ\\n"; read x')
        win, _ = create(c, 6, 61, ['/bin/sh', '-c', script])
        win.read_until(b'\xc3')
        steps = [
            (62, b'\x1b]2;t\xc3', b'\xc3\xa4\x1b]2;t\xc3'),
            (63, b'\xa4\x07\x1b[3', b'\x1b]2;t\xc3\xa4\x07\x1b[3'),
            (64, b'1mZ', b'\x1b[31mZ'),
        ]
        for token, until, expected in steps:
            win.close()
            time.sleep(.2)
            win = attach(c, 6, token)
            win.write(b'x\n')
            out = win.read_until(until)
            if expected not in out:
                raise Failure(f'incomplete sequence: {expected!r} not in {out[-300:]!r}')
        sessions, _ = list_sessions(c)
        if [s['title'] for s in sessions if s['id'] == 6] != ['t\u00e4']:
            raise Failure(f'title: {sessions}')
        c.send(KILL, u64(6) + u64(64))
        win.read_eof()

        # Another footd, for the same socket: it doesn't take over
        other = subprocess.run([footd, f'--socket={path}', '--log-level=none'],
                               timeout=TIMEOUT)
        if other.returncode == 0:
            raise Failure('a second footd ran')
        Client(path).close()

        # Quitting foot ending its sessions: the ones no window is attached to
        win7, pid7 = create(c, 7, 71, ['/bin/sh', '-c', 'echo up; sleep 100'])
        win7.read_until(b'up')
        win7.close()
        win8, _ = create(c, 8, 81, ['/bin/sh', '-c', 'echo up; read x; echo "got $x"'])
        win8.read_until(b'up')
        c.send(KILL, u64(0) + u64(0))
        deadline = time.time() + TIMEOUT
        while [s['id'] for s in list_sessions(c)[0]] != [8]:
            if time.time() > deadline:
                raise Failure(f'after ending detached sessions: {list_sessions(c)[0]}')
            time.sleep(.05)
        win8.write(b'still\n')
        win8.read_until(b'got still')
        win8.read_eof()

        # The layout is kept
        c.send(LAYOUT, string('a layout'))
        _, layout = list_sessions(c)
        if layout != b'a layout':
            raise Failure(f'layout: {layout!r}')

        # Another protocol version is turned away
        other = Client(path)
        other.send(HELLO, u32(VERSION + 1) + i32(os.getpid()))
        try:
            other.recv()
            raise Failure('another protocol version was welcomed')
        except Failure as e:
            if 'closed' not in str(e):
                raise
        other.close()

        # With sessions left, footd stays; without, and nobody connected, it exits
        win6, _ = create(c, 5, 51, ['/bin/sh', '-c', 'echo up; sleep 100'])
        win6.read_until(b'up')
        win6.close()
        c.close()
        time.sleep(.5)
        if proc.poll() is not None:
            raise Failure('footd exited with a session left')

        c = Client(path)
        c.hello()
        sessions, _ = list_sessions(c)
        if [s['id'] for s in sessions] != [5]:
            raise Failure(f'sessions: {sessions}')
        win6 = attach(c, 5, 52)
        win6.read_until(b'up')
        c.send(KILL, u64(5) + u64(52))
        win6.read_eof()
        c.close()

        if wait_exit(proc) != 0:
            raise Failure('footd failed')
        if os.path.exists(path):
            raise Failure('the socket is left behind')

        test_remote(footd, tmp)

        print('footd: all tests passed')
        return 0

    except Failure as e:
        print(f'FAIL: {e}', file=sys.stderr)
        return 1
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()


if __name__ == '__main__':
    sys.exit(main())
