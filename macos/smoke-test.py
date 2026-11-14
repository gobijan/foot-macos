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

    def test_control(self):
        """foot @: the terminals are listed, typed into, read and split from outside"""
        tmp = os.path.join(self.tmp, 'control')
        os.mkdir(tmp)
        config = os.path.join(tmp, 'foot.ini')
        # A pager that keeps what it's shown, and where that was
        pager = os.path.join(tmp, 'pager.sh')
        paged = os.path.join(tmp, 'paged')
        with open(pager, 'w') as f:
            f.write(f'#!/bin/sh\ncat "$1" > {paged}.tmp\necho "$1" > {paged}.path\n'
                    f'mv {paged}.tmp {paged}\n')
        os.chmod(pager, 0o755)

        with open(config, 'w') as f:
            f.write(f'shell=/bin/sh\nlogin-shell=no\nmacos-confirm-close=no\nmacos-pager={pager}\n')

        env = dict(ENV, FOOT_CONTROL_SOCKET=os.path.join(tmp, 'c.sock'))
        script = 'printf "\\033]2;a title\\007"; echo ready; read x; echo "got $x"; sleep 100'
        foot = subprocess.Popen(
            [self.foot, f'--config={config}', '--window-size-chars=100x30', 'sh', '-c', script],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, env=env)

        def at(*args, expect=0):
            return run(self.foot, '@', *args, expect=expect, env=env)

        def wait(what, check):
            deadline = time.time() + TIMEOUT
            while not check():
                if time.time() > deadline or foot.poll() is not None:
                    raise Failure(f'{what}: not reached')
                time.sleep(.1)

        try:
            wait_for(env['FOOT_CONTROL_SOCKET'])
            wait('the program', lambda: 'ready' in at('get-text'))

            terminals = json.loads(at('ls'))
            if len(terminals) != 1:
                raise Failure(f'ls: {terminals}')
            first = terminals[0]
            if (first['title'], first['columns'], first['rows'], first['focused']) != \
                    ('a title', 100, 30, True):
                raise Failure(f'ls: {first}')

            # Typed, with Return; and what the terminal shows then
            at('send-text', 'hello', 'there\\r')
            wait('the input', lambda: 'got hello there' in at('get-text', '--scrollback'))

            # From stdin, as it is (the terminal echoes it)
            subprocess.run([self.foot, '@', 'send-text', '--stdin'], input=b'as\\tit is',
                           env=env, check=True, timeout=TIMEOUT)
            wait('the input from stdin', lambda: 'as\\tit is' in at('get-text'))

            # The scrollback, in the pager: a window of its own, until it exits;
            # the text's file is gone then
            at('run', 'Show Scrollback in Pager')
            wait('the pager', lambda: os.path.exists(paged))
            with open(paged) as f:
                text = f.read()
            if 'ready' not in text or 'got hello there' not in text:
                raise Failure(f'the pager was shown: {text!r}')
            wait('the pager\'s end', lambda: len(json.loads(at('ls'))) == 1)
            with open(paged + '.path') as f:
                path = f.read().strip()
            if not os.path.basename(path).startswith('foot-scrollback-') or os.path.exists(path):
                raise Failure(f'the pager\'s file: {path}')

            # Hints: the words on the screen get labels (the mode ends with
            # Escape, or the terminal)
            if 'Hints: Copy Word\n' not in at('commands') or \
                    'Open URL or Path\t' not in at('commands'):
                raise Failure(f'commands: {at("commands")}')
            at('run', 'Hints: Copy Word')

            # The menus' commands, for a terminal: a split, which has the focus then
            if 'Split Right\t' not in at('commands'):
                raise Failure(f'commands: {at("commands")}')
            at('run', 'split', 'right')
            wait('the split', lambda: len(json.loads(at('ls'))) == 2)
            second = [t for t in json.loads(at('ls')) if t['id'] != first['id']][0]
            if not second['focused'] or second['window'] != first['window']:
                raise Failure(f'ls, after the split: {second}')

            # A title of its own, whatever the program set; and that one again
            at('set-title', f'--id={first["id"]}', 'my', 'title')
            if [t['title'] for t in json.loads(at('ls'))][0] != 'my title':
                raise Failure(f'set-title: {at("ls")}')
            at('set-title', f'--id={first["id"]}')
            if [t['title'] for t in json.loads(at('ls'))][0] != 'a title':
                raise Failure(f'set-title, without one: {at("ls")}')

            # Input to all splits: switched on, and off again (typing needs
            # the keyboard focus, which a test doesn't take)
            for state in ('on', 'off'):
                at('run', 'Send Input to All Splits')

            # By its ID: the first one still is what it was, and closes
            if 'got hello there' not in at('get-text', f'--id={first["id"]}'):
                raise Failure('get-text --id: not the first terminal')
            at('run', '--id', str(first['id']), 'Close Split')
            wait('the close', lambda: [t['id'] for t in json.loads(at('ls'))] == [second['id']])

            # launch: a command in a split of that one, in the caller's directory
            # and with its environment; it prints the new terminal's ID
            where = os.path.realpath(tmp)
            out = subprocess.run(
                [self.foot, '@', 'launch', '--type=split', '--direction=down', '--title=launched',
                 'sh', '-c', 'echo "in $PWD with $FOOT_TEST_VAR"; sleep 100'],
                cwd=where, env=dict(env, FOOT_TEST_VAR='a value'), check=True, timeout=TIMEOUT,
                stdout=subprocess.PIPE, text=True).stdout
            launched = int(out)
            wait('the launched command',
                 lambda: f'in {where} with a value' in at('get-text', f'--id={launched}'))
            entry = [t for t in json.loads(at('ls')) if t['id'] == launched][0]
            if (entry['title'], entry['window']) != ('launched', second['window']):
                raise Failure(f'ls, after launch: {entry}')

            # In a tab (a window of its own, to macOS), with another directory;
            # and the shell, in a window
            tab = int(at('launch', '--type=tab', '--cwd=/usr', 'sh', '-c', 'pwd; sleep 100'))
            wait('the tab', lambda: '/usr' in at('get-text', f'--id={tab}'))
            shell = int(at('launch'))
            if len({launched, tab, shell, second['id']}) != 4 or len(json.loads(at('ls'))) != 4:
                raise Failure(f'launch: {at("ls")}')

            # --match: the terminals with such a title (all of them, for
            # set-title), directory, program
            if f'in {where}' not in at('get-text', '--match', 'title:^launch'):
                raise Failure('--match title: not the launched terminal')
            at('set-title', '--match=title:^launched$', 'renamed')
            at('set-title', f'--match=id:{tab}', 'renamed')
            renamed = sorted(t['id'] for t in json.loads(at('ls')) if t['title'] == 'renamed')
            if renamed != sorted([launched, tab]):
                raise Failure(f'--match: renamed {renamed}')
            at('send-text', '--match', 'title:renamed', 'q')
            # (macOS' sh runs bash)
            program = [t['program'] for t in json.loads(at('ls')) if t['id'] == second['id']][0]
            if program not in ('sh', 'bash') or \
                    str(second['id']) not in at('ls', '--match', f'program:^{program}$'):
                raise Failure(f'ls: the program: {at("ls")}')
            # close: those that are meant
            at('close', '--force', '--match=title:^renamed$')
            wait('the close of two', lambda: sorted(t['id'] for t in json.loads(at('ls'))) ==
                 sorted([second['id'], shell]))
            at('close', f'--id={shell}')
            wait('the close', lambda: [t['id'] for t in json.loads(at('ls'))] == [second['id']])

            for args in (['get-text', '--match=title:no such'], ['get-text', '--match=color:red'],
                         ['get-text', '--match=title:('], ['launch', '--type=cube']):
                at(*args, expect=1)

            for args in (['run', 'no such command'], ['get-text', '--id=99'], ['dance']):
                at(*args, expect=1)
        finally:
            foot.terminate()
            out = foot.communicate()[0]

        if 'ERROR' in out:
            raise Failure(f'foot:\n{out}')

    def test_settings(self):
        """foot > Settings: an item writes its option to foot.ini, and the open window takes it"""
        tmp = os.path.join(self.tmp, 'settings')
        os.mkdir(tmp)
        config = os.path.join(tmp, 'foot.ini')
        theme = os.path.join(tmp, 'theme.ini')
        with open(theme, 'w') as f:
            f.write('[cursor]\nblink=yes\n')

        before = (
            '# -*- conf -*-\n'
            '\n'
            'macos-quake-position=left  # above the include\n'
            f'include={theme}\n'
            'shell=/bin/sh\n'
            'login-shell=no\n'
            'macos-confirm-close=no\n'
            '# font=monospace:size=8\n'
            '  pad=0x0 center   # none\n'
            '\n'
            '[cursor]\n'
            '# style=block\n'
            '# blink=no\n'
            '\n'
            '[mouse]\n'
            'hide-when-typing=no\n'
            'alternate-scroll-mode=yes')  # (No newline at its end)
        with open(config, 'w') as f:
            f.write(before)

        env = dict(ENV, FOOT_CONTROL_SOCKET=os.path.join(tmp, 'c.sock'))
        foot = subprocess.Popen(
            [self.foot, f'--config={config}', '--window-size-chars=100x30',
             '--override=resize-keep-grid=no', 'sh', '-c', 'echo ready; sleep 1000'],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, env=env)

        def at(*args, expect=0):
            return run(self.foot, '@', *args, expect=expect, env=env)

        def wait(what, check):
            deadline = time.time() + TIMEOUT
            while not check():
                if foot.poll() is not None:
                    raise Failure(f'{what}: foot exited ({foot.returncode})')
                if time.time() > deadline:
                    raise Failure(f'{what}: not reached ({at("ls")})')
                time.sleep(.1)

        def size():
            t = json.loads(at('ls'))[0]
            return t['rows'], t['columns']

        def lines():
            with open(config) as f:
                return f.read().split('\n')

        def checked():
            return [line.split('\t')[0] for line in at('commands').split('\n')
                    if line.endswith('\t✓') and line.startswith('Settings: ')]

        def expect(what, *titles):
            got = checked()
            for title in titles:
                if f'Settings: {title}' not in got:
                    raise Failure(f'{what}: {title} has no check mark: {got}')

        failed = None
        try:
            wait_for(env['FOOT_CONTROL_SOCKET'])
            wait('the program', lambda: 'ready' in at('get-text'))
            if size() != (30, 100):
                raise Failure(f'the window: {size()}')

            # What the file says, the included one too, and the defaults
            expect('at first', 'Font: Default (SF Mono)', 'Font Size: 13 pt', 'Padding: None',
                   'Cursor: Block', 'Cursor: Blink', 'Opacity: 100%', 'Bell: Sound',
                   'Option as Alt: By Keyboard Layout', 'Scrollback (New Windows): 1,000 Lines',
                   'Notify When a Command Finishes: Never', 'Quake Terminal: Left',
                   'Font Smoothing: 100%', 'Shell Integration (New Windows)')
            if 'Settings: Ask Before Closing a Running Program' in checked():
                raise Failure(f'macos-confirm-close=no has a check mark: {checked()}')

            # Below the commands: the list starts with what the other menus have
            titles = [line.split('\t')[0] for line in at('commands').split('\n') if line]
            settings = [title.startswith('Settings: ') for title in titles]
            if settings[0] or not settings[-1] or sorted(settings) != settings or \
                    titles.index('Edit foot.ini…') > titles.index('New Window'):
                raise Failure(f'the settings aren\'t the last commands: {titles}')

            # The assignment there is: replaced, as it's written (and the window
            # keeps its size: less of it is the grid's)
            at('run', 'Settings: Padding: 32 pt')
            wait('padding', lambda: size()[0] < 30 and size()[1] < 100)
            if '  pad=32x32 center   # none' not in lines():
                raise Failure(f'pad: {lines()}')
            expect('padding', 'Padding: 32 pt')
            at('run', 'Settings: Padding: None')
            wait('no padding', lambda: size() == (30, 100))

            # None: below the documented default; all the rest is as it was
            at('run', 'Settings: Font Size: 28 pt')
            wait('a larger font', lambda: 10 <= size()[0] <= 17)
            got = lines()
            at_font = got.index('# font=monospace:size=8')
            if got[at_font + 1] != 'font=monospace:size=28' or \
                    got[:at_font + 1] + got[at_font + 2:] != before.split('\n') + ['']:
                raise Failure(f'font: {got}')
            expect('the font size', 'Font Size: 28 pt')

            # Another font, in that size; then the size the window has
            at('run', 'Settings: Font: Menlo')
            wait('Menlo', lambda: 'font=Menlo:size=28' in lines())
            expect('the font', 'Font: Menlo', 'Font Size: 28 pt')
            columns = size()[1]
            for i in range(4):
                at('run', 'Smaller')
            wait('a smaller font', lambda: size()[1] > columns)
            at('run', "Settings: Font Size: This Window's")
            fonts = [line for line in lines() if line.startswith('font=')]
            if len(fonts) != 1 or not fonts[0].startswith('font=Menlo:size=') or \
                    not 13 < float(fonts[0].split('=')[-1]) < 28:
                raise Failure(f'this window\'s font size: {fonts}')

            # Below the documented default too, and that's after what the
            # included file says; a switch is off again when it's chosen once more
            at('run', 'Settings: Cursor: Beam')
            at('run', 'Settings: Cursor: Blink')
            got = lines()
            if got[got.index('# style=block') + 1] != 'style=beam' or \
                    got[got.index('# blink=no') + 1] != 'blink=no':
                raise Failure(f'cursor: {got}')
            expect('the cursor', 'Cursor: Beam')
            if 'Settings: Cursor: Blink' in checked():
                raise Failure('blink=no has a check mark')
            at('run', 'Settings: Cursor: Blink')
            if 'blink=yes' not in lines() or 'blink=no' in lines():
                raise Failure(f'blink, again: {lines()}')

            # Replaced; the file's last line had no end, and has one now
            at('run', 'Settings: Hide the Mouse Pointer While Typing')
            got = lines()
            if 'hide-when-typing=yes' not in got or got[-2:] != ['alternate-scroll-mode=yes', '']:
                raise Failure(f'mouse: {got}')

            # Sections that aren't there: added; several options at once
            at('run', 'Settings: Opacity: 90%')
            at('run', 'Settings: Bell: Flash')
            got = lines()
            if got[-10:] != ['', '[colors-dark]', 'alpha=0.9', '', '[colors-light]', 'alpha=0.9',
                            '', '[bell]', 'visual=yes', '']:
                raise Failure(f'new sections: {got}')
            expect('new sections', 'Opacity: 90%', 'Bell: Flash', 'Bell: Sound')

            # In [main], though the file's last section is another: after its
            # last assignment
            at('run', 'Settings: Font Smoothing: 50%')
            at('run', 'Settings: Option as Alt: Both Option Keys')
            at('run', 'Settings: Notify When a Command Finishes: After 10 Seconds')
            got = lines()
            start = got.index('  pad=0x0 center   # none')
            if got[start + 1:start + 6] != [
                    'macos-font-smoothing=yes', 'macos-font-smoothing-strength=0.5',
                    'macos-option-as-alt=both', 'macos-notify-on-command-finish=10', '']:
                raise Failure(f'[main]: {got}')
            expect('[main]', 'Font Smoothing: 50%', 'Option as Alt: Both Option Keys',
                   'Notify When a Command Finishes: After 10 Seconds')

            # An assignment above an include=: it goes below it (the included
            # file's would count else), with its comment
            expect('above the include', 'Quake Terminal: Left')
            at('run', 'Settings: Quake Terminal: Bottom')
            got = lines()
            start = got.index('  pad=0x0 center   # none')
            if got[start + 5] != "macos-quake-position=bottom  # above the include" or \
                    len([line for line in got if line.startswith('macos-quake-position')]) != 1:
                raise Failure(f'above the include: {got}')
            expect('below the include', 'Quake Terminal: Bottom')
        except Failure as failure:
            failed = failure
        finally:
            foot.terminate()
            out = foot.communicate()[0]

        if failed is not None or 'ERROR' in out:
            raise Failure(f'{failed or "errors"}\nfoot:\n{out}')

    def test_command_finish(self):
        """A command that took long is notified of, with its exit status; a short one isn't"""
        tmp = os.path.join(self.tmp, 'command-finish')
        os.mkdir(tmp)
        out = os.path.join(tmp, 'notified')
        config = os.path.join(tmp, 'foot.ini')
        with open(config, 'w') as f:
            f.write('macos-notify-on-command-finish=1\n'
                    '[desktop-notifications]\n'
                    'inhibit-when-focused=no\n'
                    f'command=sh -c \'printf "%s|%s|" "$1" "$2" >> {out}\' sh ${{title}} ${{body}}\n')

        # What a shell with the integration prints (OSC 133;C and D): a short
        # command, then a long one
        script = f"""
            # Once foot reads what's printed (it answers): it measures from
            # when it reads a command's start, and a first start can take long
            stty -icanon -echo min 1 time 0
            printf '\\033[6n'; dd bs=1 count=1 > /dev/null 2>&1
            printf '\\033]2;a title\\007'
            printf '\\033]133;C\\007'; printf '\\033]133;D;0\\007'
            printf '\\033]133;C\\007'; sleep 1.2; printf '\\033]133;D;3\\007'
            for i in $(seq 100); do test -s {out} && break; sleep .1; done
        """
        run(self.foot, f'--config={config}', 'sh', '-c', script)
        with open(out) as f:
            got = f.read()
        if got != 'Command failed|a title\nExit status 3, after 1s|':
            raise Failure(f'notified: {got!r}')

    def test_shell_integration(self):
        """zsh reads foot's integration, and the user's startup files after it"""
        home = os.path.join(self.tmp, 'shell-home')
        os.mkdir(home)
        out = os.path.join(home, 'started')
        with open(os.path.join(home, '.zshrc'), 'w') as f:
            f.write(f'print -r -- "${{precmd_functions[*]}}|${{ZDOTDIR-none}}" > {out}\nexit 0\n')

        # (The scripts are written below HOME)
        env = dict(ENV, HOME=home)
        env.pop('ZDOTDIR', None)
        run(*self.foot_args('zsh'), env=env)
        with open(out) as f:
            got = f.read().strip()
        if got != '_foot_precmd|none':
            raise Failure(f'with the integration: {got!r}')

        run(*self.foot_args('-o', 'macos-shell-integration=no', 'zsh'), env=env)
        with open(out) as f:
            got = f.read().strip()
        if got != '|none':
            raise Failure(f'without the integration: {got!r}')

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
            # (At once: the driver may read the file at any time)
            path = os.path.join(self.tmp, 'action')
            with open(path + '.tmp', 'w') as f:
                f.write(action)
            os.rename(path + '.tmp', path)

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
            t.configure('pad=0x0\nfont=monospace:size=28, Menlo:size=26\n')
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

    def test_cursor_click(self):
        """Option and a click at the prompt: the shell gets the arrow keys to there"""
        t = self.Panes(self, 'cursor-click')
        keys = os.path.join(t.tmp, 'keys')
        ready = os.path.join(t.tmp, 'ready')

        # A prompt as the shell integration marks it, with 60 characters
        # typed: the cursor is in column 62 (of 100). What's typed then is kept.
        with open(t.script, 'w') as f:
            f.write(f"""
                stty -icanon -echo min 1 time 0
                printf '\\033]133;A\\007$ %060d' 0
                # (All of the answer, ESC [ 1 ; 6 3 R, 7 bytes: foot reads, and shows it)
                printf '\\033[6n'; dd bs=1 count=7 > /dev/null 2>&1
                touch {ready}
                dd bs=1024 count=1 2> /dev/null > {keys}.tmp < /dev/tty
                mv {keys}.tmp {keys}
                while [ ! -e {t.tmp}/quit ]; do sleep .1; done
            """)

        def wait(what, path):
            deadline = time.time() + TIMEOUT
            while not os.path.exists(path):
                if time.time() > deadline or t.foot.poll() is not None:
                    raise Failure(what)
                time.sleep(.1)

        t.start()
        try:
            wait('the prompt isn\'t there', ready)
            time.sleep(1)
            t.perform('optionClick 25')
            wait('the shell got no keys', keys)
            with open(keys, 'rb') as f:
                got = f.read()

            # To column 25, or one next to it: the padding isn't part of the width
            count = len(got) // 3
            if got != b'\x1b[D' * count or not 35 <= count <= 39:
                raise Failure(f'the keys: {got!r}')
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

    def test_palette(self):
        """The command palette: a command found by typing, and run"""
        t = self.Panes(self, 'palette')
        t.start()
        out = ''

        try:
            t.wait('the window', lambda got: len(got) == 1)

            # The first one starting with what's typed (others: "New Window on: …")
            t.perform('palette new window')
            t.wait('a new window', lambda got: len(got) == 2)

            # A setting, by some of its words: written to foot.ini
            t.perform('palette cursor beam')
            deadline = time.time() + TIMEOUT
            while True:
                with open(t.config) as f:
                    if '[cursor]\nstyle=beam\n' in f.read():
                        break
                if time.time() > deadline or t.foot.poll() is not None:
                    raise Failure('the palette\'s setting: not written')
                time.sleep(.1)
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
                            FOOT_SMOKE_DIR=tmp, FOOT_CONTROL_SOCKET=os.path.join(tmp, 'c.sock'))

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
            # (At once: the driver may read the file at any time)
            path = os.path.join(self.tmp, 'action')
            with open(path + '.tmp', 'w') as f:
                f.write(action)
            os.rename(path + '.tmp', path)

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

            # foot @ launch: a session too, with the caller's directory and
            # environment; gone with its command
            launched = os.path.join(t.tmp, 'launched')
            subprocess.run(
                [self.foot, '@', 'launch', '--type=split', 'sh', '-c',
                 f'echo "$FOOT_TEST_VAR in $PWD" > {launched}.tmp; mv {launched}.tmp {launched}'],
                cwd='/usr', env=dict(t.env, FOOT_TEST_VAR='a value'), check=True, timeout=TIMEOUT,
                stdout=subprocess.DEVNULL)
            t.wait('the launched command', lambda: os.path.exists(launched), foot)
            with open(launched) as f:
                if f.read() != 'a value in /usr\n':
                    raise Failure('launch: not with the caller\'s directory and environment')
            t.wait('the launched session\'s end', lambda: len(t.sessions()) == 1, foot)

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

            # (Until footd has caught up with its backlog, it refuses connections:
            # also after one that it accepted)
            sessions = []

            def answering():
                try:
                    sessions[:] = t.sessions()
                    return True
                except ConnectionRefusedError:
                    return False
            t.wait('footd accepting again', answering, foot)
            if os.stat(t.sock).st_ino != inode or [s['pid'] for s in sessions] != [shell]:
                raise Failure(f'footd was replaced: {sessions}')

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
