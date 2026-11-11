#!/usr/bin/env python3
#
# Smoke test: runs foot for real, in real windows (they open on screen,
# briefly): the platform layer in macos/ is what upstream's tests can't
# reach. Together with a sanitizer build (make sanitize), this is what
# finds memory errors in it.
#
# Usage: smoke-test.py <directory with foot and footclient> [tests...]

import importlib.util
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TIMEOUT = 180  # Seconds, for each step (the sanitizers on CI take a minute)

# Sanitizer findings fail the test, not just print something
ENV = dict(
    os.environ,
    ASAN_OPTIONS='halt_on_error=1:detect_leaks=0',
    UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1:suppressions='
                  + os.path.join(ROOT, 'macos', 'ubsan.supp'),
)


class Failure(Exception):
    pass


def run(*args, expect=0, env=None):
    proc = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=TIMEOUT,
                          env=env if env is not None else ENV)
    if proc.returncode != expect:
        raise Failure(f'{args}: exit code {proc.returncode}, expected {expect}\n{proc.stdout}')
    return proc.stdout


def wait_for(path):
    deadline = time.time() + TIMEOUT
    while not os.path.exists(path):
        if time.time() > deadline:
            raise Failure(f'{path}: not created')
        time.sleep(.05)


class Tests:
    def __init__(self, bin_dir, tmp):
        self.foot = os.path.join(bin_dir, 'foot')
        self.footclient = os.path.join(bin_dir, 'footclient')
        self.tmp = tmp

        self.config = os.path.join(tmp, 'empty.ini')
        open(self.config, 'w').close()

        self.tbench = os.path.join(tmp, 'tbench')
        subprocess.run(['xcrun', 'cc', '-O2', '-o', self.tbench,
                        os.path.join(ROOT, 'macos', 'bench', 'tbench.c')], check=True)

        # Runs menu actions in foot, for test_splits
        self.driver = os.path.join(tmp, 'smoke-driver.dylib')
        subprocess.run(['xcrun', 'cc', '-dynamiclib', '-fobjc-arc', '-framework', 'AppKit',
                        '-o', self.driver, os.path.join(ROOT, 'macos', 'smoke-driver.m')],
                       check=True)

    def foot_args(self, *args):
        return [self.foot, f'--config={self.config}', *args]

    def test_unit_tests(self):
        """Debug builds run foot's unit tests at startup"""
        run(self.foot, '--version')

    def test_exit_code(self):
        """A window opens, runs the command, and foot exits with its exit code"""
        run(*self.foot_args('sh', '-c', 'exit 7'), expect=7)

    def test_output(self):
        """The workloads are processed: the terminal answers the query after each"""
        # All of them take too long with sanitizers in a large window
        for size, only in (('80x24', None), ('200x60', 'ascii')):
            out = os.path.join(self.tmp, f'tbench-{size}.txt')
            env = dict(ENV, TBENCH_ONLY=only) if only is not None else ENV
            run(*self.foot_args(f'--window-size-chars={size}', self.tbench, 'run', out, '1'), env=env)
            with open(out) as f:
                lines = f.read().splitlines()
            if lines[-1] != 'done' or len(lines) != (3 if only is not None else 8):
                raise Failure(f'{size}: unexpected results: {lines}')

    def test_input(self):
        """Control sequences a shell's programs use: terminal size, colors, a title"""
        out = os.path.join(self.tmp, 'stty.txt')
        script = f"""
            # The window's size arrives right after the command started
            for i in $(seq 100); do
                test "$(stty size)" = "30 100" && break
                sleep .1
            done
            stty size > {out}.tmp
            printf '\\033]0;a title\\007\\033[1;31mred\\033[0m\\033]8;;https://example.com\\033\\\\link\\033]8;;\\033\\\\\\n'
            printf '\\033[?1049h\\033[2J\\033[?1049l'
            # Restoring the cursor restores the underline's style too, and deleted
            # characters take theirs with them (foot's own bugs, fixed in the fork:
            # macos/UPSTREAM.md); drawn before foot exits
            printf '\\033[4;58:2::255:0:0m\\033[s\\033[24m\\033[uunderlined\\033[0m\\n'
            printf '\\033[58;5;3mA\\033[4mU\\033[0m\\r\\033[P\\n'
            sleep .5
            mv {out}.tmp {out}
        """
        run(*self.foot_args('--window-size-chars=100x30', 'sh', '-c', script))
        with open(out) as f:
            if f.read().split() != ['30', '100']:
                raise Failure('stty size: not the window size')

    class Panes:
        """foot, driven by macos/smoke-driver.m; each pane's shell reports its size"""

        def __init__(self, tests, name):
            self.tmp = tmp = os.path.join(tests.tmp, name)
            os.mkdir(tmp)
            self.tests = tests
            self.foot = None

            # Until the test is done
            self.script = os.path.join(tmp, 'pane.sh')
            with open(self.script, 'w') as f:
                f.write(f"""
                    last=
                    while [ ! -e {tmp}/quit ]; do
                        size=$(stty size)
                        if [ "$size" != "$last" ]; then
                            echo "$size" > {tmp}/size.$$.tmp
                            mv {tmp}/size.$$.tmp {tmp}/size.$$
                            last=$size
                        fi
                        sleep .1
                    done
                """)

            self.config = os.path.join(tmp, 'foot.ini')
            self.configure()

        def configure(self, options=''):
            """foot.ini: the panes' shell, and 'options' (lines)"""
            with open(self.config, 'w') as f:
                f.write(f'shell=/bin/sh {self.script}\nlogin-shell=no\n'
                        f'macos-confirm-close=no\n{options}')

        def start(self, *args):
            env = dict(ENV, DYLD_INSERT_LIBRARIES=self.tests.driver, FOOT_SMOKE_DIR=self.tmp)
            self.foot = subprocess.Popen(
                [self.tests.foot, f'--config={self.config}', '--window-size-chars=100x30', *args],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, env=env)

        def sizes(self):
            result = {}
            for name in os.listdir(self.tmp):
                if name.startswith('size.') and not name.endswith('.tmp'):
                    with open(os.path.join(self.tmp, name)) as f:
                        rows, cols = f.read().split()
                    result[name[5:]] = (int(rows), int(cols))
            return result

        def wait(self, what, check):
            deadline = time.time() + TIMEOUT
            while True:
                got = self.sizes()
                if check(got):
                    return got
                if time.time() > deadline or self.foot.poll() is not None:
                    raise Failure(f'{what}: the panes\' sizes are {got}')
                time.sleep(.1)

        def perform(self, action):
            with open(os.path.join(self.tmp, 'action'), 'w') as f:
                f.write(action)

        def quit(self):
            """The shells exit, and foot with its last window; returns its output"""
            open(os.path.join(self.tmp, 'quit'), 'w').close()
            try:
                out, _ = self.foot.communicate(timeout=TIMEOUT)
            except subprocess.TimeoutExpired:
                self.foot.kill()
                out, _ = self.foot.communicate()
            return out

    def test_reload(self):
        """Reload Configuration: the panes take the file's padding and fonts, and keep the command line's overrides"""
        t = self.Panes(self, 'reload')
        t.configure('pad=0x0\n')

        # Keeping its grid (the default), the window would grow with the
        # font instead: as far as the screen allows
        t.start('--override=resize-keep-grid=no')
        out = ''

        try:
            got = t.wait('the window', lambda got: list(got.values()) == [(30, 100)])
            a = next(iter(got))

            # The window keeps its size: less of it is the grid's
            t.configure('pad=40x40\n')
            t.perform('reloadConfiguration:')
            t.wait('padding', lambda got: got[a][0] < 30 and got[a][1] < 100)

            t.configure('pad=0x0\n')
            t.perform('reloadConfiguration:')
            t.wait('no padding', lambda got: got[a] == (30, 100))

            # Twice the font size (and another number of fonts): half the grid
            t.configure('pad=0x0\nfont=monospace:size=26, Menlo:size=26\n')
            t.perform('reloadConfiguration:')
            t.wait('a larger font', lambda got: 10 <= got[a][0] <= 17 and 40 <= got[a][1] <= 55)

            # All panes: also one that loaded the file itself
            t.perform('splitRight:')
            got = t.wait('split', lambda got: len(got) == 2 and all(rows <= 17 for rows, _ in got.values()))
            b = next(pane for pane in got if pane != a)

            t.configure('pad=0x0\n')
            t.perform('reloadConfiguration:')
            t.wait('the default font', lambda got: got[a][0] >= 27 and got[b][0] >= 27)
        finally:
            out = t.quit()

        if t.foot.returncode != 0:
            raise Failure(f'foot: exit code {t.foot.returncode}\n{out}')

    def test_selection(self):
        """A selection dragged beyond the window's edges, and back: the terminal goes on"""
        t = self.Panes(self, 'selection')
        answers = os.path.join(t.tmp, 'answers')
        dragged = os.path.join(t.tmp, 'dragged')

        # Something to scroll; then the terminal is asked for the cursor's
        # position, over and over (it only answers while foot reads)
        with open(t.script, 'w') as f:
            f.write(f"""
                seq 1 200
                n=0
                while [ ! -e {t.tmp}/quit ]; do
                    printf '\\033[6n'
                    if read -r -s -t 5 -d R answer; then
                        n=$((n + 1))
                        echo $n > {answers}.tmp && mv {answers}.tmp {answers}
                    fi
                    sleep .1
                done
            """)

        def count():
            try:
                with open(answers) as f:
                    return int(f.read())
            except FileNotFoundError:
                return 0

        def wait(what, check):
            deadline = time.time() + TIMEOUT
            while not check():
                if time.time() > deadline or t.foot.poll() is not None:
                    raise Failure(what)
                time.sleep(.1)

        t.start()
        out = ''

        try:
            wait('the terminal doesn\'t answer', lambda: count() > 0)
            t.perform('dragSelection')
            wait('the selection wasn\'t dragged', lambda: os.path.exists(dragged))

            # (Not TIMEOUT: it's what a frozen terminal looks like)
            before = count()
            deadline = time.time() + 30
            while count() <= before + 1:
                if time.time() > deadline:
                    raise Failure('the terminal doesn\'t answer any more, after the drag')
                time.sleep(.1)
        finally:
            out = t.quit()

        if t.foot.returncode != 0:
            raise Failure(f'foot: exit code {t.foot.returncode}\n{out}')

    def test_splits(self):
        """Splits: right and down, zoomed, closed; each pane's terminal size"""
        t = self.Panes(self, 'splits')
        wait, perform = t.wait, t.perform

        def new_pane(got, known):
            panes = [pane for pane in got if pane not in known]
            return panes[0] if len(panes) == 1 else None

        t.start()
        out = ''

        try:
            got = wait('the window', lambda got: list(got.values()) == [(30, 100)])
            a = next(iter(got))

            # Side by side: halves of the width
            perform('splitRight:')
            got = wait('split right', lambda got: (
                (b := new_pane(got, [a])) is not None and
                got[a][0] == got[b][0] == 30 and abs(got[a][1] - got[b][1]) <= 1 and
                90 <= got[a][1] + got[b][1] < 100))
            b = new_pane(got, [a])
            a_size, b_size = got[a], got[b]

            # The new pane has the focus: it's split, stacked
            perform('splitDown:')
            got = wait('split down', lambda got: (
                (c := new_pane(got, [a, b])) is not None and got[a] == a_size and
                got[b][1] == got[c][1] == b_size[1] and abs(got[b][0] - got[c][0]) <= 1 and
                25 <= got[b][0] + got[c][0] < 30))
            c = new_pane(got, [a, b])

            # Zoomed: the whole window; the other panes keep their size
            perform('toggleSplitZoom:')
            got = wait('zoom', lambda got: got[c] == (30, 100) and got[a] == a_size)
            perform('toggleSplitZoom:')
            wait('zoom out', lambda got: got[c][1] == b_size[1] and got[c][0] < 30)

            # Closing the new pane: the one it split gets its space back
            perform('performClose:')
            wait('close', lambda got: got[b] == b_size and got[a] == a_size)
        finally:
            out = t.quit()

        if t.foot.returncode != 0:
            raise Failure(f'foot: exit code {t.foot.returncode}\n{out}')

    class Sessions:
        """foot with sessions (in a footd of its own), driven by macos/smoke-driver.m"""

        def __init__(self, tests, name):
            self.tmp = tmp = os.path.join(tests.tmp, name)
            os.mkdir(tmp)
            self.sock = os.path.join(tmp, 's.sock')
            self.foot = tests.foot

            # footd's protocol, as macos/test-footd.py speaks it (no __pycache__ in macos/)
            sys.dont_write_bytecode = True
            spec = importlib.util.spec_from_file_location(
                'test_footd', os.path.join(ROOT, 'macos', 'test-footd.py'))
            self.footd = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(self.footd)

            # Each shell reports its PID and size, until the test is done
            script = os.path.join(tmp, 'shell.sh')
            with open(script, 'w') as f:
                f.write(f"""
                    echo $$ > {tmp}/pid.$$.tmp && mv {tmp}/pid.$$.tmp {tmp}/pid.$$
                    last=
                    while [ ! -e {tmp}/quit ]; do
                        size=$(stty size)
                        if [ "$size" != "$last" ]; then
                            echo "$size" > {tmp}/size.$$.tmp
                            mv {tmp}/size.$$.tmp {tmp}/size.$$
                            last=$size
                        fi
                        sleep .1
                    done
                """)

            self.config = os.path.join(tmp, 'foot.ini')
            with open(self.config, 'w') as f:
                f.write(f'shell=/bin/sh {script}\nlogin-shell=no\nmacos-confirm-close=no\n')

            self.env = dict(ENV, FOOT_SESSIONS_SOCKET=self.sock, DYLD_INSERT_LIBRARIES=tests.driver,
                            FOOT_SMOKE_DIR=tmp)

        def start(self):
            return subprocess.Popen(
                [self.foot, f'--config={self.config}', '--window-size-chars=100x30'],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, env=self.env)

        def shells(self):
            return sorted(int(name[4:]) for name in os.listdir(self.tmp)
                          if name.startswith('pid.') and not name.endswith('.tmp'))

        def size(self, pid):
            with open(os.path.join(self.tmp, f'size.{pid}')) as f:
                rows, cols = f.read().split()
            return int(rows), int(cols)

        @staticmethod
        def alive(pid):
            try:
                os.kill(pid, 0)
                return True
            except ProcessLookupError:
                return False

        def footd_pid(self, shell):
            parent = int(run('ps', '-o', 'ppid=', '-p', str(shell)).strip())
            if 'footd' not in run('ps', '-o', 'comm=', '-p', str(parent)):
                raise Failure('the shell is not footd\'s')
            return parent

        def sessions(self):
            client = self.footd.Client(self.sock)
            client.hello()
            got, _ = self.footd.list_sessions(client)
            client.close()
            return got

        @staticmethod
        def wait(what, check, proc=None):
            deadline = time.time() + TIMEOUT
            while not check():
                if time.time() > deadline or (proc is not None and proc.poll() is not None):
                    raise Failure(f'{what}: not so')
                time.sleep(.1)

        def perform(self, action):
            with open(os.path.join(self.tmp, 'action'), 'w') as f:
                f.write(action)

        def windows(self, proc):
            path = os.path.join(self.tmp, 'windows')
            if os.path.exists(path):
                os.unlink(path)
            self.perform('windows')
            self.wait('the windows', lambda: os.path.exists(path), proc)
            with open(path) as f:
                return sorted((w['tabs'], w['panes']) for w in json.load(f))

        @staticmethod
        def finish(proc, what, check=True):
            try:
                out, _ = proc.communicate(timeout=TIMEOUT)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, _ = proc.communicate()
                raise Failure(f'{what}: foot did not exit\n{out}')
            if check and proc.returncode != 0:
                raise Failure(f'{what}: exit code {proc.returncode}\n{out}')

        def done(self, foot):
            open(os.path.join(self.tmp, 'quit'), 'w').close()
            if foot is not None and foot.poll() is None:
                foot.kill()
                foot.communicate()

    def test_sessions(self):
        """Sessions: in footd; kept, with their splits, tabs and quake terminal, when foot quits; ended with their panes"""
        t = self.Sessions(self, 'sessions')

        foot = t.start()
        try:
            # The shell is footd's (its parent), and has the window's size
            t.wait('a shell', lambda: len(t.shells()) == 1 and os.path.exists(f'{t.tmp}/size.{t.shells()[0]}'), foot)
            a = t.shells()[0]
            t.footd_pid(a)
            if t.size(a) != (30, 100):
                raise Failure(f'size: {t.size(a)}')

            t.perform('splitRight:')
            t.wait('a split', lambda: len(t.shells()) == 2, foot)
            b = [pid for pid in t.shells() if pid != a][0]
            t.wait('the split\'s size', lambda: os.path.exists(f'{t.tmp}/size.{b}') and t.size(a)[1] < 100, foot)

            # A tab (the tab bar takes some rows), and the quake terminal
            t.perform('newWindowForTab:')
            t.wait('a tab', lambda: len(t.shells()) == 3, foot)
            t.wait('the tab bar', lambda: t.size(a)[0] < 30 and t.size(b)[0] < 30, foot)
            sizes = {a: t.size(a), b: t.size(b)}
            t.perform('toggleQuakeTerminal:')
            t.wait('the quake terminal', lambda: len(t.shells()) == 4, foot)
            all_shells = t.shells()
            layout = t.windows(foot)
            if layout != [(2, 1), (2, 2)]:
                raise Failure(f'windows (tabs, panes): {layout}')

            # Quitting (Cmd+Q) only detaches: the shells keep running
            t.perform('terminate:')
            t.finish(foot, 'quitting')
            if not all(t.alive(pid) for pid in all_shells):
                raise Failure('a shell ended when foot quit')
            if [s['attached'] for s in t.sessions()] != [False] * 4:
                raise Failure(f'after quitting: {t.sessions()}')

            # Started again: attached again, as they were, nothing new started
            foot = t.start()
            t.wait('attached again', lambda: sorted(s['attached'] for s in t.sessions()) == [False, True, True, True], foot)
            time.sleep(1)
            if t.windows(foot) != layout:
                raise Failure(f'windows (tabs, panes): {t.windows(foot)}, were {layout}')
            if {a: t.size(a), b: t.size(b)} != sizes:
                raise Failure(f'sizes {sizes} became {({a: t.size(a), b: t.size(b)})}')

            # The quake terminal's, once it's shown
            t.perform('toggleQuakeTerminal:')
            t.wait('the quake terminal attached', lambda: all(s['attached'] for s in t.sessions()), foot)
            if t.shells() != all_shells:
                raise Failure(f'new shells: {t.shells()}')

            # Closing a pane ends its session; the last ones, the windows (and foot)
            t.perform('performClose:')
            t.wait('a session ended', lambda: len(t.sessions()) == 3, foot)
            t.perform('performCloseWindow:')
            t.wait('the window\'s sessions ended', lambda: len(t.sessions()) == 1, foot)
            t.perform('toggleQuakeTerminal:')
            t.perform('performClose:')
            t.finish(foot, 'closing the windows')
            t.wait('footd exiting', lambda: not os.path.exists(t.sock))
            if any(t.alive(pid) for pid in all_shells):
                raise Failure('a shell is still running')
        finally:
            t.done(foot)

    def test_sessions_ending(self):
        """Sessions: Quit and End Sessions ends those no window shows too; a busy footd is kept, a crashed one replaced"""
        t = self.Sessions(self, 'sessions-ending')

        foot = t.start()
        try:
            t.wait('a shell', lambda: len(t.shells()) == 1, foot)
            t.perform('toggleQuakeTerminal:')
            t.wait('the quake terminal', lambda: len(t.shells()) == 2, foot)
            first = t.shells()
            t.perform('terminate:')
            t.finish(foot, 'quitting')

            # The quake terminal's session is attached once it's shown: not yet
            foot = t.start()
            t.wait('attached again', lambda: sorted(s['attached'] for s in t.sessions()) == [False, True], foot)
            t.perform('quitEndingSessions:')
            t.finish(foot, 'quitting, ending the sessions')
            t.wait('footd exiting', lambda: not os.path.exists(t.sock))
            t.wait('the shells ending', lambda: not any(t.alive(pid) for pid in first))

            foot = t.start()
            t.wait('a shell', lambda: len(t.shells()) == 3, foot)
            shell = [pid for pid in t.shells() if pid not in first][0]
            footd_pid = t.footd_pid(shell)

            # footd is busy (its backlog is full): another foot doesn't replace it, and
            # gets a PTY of its own
            inode = os.stat(t.sock).st_ino
            os.kill(footd_pid, signal.SIGSTOP)
            backlog = []
            other = None
            try:
                while True:
                    conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    try:
                        conn.connect(t.sock)
                    except ConnectionRefusedError:
                        conn.close()
                        break
                    backlog.append(conn)
                    if len(backlog) > 1000:
                        raise Failure('footd\'s backlog is never full')

                other = t.start()
                t.wait('the other foot\'s shell', lambda: len(t.shells()) == 4, other)
                own = [pid for pid in t.shells() if pid not in first + [shell]][0]
                parent = int(run('ps', '-o', 'ppid=', '-p', str(own)).strip())
                if parent != other.pid:
                    raise Failure(f'the other foot\'s shell is {parent}\'s')
                other.send_signal(signal.SIGTERM)
                t.finish(other, 'the other foot', check=False)
            finally:
                os.kill(footd_pid, signal.SIGCONT)
                for conn in backlog:
                    conn.close()
                if other is not None and other.poll() is None:
                    other.kill()
                    other.communicate()

            # (Until footd has caught up with its backlog, it refuses connections)
            def accepting():
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
                    try:
                        conn.connect(t.sock)
                        return True
                    except ConnectionRefusedError:
                        return False
            t.wait('footd accepting again', accepting, foot)
            if os.stat(t.sock).st_ino != inode or [s['pid'] for s in t.sessions()] != [shell]:
                raise Failure(f'footd was replaced: {t.sessions()}')

            # footd crashes: its windows end, and the next foot starts another one
            # (the socket it left behind refuses connections)
            os.kill(footd_pid, signal.SIGKILL)
            t.finish(foot, 'footd crashed', check=False)
            if not os.path.exists(t.sock):
                raise Failure('footd took its socket with it')
            known = t.shells()
            foot = t.start()
            t.wait('a shell', lambda: len(t.shells()) == len(known) + 1, foot)
            new = [pid for pid in t.shells() if pid not in known][0]
            if t.footd_pid(new) == footd_pid:
                raise Failure('the same footd')
            t.perform('performClose:')
            t.finish(foot, 'closing the window')
            t.wait('footd exiting', lambda: not os.path.exists(t.sock))
        finally:
            t.done(foot)

    def test_server(self):
        """foot --server, footclient, and the quake terminal"""
        env = dict(ENV, XDG_RUNTIME_DIR=self.tmp)
        server = subprocess.Popen(
            self.foot_args('--server'), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, env=env)

        try:
            wait_for(os.path.join(self.tmp, 'foot.sock'))

            marker = os.path.join(self.tmp, 'footclient-marker')
            run(self.footclient, 'sh', '-c', f'touch {marker}', env=env)
            wait_for(marker)

            for action, state in (('status', 'hidden'), ('hide', 'hidden'), ('show', 'visible'),
                                  ('status', 'visible'), ('hide', 'hidden')):
                got = run(self.footclient, f'--quake={action}', env=env).strip().splitlines()[-1]
                if got != state:
                    raise Failure(f'footclient --quake={action}: {got}, expected {state}')
        finally:
            server.send_signal(signal.SIGTERM)
            out, _ = server.communicate(timeout=TIMEOUT)

        if server.returncode != 0:
            raise Failure(f'foot --server: exit code {server.returncode}\n{out}')


def main():
    if len(sys.argv) < 2:
        sys.exit('usage: smoke-test.py <directory with foot and footclient> [test names...]')
    only = [f'test_{name}' for name in sys.argv[2:]]

    # Short: it's where foot's server socket goes
    tmp = tempfile.mkdtemp(prefix='foot-', dir='/tmp')
    failed = 0

    try:
        tests = Tests(os.path.abspath(sys.argv[1]), tmp)
        for name in sorted(dir(tests)):
            if not name.startswith('test_') or (only and name not in only):
                continue

            test = getattr(tests, name)
            try:
                test()
                print(f'ok    {test.__doc__}', flush=True)
            except (Failure, subprocess.TimeoutExpired) as e:
                failed += 1
                print(f'FAIL  {test.__doc__}\n{e}', flush=True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
