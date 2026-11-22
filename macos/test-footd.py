#!/usr/bin/env python3
#
# footd (macos/footd.c), driven as foot's windows do: sessions are
# created, detached from, attached to again (getting their state), and
# ended; footd exits when there's nothing left. No windows involved.
#
# Then sessions on another machine: a second footd, behind footd
# --bridge, that a script stands in for ssh to ($FOOTD_SSH). And that
# machine's ports: a server here stands in for one there.
#
# Usage: test-footd.py <footd>

import hashlib
import os
import select
import shutil
import signal
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time

VERSION = 1

HELLO, CREATE, ATTACH, RESIZE, KILL, LIST, LAYOUT, UNINSTALL, WATCH, ASKPASS, ANSWER, \
    FORWARD, UNFORWARD, PORTS = range(1, 15)
WELCOME, STARTED, FAILED, DETACHED, SESSIONS, UNINSTALLED, REMOTES, ASK, ASKED, ANSWERED, \
    FORWARDED, LISTENING = range(100, 112)
FEATURE_REMOTE = 1 << 0
FEATURE_UNINSTALL = 1 << 1
FEATURE_WATCH = 1 << 2
FEATURE_PORTS = 1 << 3
CLIENT_ASK = 1 << 0

# REMOTES: how the connection to another machine is
IDLE, CONNECTING, CONNECTED, RETRYING, STUCK, ASKING, INSTALLING, REMOVING, LOGIN = range(9)
STATES = ['idle', 'connecting', 'connected', 'retrying', 'stuck', 'asking', 'installing',
          'removing', 'login']

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
        # (A footd just started: its socket is there before it listens)
        deadline = time.time() + 2
        while True:
            try:
                self.sock.connect(path)
                break
            except ConnectionRefusedError:
                if time.time() > deadline:
                    raise
                time.sleep(.02)
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

    def hello(self, features=None):
        self.send(HELLO, u32(VERSION) + i32(os.getpid()) +
                  (u32(features) if features is not None else b''))
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

    def read_past(self, needle):
        """Until 'needle'; what came before it (and it) is dropped"""
        self.read_until(needle)
        self.output = self.output[self.output.index(needle) + len(needle):]

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


def create(client, id, token, argv, cols=80, rows=24, cwd='/', host=None, started=True):
    win = Window()
    payload = (u64(id) + u64(token) + size(cols, rows) + string(cwd) +
               string(CONFIG) + string('xterm-256color') + string('/bin/sh') + u8(0) +
               u32(1000) + strv(argv) + strv([f'{k}={v}' for k, v in ENV.items()]))
    if host is not None:
        payload += string(host)
    client.send(CREATE, payload, win.give())
    if not started:
        # (Not yet: the caller calls win.sent() once footd has it)
        return win, None
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


def watch_until(watcher, host, state, sessions=None):
    """REMOTES (to a client that WATCHes), until 'host' is in 'state': (state, sessions, detail)"""
    seen = []
    try:
        while True:
            r = watcher.expect(REMOTES)
            remotes = {}
            for _ in range(r.u32()):
                name = r.str()
                remotes[name] = (r.u8(), r.u32(), r.str())
            now = remotes.get(host)
            seen.append(now and (STATES[now[0]], now[1], now[2]))
            if now is not None and now[0] == state and sessions in (None, now[1]):
                return now
    except Failure as e:
        raise Failure(f'remote: {host} not {STATES[state]} ({sessions} sessions): '
                      f'{seen} ({e})')


