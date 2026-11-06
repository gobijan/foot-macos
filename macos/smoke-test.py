#!/usr/bin/env python3
#
# Smoke test: runs foot for real, in real windows (they open on screen,
# briefly): the platform layer in macos/ is what upstream's tests can't
# reach. Together with a sanitizer build (make sanitize), this is what
# finds memory errors in it.
#
# Usage: smoke-test.py <directory with foot and footclient> [tests...]

import importlib.util
import os
import shutil
import signal
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
            mv {out}.tmp {out}
        """
        run(*self.foot_args('--window-size-chars=100x30', 'sh', '-c', script))
        with open(out) as f:
            if f.read().split() != ['30', '100']:
                raise Failure('stty size: not the window size')

    def test_splits(self):
        """Splits: right and down, zoomed, closed; each pane's terminal size"""
        tmp = os.path.join(self.tmp, 'splits')
        os.mkdir(tmp)

        # Each pane's shell reports its size, until the test is done
        script = os.path.join(tmp, 'pane.sh')
        with open(script, 'w') as f:
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

        config = os.path.join(tmp, 'foot.ini')
        with open(config, 'w') as f:
            f.write(f'shell=/bin/sh {script}\nlogin-shell=no\nmacos-confirm-close=no\n')

        def sizes():
            result = {}
            for name in os.listdir(tmp):
                if name.startswith('size.') and not name.endswith('.tmp'):
                    with open(os.path.join(tmp, name)) as f:
                        rows, cols = f.read().split()
                    result[name[5:]] = (int(rows), int(cols))
            return result

        def wait(what, check):
            deadline = time.time() + TIMEOUT
            while True:
                got = sizes()
                if check(got):
                    return got
                if time.time() > deadline or foot.poll() is not None:
                    raise Failure(f'{what}: the panes\' sizes are {got}')
                time.sleep(.1)

        def perform(action):
            with open(os.path.join(tmp, 'action'), 'w') as f:
                f.write(action)

        def new_pane(got, known):
            panes = [pane for pane in got if pane not in known]
            return panes[0] if len(panes) == 1 else None

        env = dict(ENV, DYLD_INSERT_LIBRARIES=self.driver, FOOT_SMOKE_DIR=tmp)
        foot = subprocess.Popen(
            [self.foot, f'--config={config}', '--window-size-chars=100x30'],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, env=env)

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
            open(os.path.join(tmp, 'quit'), 'w').close()
            try:
                out, _ = foot.communicate(timeout=TIMEOUT)
            except subprocess.TimeoutExpired:
                foot.kill()
                out, _ = foot.communicate()

        if foot.returncode != 0:
            raise Failure(f'foot: exit code {foot.returncode}\n{out}')

    def test_sessions(self):
        """Sessions: in footd; kept, with their splits, when foot quits; ended with their panes"""
        tmp = os.path.join(self.tmp, 'sessions')
        os.mkdir(tmp)
        sock = os.path.join(tmp, 's.sock')

        # footd's protocol, as macos/test-footd.py speaks it (no __pycache__ in macos/)
        sys.dont_write_bytecode = True
        spec = importlib.util.spec_from_file_location(
            'test_footd', os.path.join(ROOT, 'macos', 'test-footd.py'))
        footd = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(footd)

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

        config = os.path.join(tmp, 'foot.ini')
        with open(config, 'w') as f:
            f.write(f'shell=/bin/sh {script}\nlogin-shell=no\nmacos-confirm-close=no\n')

        env = dict(ENV, FOOT_SESSIONS_SOCKET=sock, DYLD_INSERT_LIBRARIES=self.driver,
                   FOOT_SMOKE_DIR=tmp)

        def start():
            return subprocess.Popen(
                [self.foot, f'--config={config}', '--window-size-chars=100x30'],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, env=env)

        def shells():
            return sorted(int(name[4:]) for name in os.listdir(tmp)
                          if name.startswith('pid.') and not name.endswith('.tmp'))

        def size(pid):
            with open(os.path.join(tmp, f'size.{pid}')) as f:
                rows, cols = f.read().split()
            return int(rows), int(cols)

        def alive(pid):
            try:
                os.kill(pid, 0)
                return True
            except ProcessLookupError:
                return False

        def sessions():
            client = footd.Client(sock)
            client.hello()
            got, _ = footd.list_sessions(client)
            client.close()
            return got

        def wait(what, check, proc=None):
            deadline = time.time() + TIMEOUT
            while not check():
                if time.time() > deadline or (proc is not None and proc.poll() is not None):
                    raise Failure(f'{what}: not so')
                time.sleep(.1)

        def perform(action):
            with open(os.path.join(tmp, 'action'), 'w') as f:
                f.write(action)

        def finish(proc, what):
            try:
                out, _ = proc.communicate(timeout=TIMEOUT)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, _ = proc.communicate()
                raise Failure(f'{what}: foot did not exit\n{out}')
            if proc.returncode != 0:
                raise Failure(f'{what}: exit code {proc.returncode}\n{out}')

        foot = start()
        try:
            # The shell is footd's (its parent), and has the window's size
            wait('a shell', lambda: len(shells()) == 1 and os.path.exists(f'{tmp}/size.{shells()[0]}'), foot)
            a = shells()[0]
            parent = int(run('ps', '-o', 'ppid=', '-p', str(a)).strip())
            if 'footd' not in run('ps', '-o', 'comm=', '-p', str(parent)):
                raise Failure('the shell is not footd\'s')
            if size(a) != (30, 100):
                raise Failure(f'size: {size(a)}')

            perform('splitRight:')
            wait('a split', lambda: len(shells()) == 2, foot)
            b = [pid for pid in shells() if pid != a][0]
            wait('the split\'s size', lambda: os.path.exists(f'{tmp}/size.{b}') and size(a)[1] < 100, foot)
            sizes = {a: size(a), b: size(b)}

            # Quitting (Cmd+Q) only detaches: the shells keep running
            perform('terminate:')
            finish(foot, 'quitting')
            if not (alive(a) and alive(b)):
                raise Failure('a shell ended when foot quit')
            if sorted(s['attached'] for s in sessions()) != [False, False]:
                raise Failure(f'after quitting: {sessions()}')

            # Started again: attached again, split as they were, nothing new started
            foot = start()
            wait('attached again', lambda: [s['attached'] for s in sessions()] == [True, True], foot)
            time.sleep(1)
            if shells() != sorted([a, b]):
                raise Failure(f'new shells: {shells()}')
            if {a: size(a), b: size(b)} != sizes:
                raise Failure(f'sizes {sizes} became {({a: size(a), b: size(b)})}')

            # Closing a pane ends its session; the last one, the window (and foot)
            perform('performClose:')
            wait('a session ended', lambda: len(sessions()) == 1 and alive(a) != alive(b), foot)
            perform('performClose:')
            finish(foot, 'closing the window')
            wait('footd exiting', lambda: not os.path.exists(sock))
            if alive(a) or alive(b):
                raise Failure('a shell is still running')
        finally:
            open(os.path.join(tmp, 'quit'), 'w').close()
            if foot.poll() is None:
                foot.kill()
                foot.communicate()

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
