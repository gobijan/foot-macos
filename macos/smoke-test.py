#!/usr/bin/env python3
#
# Smoke test: runs foot for real, in real windows (they open on screen,
# briefly): the platform layer in macos/ is what upstream's tests can't
# reach. Together with a sanitizer build (make sanitize), this is what
# finds memory errors in it.
#
# Usage: smoke-test.py <directory with foot and footclient>

import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TIMEOUT = 60  # Seconds, for each step

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
    if len(sys.argv) != 2:
        sys.exit('usage: smoke-test.py <directory with foot and footclient>')

    # Short: it's where foot's server socket goes
    tmp = tempfile.mkdtemp(prefix='foot-', dir='/tmp')
    failed = 0

    try:
        tests = Tests(os.path.abspath(sys.argv[1]), tmp)
        for name in sorted(dir(tests)):
            if not name.startswith('test_'):
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