def wait_exit(proc):
    try:
        return proc.wait(timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        raise Failure('footd did not exit')


def children(pid):
    out = subprocess.run(['pgrep', '-P', str(pid)], capture_output=True, text=True).stdout
    return [int(p) for p in out.split()]


def test_shell_integration(footd, tmp):
    """
    zsh, bash and fish read foot's integration: they mark their prompts
    (OSC 133), report the working directory (OSC 7), read the user's own
    startup files, and find the environment as it was
    """
    global CONFIG

    path = os.path.join(tmp, 'shells.sock')
    home = os.path.join(tmp, 'home')
    os.mkdir(home)

    # The user's startup files (HOME is the sessions' too)
    with open(os.path.join(home, '.zshenv'), 'w') as f:
        f.write('STARTUP=zshenv\n')
    with open(os.path.join(home, '.zshrc'), 'w') as f:
        f.write('STARTUP=$STARTUP-zshrc\nPS1="zsh> "\n')
    with open(os.path.join(home, '.bashrc'), 'w') as f:
        f.write('STARTUP=bashrc\nPS1="bash> "\n')

    # footd writes the scripts below its HOME
    proc = subprocess.Popen([footd, f'--socket={path}', '--log-to-stderr', '--log-level=warning'],
                            env=dict(os.environ, HOME=home))
    saved = CONFIG, dict(ENV)
    try:
        deadline = time.time() + TIMEOUT
        while not os.path.exists(path):
            if time.time() > deadline or proc.poll() is not None:
                raise Failure('footd did not start')
            time.sleep(.02)

        c = Client(path)
        c.hello()

        # Also Homebrew's shells: macOS has no fish, and its bash is left alone
        search = '/opt/homebrew/bin:/usr/local/bin:' + ENV['PATH']
        ENV.update(HOME=home, PATH=search, ZDOTDIR=home, ENV='/nonexistent/env')

        def run(id, shell, command, expect, leave=None):
            win, _ = create(c, id, id * 10, [shell], cwd=tmp)
            try:
                # The window is the terminal: fish 4 waits for its answer to
                # DA1, the last of its queries
                answered = 0

                def read_until(needle):
                    nonlocal answered
                    deadline = time.time() + TIMEOUT
                    while needle not in win.output:
                        queries = win.output.count(b'\x1b[0c')
                        if queries > answered:
                            win.write(b'\x1b[?62;22c' * (queries - answered))
                            answered = queries
                        if time.time() > deadline:
                            raise Failure(f'{shell}: {needle!r} not received; '
                                          f'got {win.output[-500:]!r}')
                        if select.select([win.ours], [], [], 0.5)[0]:
                            win.output += win.ours.recv(65536)

                read_until(b'\x1b]133;A')
                win.write(command.encode() + b'\r')
                read_until(expect.encode())
                read_until(b'\x1b]133;D;0')
                out = win.output
                for mark in (b'\x1b]133;C', b'\x1b]7;file://'):
                    if mark not in out:
                        raise Failure(f'{shell}: no {mark!r}: {out[-500:]!r}')
                if out.index(b'\x1b]133;C') > out.index(b'\x1b]133;D;0'):
                    raise Failure(f'{shell}: the output ends before it starts: {out[-500:]!r}')

                if leave is not None:
                    win.write(leave)
                else:
                    c.send(KILL, u64(id) + u64(id * 10))
                win.read_eof()
                return out
            finally:
                win.close()

        tested = []

        zsh = shutil.which('zsh', path=search)
        if zsh is not None:
            out = run(101, zsh, 'echo "<$STARTUP|$ZDOTDIR|${FOOT_ZSH_ZDOTDIR-none}>"',
                      f'<zshenv-zshrc|{home}|none>')
            if b'\x1b]7;file://' + socket.gethostname().encode() + tmp.encode() not in out:
                raise Failure(f'zsh: not the working directory: {out[-500:]!r}')
            tested.append('zsh')

        # (-i, as on Linux: without it, a Debian's bashrc returns right away)
        bash = shutil.which('bash', path=search)
        if bash is not None and not (sys.platform == 'darwin' and os.path.realpath(bash) == '/bin/bash'):
            run(102, bash,
                'echo "<$STARTUP|$ENV|${FOOT_BASH_ENV-none}${FOOT_BASH_INJECT-}|$(shopt -po posix)>"',
                '<bashrc|/nonexistent/env|none|set +o posix>',
                # (Not KILL: Homebrew's bash 5.3 stays at its prompt after a SIGHUP)
                leave=b'exit\r')
            tested.append('bash')

        fish = shutil.which('fish', path=search)
        if fish is not None:
            run(103, fish,
                'echo "<"(set -q XDG_DATA_DIRS; echo $status)"|$FOOT_FISH_XDG_DATA_DIR$FOOT_FISH_XDG_UNSET>"',
                '<1|>')
            ENV['XDG_DATA_DIRS'] = '/nonexistent/share:/usr/share'
            run(104, fish, 'echo "<$XDG_DATA_DIRS|$FOOT_FISH_XDG_DATA_DIR$FOOT_FISH_XDG_UNSET>"',
                '</nonexistent/share:/usr/share|>')
            del ENV['XDG_DATA_DIRS']
            tested.append('fish')

        # macOS' own bash doesn't read $ENV in POSIX mode: started as it is
        if sys.platform == 'darwin':
            win, _ = create(c, 105, 1050, ['/bin/bash'], cwd=tmp)
            win.read_until(b'bash> ')
            win.write(b'echo "<$STARTUP|$(shopt -po posix)>"\r')
            win.read_until(b'<bashrc|set +o posix>')
            c.send(KILL, u64(105) + u64(1050))
            win.read_eof()
            win.close()

        # macos-shell-integration=no: started as it is
        if zsh is not None:
            CONFIG = os.path.join(tmp, 'no-shell-integration.ini')
            with open(CONFIG, 'w') as f:
                f.write('macos-shell-integration=no\n')
            win, _ = create(c, 106, 1060, [zsh], cwd=tmp)
            win.read_until(b'zsh> ')
            win.write(b'echo "<$STARTUP|$ZDOTDIR>"\r')
            win.read_until(f'<zshenv-zshrc|{home}>'.encode())
            win.read_until(b'zsh> ')
            if b'\x1b]133;' in win.output:
                raise Failure(f'zsh, without the integration: {win.output[-500:]!r}')
            c.send(KILL, u64(106) + u64(1060))
            win.read_eof()
            win.close()

        c.close()
        if wait_exit(proc) != 0:
            raise Failure('footd failed')

        print(f'footd: shell integration: {", ".join(tested) or "no shell to test with"}')
    finally:
        CONFIG = saved[0]
        ENV.clear()
        ENV.update(saved[1])
        if proc.poll() is None:
            proc.kill()
            proc.wait()


def test_remote(footd, tmp):
    """Sessions on another machine: footd --bridge to a second footd, a script for ssh"""
    remote_path = os.path.join(tmp, 'r.sock')
    pw_remote_path = os.path.join(tmp, 'p.sock')
    ssh = os.path.join(tmp, 'ssh')
    newhost = os.path.join(tmp, 'newhost')
    os.mkdir(newhost)
    login = os.path.join(tmp, 'login')
    os.mkdir(login)
    others = [remote_path, os.path.join(newhost, 's.sock')] + \
        [f'{pw_remote_path}.{host}' for host in ['pwhost', 'pwhost2', 'pwhost3']]
    with open(ssh, 'w') as f:
        # ssh's arguments: OPTIONS -- HOST COMMAND. newhost: footd isn't
        # there (its "old" file: an old one is); what the commands do.
        # pwhost*: asks for a password ("secret"), unless it has a key
        # ($TEST_LOGIN/HOST.key); its token is in $TEST_LOGIN/HOST.token
        f.write('#!/bin/sh\n'
                'while [ "$1" != -- ]; do shift; done\n'
                'case $2 in\n'
                'testhost) exec "$TEST_FOOTD" --bridge --socket="$TEST_REMOTE" --log-to-stderr ;;\n'
                'newhost)\n'
                '    d=$TEST_NEWHOST\n'
                '    case $3 in\n'
                '    *footd.new*) cat >"$d/footd" && chmod 755 "$d/footd" && rm -f "$d/old" ;;\n'
                '    *pkill*) rm "$d/footd" && pkill -f "$d/s.sock"; exit 0 ;;\n'
                '    *)  m="footd-install: $(uname -s) $(uname -m)"\n'
                '        [ -e "$d/old" ] && { echo "$m /old/footd" >&2; exit 3; }\n'
                '        [ -x "$d/footd" ] || { echo "$m none" >&2; exit 3; }\n'
                '        exec "$d/footd" --bridge --socket="$d/s.sock" --log-to-stderr ;;\n'
                '    esac ;;\n'
                'pwhost*)\n'
                '    t=$TEST_LOGIN/$2.token\n'
                '    echo "$FOOTD_ASKPASS_TOKEN" >"$t.new" && mv "$t.new" "$t"\n'
                '    for i in 1 2 3; do\n'
                '        [ -e "$TEST_LOGIN/$2.key" ] && pw=secret ||\n'
                '            pw=$("$SSH_ASKPASS" "$2\'s password: ") || break\n'
                '        [ "$pw" = secret ] && exec "$TEST_FOOTD" --bridge --socket="$TEST_PWREMOTE.$2" --log-to-stderr\n'
                '    done\n'
                '    echo "Permission denied (password)." >&2; exit 255 ;;\n'
                '*) echo "no such host: $2" >&2; exit 255 ;;\n'
                'esac\n')
    os.chmod(ssh, 0o755)

    # A check (of a connection nothing came over for twice that long) times out after this
    check_timeout = .3

    path = os.path.join(tmp, 'l.sock')
    env = dict(os.environ, FOOTD_SSH=ssh, TEST_FOOTD=os.path.abspath(footd),
               TEST_REMOTE=remote_path, TEST_PWREMOTE=pw_remote_path, TEST_NEWHOST=newhost, TEST_LOGIN=login, REMOTE_MARK='there', HOME=tmp,
               FOOTD_CHECK_TIMEOUT=str(int(check_timeout * 1000)), FOOTD_LINGER='300', FOOTD_MIN_GAP='500')
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
        if not c.features & FEATURE_REMOTE or not c.features & FEATURE_UNINSTALL:
            raise Failure(f'no remote sessions (features: {c.features})')

        # Another client watches the connections (Shell > Connections): none yet
        watcher = Client(path)
        watcher.hello()
        if not watcher.features & FEATURE_WATCH:
            raise Failure(f'no WATCH (features: {watcher.features})')
        watcher.send(WATCH, u8(1))
        if watcher.expect(REMOTES).u32() != 0:
            raise Failure('remote: REMOTES before any')

        # There: the window's size; that machine's environment, not the window's
        global ENV
        ENV = dict(ENV, WINDOW_MARK='here')
        script = ('stty size; echo "w=${WINDOW_MARK:-} r=${REMOTE_MARK:-}"; echo ready; '
                  'read x; echo "got $x"; read y; echo bye')
        win, _ = create(c, 100, 1, ['/bin/sh', '-c', script], cols=90, rows=20, host='testhost')
        out = win.read_until(b'ready')
        watch_until(watcher, 'testhost', CONNECTED, 1)
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

        # The connection is lost: the window stays, is told, and it's connected again
        script = 'echo up; read x; echo "got $x"; read y; echo "got $y"'
        win, _ = create(c, 101, 3, ['/bin/sh', '-c', script], host='testhost')
        win.read_until(b'up')
        for pid in children(proc.pid):
            os.kill(pid, signal.SIGKILL)
        win.read_past(b'connection to testhost is lost')
        win.read_until(b'up')       # Its state, anew
        c.expect(STARTED)
        watch_until(watcher, 'testhost', RETRYING, 1)
        watch_until(watcher, 'testhost', CONNECTED, 1)
        win.write(b'one\n')
        win.read_until(b'got one')

        # Nothing comes (e.g. the Mac slept): input after a quiet while checks it, finds it lost
        stalled = children(proc.pid)
        for pid in stalled:
            os.kill(pid, signal.SIGSTOP)
        time.sleep(2 * check_timeout + .1)
        win.output = b''
        win.write(b'x')             # (Lost with the connection)
        win.read_past(b'connection to testhost is lost')
        win.read_until(b'got one')
        c.expect(STARTED)
        for pid in stalled:
            try:
                os.kill(pid, signal.SIGCONT)    # (It's been told to go)
            except ProcessLookupError:
                pass
        win.write(b'two\n')
        win.read_until(b'two')
        win.read_eof()

        # It can't connect: the window is told why, and it's created once it can
        win, _ = create(c, 103, 5, ['/bin/sh', '-c', 'echo up'], host='otherhost', started=False)
        win.read_until(b"can't connect to otherhost: no such host: otherhost; trying again")
        win.sent()
        detail = watch_until(watcher, 'otherhost', RETRYING, 1)[2]
        if detail != 'no such host: otherhost':
            raise Failure(f'remote: why otherhost can not be reached: {detail!r}')
        win.close()
        c.send(KILL, u64(103) + u64(5))
        time.sleep(.3)
        sessions, _ = list_sessions(c)
        if sessions:
            raise Failure(f'remote: one that could not be created is left: {sessions}')

        # footd isn't there: the window asks, and Enter installs it (this one: a machine like this)
        win, _ = create(c, 104, 6, ['/bin/sh', '-c', 'echo up; read x'], host='newhost', started=False)
        win.read_until(b"footd isn't on newhost")
        win.sent()
        detail = watch_until(watcher, 'newhost', ASKING, 1)[2]
        if detail != f'{os.uname().sysname} {os.uname().machine}':
            raise Failure(f'remote: newhost is {detail!r}')
        win.write(b'y')
        time.sleep(.3)
        if os.path.exists(os.path.join(newhost, 'footd')):
            raise Failure('remote: installed without Enter')
        win.write(b'\r')
        win.read_until(b'installing footd on newhost')
        if c.expect(STARTED).u64() != 104:
            raise Failure('remote: STARTED for another session')
        watch_until(watcher, 'newhost', INSTALLING)
        watch_until(watcher, 'newhost', CONNECTED, 1)
        win.read_until(b'up')
        with open(footd, 'rb') as a, open(os.path.join(newhost, 'footd'), 'rb') as b:
            if a.read() != b.read():
                raise Failure('remote: what was installed is not footd')

        # Removed: its sessions end, their windows close, and the window that asked is told
        c.send(UNINSTALL, string('newhost'))
        if c.expect(FAILED).u64() != 104:
            raise Failure('remote: FAILED for another session')
        r = c.expect(UNINSTALLED)
        host, error = r.str(), r.str()
        if host != 'newhost' or error:
            raise Failure(f'remote: UNINSTALLED {host!r} {error!r}')
        win.read_eof()
        if os.path.exists(os.path.join(newhost, 'footd')):
            raise Failure('remote: not removed')
        watch_until(watcher, 'newhost', REMOVING, 0)
        watch_until(watcher, 'newhost', IDLE, 0)

        # An old one there: replaced, without asking
        open(os.path.join(newhost, 'old'), 'w').close()
        win, _ = create(c, 105, 7, ['/bin/sh', '-c', 'echo up; read x'], host='newhost', started=False)
        win.read_until(b'updating footd on newhost')
        win.sent()
        if c.expect(STARTED).u64() != 105:
            raise Failure('remote: STARTED for another session')
        win.read_until(b'up')
        win.close()
        c.send(KILL, u64(105) + u64(7))

        # Logging in: ssh asks (for a password); a client that can (foot) is asked
        asker = Client(path)
        asker.hello(features=CLIENT_ASK)

        def asked(host, again=0):
            r = asker.expect(ASK)
            id, got = r.u64(), (r.str(), r.str(), r.u8(), r.u8())
            if got != (host, f"{host}'s password: ", 0, again):
                raise Failure(f'remote: asked {got}')
            return id

        def answer(id, text, ok=1):
            asker.send(ANSWER, u64(id) + u8(ok) + string(text))

        win, _ = create(c, 110, 10, ['/bin/sh', '-c', 'echo in; read x'], host='pwhost',
                        started=False)
        id = asked('pwhost')
        win.sent()
        watch_until(watcher, 'pwhost', LOGIN, 1)
        answer(id, 'wrong')
        answer(asked('pwhost', again=1), 'secret')     # (Asked again: it was wrong)
        if c.expect(STARTED).u64() != 110:
            raise Failure('remote: STARTED for another session')
        win.read_until(b'in')

        # Lost: connected again with that password, without asking
        def kill_ssh(host):
            for pid in children(proc.pid):
                args = subprocess.run(['ps', '-o', 'args=', '-p', str(pid)],
                                      capture_output=True, text=True).stdout
                if f' {host} ' in args or f'p.sock.{host} ' in args:
                    os.kill(pid, signal.SIGKILL)

        kill_ssh('pwhost')
        win.read_past(b'connection to pwhost is lost')
        win.read_until(b'in')
        c.expect(STARTED)
        if select.select([asker.sock], [], [], .3)[0]:
            raise Failure('remote: asked again after reconnecting')

        # Another program asks as ssh's helper would, with the token of an
        # ssh that hasn't asked (it has a key now): it isn't told the password
        token_path = os.path.join(login, 'pwhost.token')
        os.remove(token_path)
        open(os.path.join(login, 'pwhost.key'), 'w').close()
        kill_ssh('pwhost')
        win.read_past(b'connection to pwhost is lost')
        win.read_until(b'in')
        c.expect(STARTED)
        with open(token_path) as f:
            token = int(f.read(), 16)
        other = Client(path)
        other.hello()
        other.send(ASKPASS, u64(token) + string("pwhost's password: ") + u8(0))
        if other.expect(ANSWERED).u8() != 0:
            raise Failure('remote: another program was told the password')
        other.close()
        if select.select([asker.sock], [], [], .3)[0]:
            raise Failure('remote: asked for another program')
        if token < 1 << 32:
            raise Failure(f"remote: ssh's token can be guessed ({token})")

        # Canceled: not tried again before the window's input
        win2, _ = create(c, 111, 11, ['/bin/sh', '-c', 'echo two; read x'], host='pwhost2',
                         started=False)
        answer(asked('pwhost2'), '', ok=0)
        win2.read_until(b'logging in to pwhost2 was canceled; type to try again')
        win2.sent()
        watch_until(watcher, 'pwhost2', STUCK, 1)
        win2.write(b'x')
        answer(asked('pwhost2'), 'secret')
        c.expect(STARTED)
        win2.read_until(b'two')

        # ssh gives up while it asks: the window isn't asked any more (ASKED), then anew
        win3, _ = create(c, 112, 12, ['/bin/sh', '-c', 'echo three; read x'], host='pwhost3',
                         started=False)
        id = asked('pwhost3')
        win3.sent()
        kill_ssh('pwhost3')
        if asker.expect(ASKED).u64() != id:
            raise Failure('remote: ASKED another')
        answer(asked('pwhost3'), 'secret')
        c.expect(STARTED)
        win3.read_until(b'three')

        for w, id, token in [(win, 110, 10), (win2, 111, 11), (win3, 112, 12)]:
            w.close()
            c.send(KILL, u64(id) + u64(token))
        asker.close()

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

        # No window needs the connection: it's hung up (after a while), the other footd exits
        watch_until(watcher, 'testhost', IDLE, 0)
        deadline = time.time() + TIMEOUT
        while os.path.exists(remote_path):
            if time.time() > deadline:
                raise Failure('remote: not hung up')
            time.sleep(.05)

        # A window there connects again
        win, _ = create(c, 106, 8, ['/bin/sh', '-c', 'echo again; read x'], host='testhost')
        win.read_until(b'again')
        watch_until(watcher, 'testhost', CONNECTED, 1)
        win.close()
        c.send(KILL, u64(106) + u64(8))

        # Without sessions and windows, both footds exit (the remote one: its bridge gone)
        watcher.send(WATCH, u8(0))
        watcher.close()
        c.close()
        if wait_exit(proc) != 0:
            raise Failure('remote: footd failed')
        deadline = time.time() + TIMEOUT
        while any(os.path.exists(other) for other in others):
            if time.time() > deadline:
                left = [os.path.relpath(other, tmp) for other in others if os.path.exists(other)]
                raise Failure(f'remote: the other footds did not exit: {left}')
            time.sleep(.05)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()

        # Those left (it failed) are ended too (SIGTERM: their sessions with them)
        if any(os.path.exists(other) for other in others):
            subprocess.run(['pkill', '-f', '--', f'--socket={tmp}/'])


class PortHandler(socketserver.BaseRequestHandler):
    """
    A server "there". Its first line says what it does: "echo" (what it
    gets, until that ends), "digest" (of what it gets, once that has
    ended: the answer to one who has closed its side), "blob N" (N bytes,
    unasked)
    """

    def handle(self):
        sock = self.request
        line = b''
        while not line.endswith(b'\n'):
            byte = sock.recv(1)
            if not byte:
                return
            line += byte
        command = line.split()

        if command[0] == b'echo':
            while True:
                data = sock.recv(65536)
                if not data:
                    return
                sock.sendall(data)
        elif command[0] == b'digest':
            digest, count = hashlib.sha256(), 0
            while True:
                data = sock.recv(65536)
                if not data:
                    break
                digest.update(data)
                count += len(data)
            sock.sendall(f'{digest.hexdigest()} {count}\n'.encode())
        elif command[0] == b'blob':
            sock.sendall(blob(int(command[1])))


class PortServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def blob(n):
    return (bytes(range(256)) * (n // 256 + 1))[:n]


def port_server():
    server = PortServer(('127.0.0.1', 0), PortHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, server.server_address[1]


def free_port():
    """One nothing listens on (for now)"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def port_connect(port, family=socket.AF_INET):
    s = socket.socket(family, socket.SOCK_STREAM)
    s.settimeout(TIMEOUT)
    s.connect(('127.0.0.1' if family == socket.AF_INET else '::1', port))
    return s


def read_all(sock):
    data = b''
    while True:
        try:
            more = sock.recv(65536)
        except socket.timeout:
            raise Failure(f'ports: no end after {len(data)} bytes')
        if not more:
            return data
        data += more


def read_exact(sock, n):
    data = b''
    while len(data) < n:
        try:
            more = sock.recv(n - len(data))
        except socket.timeout:
            raise Failure(f'ports: {len(data)} of {n} bytes')
        if not more:
            raise Failure(f'ports: closed after {len(data)} of {n} bytes')
        data += more
    return data


def port_refuses(port):
    try:
        port_connect(port).close()
        return False
    except ConnectionRefusedError:
        return True


def forward(client, request, host, port, local=0):
    """FORWARD: the port here, and why not"""
    client.send(FORWARD, u64(request) + string(host) + u32(port) + u32(local))
    r = client.expect(FORWARDED)
    if r.u64() != request:
        raise Failure('ports: FORWARDED for another request')
    return r.u32(), r.str()


def read_remotes(r):
    remotes = {}
    for _ in range(r.u32()):
        name = r.str()
        remotes[name] = (r.u8(), r.u32(), r.str())
    return remotes, [(r.str(), r.u32(), r.u32()) for _ in range(r.u32())]


def watch_ports_until(watcher, expected):
    """REMOTES (to a client that WATCHes), until the open ports are 'expected': (host, port, port here)"""
    seen = []
    try:
        while True:
            _, ports = read_remotes(watcher.expect(REMOTES))
            seen.append(ports)
            if ports == expected:
                return
    except Failure as e:
        raise Failure(f'ports: not {expected}: {seen} ({e})')


def test_ports(footd, tmp):
    """Another machine's ports, as ports of this one: a server "there" is reached here"""
    path = os.path.join(tmp, 'pl.sock')
    remote_path = os.path.join(tmp, 'pr.sock')
    ssh = os.path.join(tmp, 'ssh-ports')
    with open(ssh, 'w') as f:
        f.write('#!/bin/sh\n'
                'while [ "$1" != -- ]; do shift; done\n'
                'exec "$TEST_FOOTD" --bridge --socket="$TEST_REMOTE" --log-to-stderr\n')
    os.chmod(ssh, 0o755)

    linger = .3
    env = dict(os.environ, FOOTD_SSH=ssh, TEST_FOOTD=os.path.abspath(footd),
               TEST_REMOTE=remote_path, HOME=tmp, FOOTD_LINGER=str(int(linger * 1000)),
               FOOTD_MIN_GAP='500')
    proc = subprocess.Popen([footd, f'--socket={path}', '--log-to-stderr', '--log-level=warning'],
                            env=env)
    server, port = port_server()
    server2, port2 = port_server()
    try:
        deadline = time.time() + TIMEOUT
        while not os.path.exists(path):
            if time.time() > deadline or proc.poll() is not None:
                raise Failure('footd did not start')
            time.sleep(.02)

        c = Client(path)
        c.hello()
        if not c.features & FEATURE_PORTS:
            raise Failure(f'no ports (features: {c.features})')

        # Not of a machine there's no connection to
        here, error = forward(c, 1, 'testhost', port)
        if here != 0 or not error:
            raise Failure(f'ports: without a connection: {here}, {error!r}')

        win, _ = create(c, 200, 1, ['/bin/sh', '-c', 'echo up; read x'], host='testhost')
        win.read_until(b'up')
        watcher = Client(path)
        watcher.hello()
        watcher.send(WATCH, u8(1))
        watch_ports_until(watcher, [])

        # The same port, if it's free: here it isn't (the server "there" has it), so another
        here, error = forward(c, 2, 'testhost', port)
        if error or here in (0, port):
            raise Failure(f'ports: the port here: {here}, {error!r}')
        watch_ports_until(watcher, [('testhost', port, here)])

        s = port_connect(here)
        s.sendall(b'echo\nhello')
        if read_exact(s, 5) != b'hello':
            raise Failure('ports: echo')
        s.close()

        # As localhost, whichever that is
        try:
            s = port_connect(here, socket.AF_INET6)
        except OSError:
            s = None    # (No IPv6 here)
        if s is not None:
            s.sendall(b'echo\nsix')
            if read_exact(s, 3) != b'six':
                raise Failure('ports: echo over ::1')
            s.close()

        # One who has said all it has (its side is closed) still gets the answer
        data = os.urandom(3 * 1024 * 1024 + 17)
        s = port_connect(here)
        s.sendall(b'digest\n' + data)
        s.shutdown(socket.SHUT_WR)
        answer = read_all(s)
        s.close()
        if answer != f'{hashlib.sha256(data).hexdigest()} {len(data)}\n'.encode():
            raise Failure(f'ports: after closing one side: {answer!r}')

        # A lot from there, and its end
        s = port_connect(here)
        s.sendall(b'blob 5000003\n')
        if read_all(s) != blob(5000003):
            raise Failure('ports: a lot from there')
        s.close()

        # A lot both ways at once
        s = port_connect(here)
        s.sendall(b'echo\n')

        def send():
            s.sendall(data)
            s.shutdown(socket.SHUT_WR)
        sender = threading.Thread(target=send, daemon=True)
        sender.start()
        if read_all(s) != data:
            raise Failure('ports: a lot both ways')
        sender.join()
        s.close()

        # Open already: the same one
        again, error = forward(c, 3, 'testhost', port)
        if (again, error) != (here, ''):
            raise Failure(f'ports: opened again: {again}, {error!r}')

        # On a port asked for, if that's free
        wanted = free_port()
        here2, error = forward(c, 4, 'testhost', port2, wanted)
        if (here2, error) != (wanted, ''):
            raise Failure(f'ports: the port asked for ({wanted}): {here2}, {error!r}')
        watch_ports_until(watcher, [('testhost', port, here), ('testhost', port2, here2)])
        s = port_connect(here2)
        s.sendall(b'echo\ntwo')
        if read_exact(s, 3) != b'two':
            raise Failure('ports: the second one')
        s.close()

        # Nothing listens there: not opened, and why
        nothing = free_port()
        none, error = forward(c, 5, 'testhost', nothing)
        if none != 0 or 'nothing is listening' not in error:
            raise Failure(f'ports: nothing there: {none}, {error!r}')

        # Which ports there are, there: the server's, and whose it is
        c.send(PORTS, u64(6) + string('testhost'))
        r = c.expect(LISTENING)
        if r.u64() != 6 or r.str() != '':
            raise Failure('ports: LISTENING')
        listening = {}
        for _ in range(r.u32()):
            number = r.u32()
            listening[number] = (r.str(), r.u8())
        if port not in listening or port2 not in listening:
            raise Failure(f'ports: {port} and {port2} are not listed: {listening}')
        program, own = listening[port]
        if not program or not own:
            raise Failure(f'ports: whose {port} is: {program!r}, {own}')
        if nothing in listening:
            raise Failure(f'ports: {nothing} is listed, which nothing listens on')

        # Closed
        c.send(UNFORWARD, string('testhost') + u32(port2))
        watch_ports_until(watcher, [('testhost', port, here)])
        if not port_refuses(here2):
            raise Failure('ports: a closed one accepts')

        # The connection is lost: what was connected ends; connected again, the port is there
        s = port_connect(here)
        s.sendall(b'echo\nx')
        read_exact(s, 1)
        for pid in children(proc.pid):
            os.kill(pid, signal.SIGKILL)
        try:
            if s.recv(1) != b'':
                raise Failure('ports: a connection outlived its machine\'s')
        except ConnectionError:
            pass
        s.close()
        win.read_past(b'connection to testhost is lost')
        win.read_until(b'up')
        c.expect(STARTED)
        deadline = time.time() + TIMEOUT
        while True:
            try:
                s = port_connect(here)
                s.sendall(b'echo\nback')
                if s.recv(4) == b'back':
                    break
            except OSError:
                pass
            s.close()
            if time.time() > deadline:
                raise Failure('ports: not there after connecting again')
            time.sleep(.05)

        # No window needs the machine any more, but a connection to its port does
        win.close()
        c.send(KILL, u64(200) + u64(1))
        time.sleep(3 * linger)
        s.sendall(b'still')
        if read_exact(s, 5) != b'still':
            raise Failure('ports: a connection without windows')
        s.close()

        # Nothing does: it's hung up, and its ports are closed
        watch_ports_until(watcher, [])
        deadline = time.time() + TIMEOUT
        while not port_refuses(here) or os.path.exists(remote_path):
            if time.time() > deadline:
                raise Failure('ports: not closed with the connection')
            time.sleep(.05)

        watcher.send(WATCH, u8(0))
        watcher.close()
        c.close()
        if wait_exit(proc) != 0:
            raise Failure('ports: footd failed')
    finally:
        server.shutdown()
        server.server_close()
        server2.shutdown()
        server2.server_close()
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        if os.path.exists(remote_path):
            subprocess.run(['pkill', '-f', '--', f'--socket={remote_path}'])


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

        test_shell_integration(footd, tmp)
        test_remote(footd, tmp)
        test_ports(footd, tmp)

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
