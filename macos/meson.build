# The macOS port of foot.
#
# Included from the top-level meson.build, which is otherwise left as
# upstream has it (to keep upstream merges trivial). Sources are
# referenced relative to the project root.

add_languages('objc', native: false)

root = meson.project_source_root()

is_debug_build = get_option('buildtype').startswith('debug')

cc = meson.get_compiler('c')

# Newer clang versions warns when using __COUNTER__ without -std=c2y
if cc.has_argument('-Wc2y-extensions')
  add_project_arguments('-Wno-c2y-extensions', language: ['c', 'objc'])
endif

# Applies to subprojects too; all dependencies are linked statically
macos_min_version = '14.0'
add_global_arguments('-mmacosx-version-min=@0@'.format(macos_min_version),
                     language: ['c', 'objc'])
add_global_link_arguments('-mmacosx-version-min=@0@'.format(macos_min_version),
                          language: ['c', 'objc'])

# Headers Apple's libc lacks (C11 threads/uchar), plus the handful of
# Linux constants foot uses as its own vocabulary. See macos/compat/.
compat_dir = meson.current_source_dir() / 'compat'

add_project_arguments(
  ['-D_DARWIN_C_SOURCE',
   '-I' + compat_dir,
   '-I' + meson.current_source_dir(),
   '-I' + root,
   '-include', compat_dir / 'compat.h',
   '-DFOOT_DEFAULT_TERM="@0@"'.format(get_option('default-terminfo'))] +
  (is_debug_build
    ? ['-D_DEBUG']
    : [cc.get_supported_arguments('-fno-asynchronous-unwind-tables')]) +
  (get_option('ime')
    ? ['-DFOOT_IME_ENABLED=1']
    : []) +
  cc.get_supported_arguments(
    ['-pedantic',
     '-fstrict-aliasing',
     '-Wstrict-aliasing']),
  language: ['c', 'objc'],
)

add_project_arguments(
  ['-fobjc-arc', '-Wno-gnu-zero-variadic-macro-arguments'],
  language: 'objc')

terminfo_install_location = get_option('custom-terminfo-install-location')

if terminfo_install_location != ''
  add_project_arguments(
    ['-DFOOT_TERMINFO_PATH="@0@"'.format(
      join_paths(get_option('prefix'), terminfo_install_location))],
    language: ['c', 'objc'])
else
  terminfo_install_location = join_paths(get_option('datadir'), 'terminfo')
endif

# Compute the relative path used by compiler invocations.
source_root = root.split('/')
build_root = meson.global_build_root().split('/')
relative_dir_parts = []
i = 0
in_prefix = true
foreach p : build_root
  if i >= source_root.length() or not in_prefix or p != source_root[i]
    in_prefix = false
    relative_dir_parts += '..'
  endif
  i += 1
endforeach
i = 0
in_prefix = true
foreach p : source_root
  if i >= build_root.length() or not in_prefix or build_root[i] != p
    in_prefix = false
    relative_dir_parts += p
  endif
  i += 1
endforeach
relative_dir = join_paths(relative_dir_parts) + '/'

if cc.has_argument('-fmacro-prefix-map=/foo=')
  add_project_arguments('-fmacro-prefix-map=@0@='.format(relative_dir),
                        language: ['c', 'objc'])
endif

math = cc.find_library('m')
threads = dependency('threads')

# Built from source, and linked statically: the application bundle is
# self-contained, and everything targets the same macOS version.
pixman = subproject(
  'pixman',
  default_options: [
    'default_library=static',
    'werror=false',
    'tests=disabled',
    'demos=disabled',
    'gtk=disabled',
    'libpng=disabled',
    'openmp=disabled',
  ]).get_variable('idep_pixman')

if get_option('grapheme-clustering').disabled()
  utf8proc = dependency('', required: false)
else
  utf8proc = subproject(
    'utf8proc',
    default_options: ['default_library=static', 'werror=false'],
  ).get_variable('utf8proc_dep')
endif

if utf8proc.found()
  add_project_arguments('-DFOOT_GRAPHEME_CLUSTERING=1', language: ['c', 'objc'])
endif

add_project_arguments('-DHAVE_PIXMAN_RGBA_16', language: ['c', 'objc'])

tllist = dependency('tllist', version: '>=1.1.0', fallback: 'tllist')

# xkbcommon is used purely as a keyboard state machine, with keymaps
# generated from the active macOS keyboard layout. It is always built
# statically, without any X11/Wayland support.
xkb = subproject(
  'libxkbcommon',
  default_options: [
    'default_library=static',
    'werror=false',
    'enable-x11=false',
    'enable-wayland=false',
    'enable-tools=false',
    'enable-docs=false',
    'enable-xkbregistry=false',
    'enable-bash-completion=false',
  ]).get_variable('dep_libxkbcommon')

appkit = dependency(
  'appleframeworks',
  modules: ['AppKit', 'Carbon', 'CoreFoundation', 'CoreGraphics',
            'CoreText', 'IOSurface', 'QuartzCore', 'UserNotifications'])

coretext = dependency(
  'appleframeworks',
  modules: ['CoreFoundation', 'CoreGraphics', 'CoreText'])

# foot's font API (fcft), implemented natively on CoreText
fcft = declare_dependency(
  link_with: static_library(
    'fcft-coretext',
    'fcft.c', 'fcft/fcft.h',
    'font-pattern.c', 'font-pattern.h',
    dependencies: [pixman, coretext, utf8proc],
  ),
  dependencies: [pixman, coretext],
)

env = find_program('env', native: true)
generate_version_sh = files('../generate-version.sh')
version = custom_target(
  'generate_version',
  build_always_stale: true,
  output: 'version.h',
  command: [env, 'LC_ALL=C', generate_version_sh, meson.project_version(), root, '@OUTPUT@'])

python = find_program('python3', native: true)
generate_builtin_terminfo_py = files('../scripts/generate-builtin-terminfo.py')
foot_terminfo = files('../foot.info')
builtin_terminfo = custom_target(
  'generate_builtin_terminfo',
  output: 'foot-terminfo.h',
  command: [python, generate_builtin_terminfo_py,
            '@default_terminfo@', foot_terminfo, 'foot', '@OUTPUT@']
)

generate_emoji_variation_sequences = files('../scripts/generate-emoji-variation-sequences.py')
emoji_variation_sequences = custom_target(
  'generate_emoji_variation_sequences',
  input: files('../unicode/emoji-variation-sequences.txt'),
  output: 'emoji-variation-sequences.h',
  command: [python, generate_emoji_variation_sequences, '@INPUT@', '@OUTPUT@']
)

# The documented default configuration; written when the user opens
# the settings (Cmd+,) without having a configuration file
default_config = custom_target(
  'generate_default_config',
  input: files('../foot.ini'),
  output: 'default-config.h',
  command: [python, files('embed-file.py'), '@INPUT@', '@OUTPUT@', 'default_config',
            '--macos-defaults']
)

generate_srgb_funcs = files('../scripts/srgb.py')
srgb_funcs = custom_target(
  'generate_srgb_funcs',
  output: ['srgb.c', 'srgb.h'],
  command: [python, generate_srgb_funcs, '@OUTPUT0@', '@OUTPUT1@']
)

common = static_library(
  'common',
  '../log.c', '../log.h',
  '../char32.c', '../char32.h',
  '../debug.c', '../debug.h',
  '../macros.h',
  '../xmalloc.c', '../xmalloc.h',
  '../xsnprintf.c', '../xsnprintf.h',
  dependencies: [utf8proc]
)

misc = static_library(
  'misc',
  '../hsl.c', '../hsl.h',
  '../macros.h',
  '../misc.c', '../misc.h',
  '../uri.c', '../uri.h',
  dependencies: [utf8proc],
  link_with: [common]
)

vtlib = static_library(
  'vtlib',
  '../base64.c', '../base64.h',
  '../composed.c', '../composed.h',
  '../cursor-shape.c', '../cursor-shape.h',
  '../csi.c', '../csi.h',
  '../dcs.c', '../dcs.h',
  '../macros.h',
  '../osc.c', '../osc.h',
  '../sixel.c', '../sixel.h',
  '../vt.c', '../vt.h',
  builtin_terminfo, srgb_funcs,
  version,
  dependencies: [pixman, fcft, tllist, xkb, utf8proc],
  link_with: [common, misc],
)

pgolib = static_library(
  'pgolib',
  '../grid.c', '../grid.h',
  '../selection.c', '../selection.h',
  '../terminal.c', '../terminal.h',
  emoji_variation_sequences,
  dependencies: [pixman, fcft, tllist, xkb, utf8proc],
  link_with: vtlib,
)

tokenize = static_library(
  'tokenizelib',
  '../tokenize.c',
  dependencies: [utf8proc],
  link_with: [common],
)

foot = executable(
  'foot',
  '../async.c', '../async.h',
  '../box-drawing.c', '../box-drawing.h',
  '../config.c', '../config.h',
  '../commands.c', '../commands.h',
  '../extract.c', '../extract.h',
  '../foot-features.c', '../foot-features.h',
  '../ime.c', '../ime.h',
  '../input.c', '../input.h',
  '../key-binding.c', '../key-binding.h',
  '../main.c',
  '../notify.c', '../notify.h',
  '../reaper.c', '../reaper.h',
  '../render.c', '../render.h',
  '../search.c', '../search.h',
  '../server.c', '../server.h', '../client-protocol.h',
  '../slave.c', '../slave.h',
  '../spawn.c', '../spawn.h',
  '../tokenize.c', '../tokenize.h',
  '../unicode-mode.c', '../unicode-mode.h',
  '../url-mode.c', '../url-mode.h',
  '../user-notification.c', '../user-notification.h',
  '../wayland.h', '../shm.h',
  'fdm.c', '../fdm.h',
  'timerfd.c', 'compat/sys/timerfd.h',
  'shm.c',
  'app.m', 'app.h',
  'window.m',
  'view.m', 'view.h',
  'keyboard.m', 'keyboard.h',
  'clipboard.m',
  'selection-clipboard.c',
  'windows.c', 'windows.h',
  'themes.m',
  'updates.m',
  'default-colors.c', 'default-colors.h',
  default_config, srgb_funcs, version,
  dependencies: [math, threads, pixman, xkb, utf8proc, tllist, fcft, appkit],
  link_with: pgolib,
  install: true)

footclient = executable(
  'footclient',
  '../client.c', '../client-protocol.h',
  '../foot-features.c', '../foot-features.h',
  '../macros.h',
  '../util.h',
  version,
  dependencies: [tllist, utf8proc],
  link_with: [common, misc],
  install: true)

install_data('../foot.ini', install_dir: join_paths(get_option('sysconfdir'), 'xdg', 'foot'))
install_data(
  '../LICENSE', '../README.md', '../CHANGELOG.md',
  install_dir: join_paths(get_option('datadir'), 'doc', 'foot'))

if get_option('themes')
  install_subdir('../themes', install_dir: join_paths(get_option('datadir'), 'foot'))
  install_subdir('themes', install_dir: join_paths(get_option('datadir'), 'foot'))
endif

terminfo_base_name = get_option('terminfo-base-name')
if terminfo_base_name == ''
  terminfo_base_name = get_option('default-terminfo')
endif

tic = find_program('tic', native: true, required: get_option('terminfo'))
if tic.found()
  conf_data = configuration_data(
    {
      'default_terminfo': terminfo_base_name
    }
  )

  preprocessed = configure_file(
    input: files('../foot.info'),
    output: 'foot.info.preprocessed',
    configuration: conf_data,
  )
  # macOS' ncurses uses hashed (hex) directory names, e.g. 66/foot.
  # The -direct variant needs extended numbers (ncurses >= 6.1), which
  # macOS' ncurses doesn't support.
  terminfo = custom_target(
    'terminfo',
    output: 'terminfo',
    input: preprocessed,
    command: [find_program('sh'), '-c',
              'rm -rf "$1" && mkdir -p "$1" && exec "$2" -x -o "$1" -e "$3" "$4"', 'sh',
              '@OUTPUT@', tic, terminfo_base_name, '@INPUT@'],
    build_by_default: true,
  )
endif

# The application bundle
info_plist = configure_file(
  input: 'Info.plist.in',
  output: 'Info.plist',
  configuration: {
    'version': meson.project_version(),
    'bundle_id': 'io.github.gobijan.foot',
    'macos_min_version': macos_min_version,
  },
)

custom_target(
  'foot.app',
  output: 'foot.app',
  input: [foot, footclient, info_plist, files('icon/foot.icns'), terminfo],
  command: [files('make-bundle.sh'), '@OUTPUT@', '@INPUT@', root / 'themes',
            root / 'macos' / 'themes'],
  build_by_default: true,
  build_always_stale: true,
)

install_data('../completions/zsh/_foot', '../completions/zsh/_footclient',
             install_dir: join_paths(get_option('datadir'), 'zsh', 'site-functions'))
install_data('../completions/fish/foot.fish', '../completions/fish/footclient.fish',
             install_dir: join_paths(get_option('datadir'), 'fish', 'vendor_completions.d'))
install_data('../completions/bash/foot', '../completions/bash/footclient',
             install_dir: join_paths(get_option('datadir'), 'bash-completion', 'completions'))

executable('xtgettcap', '../utils/xtgettcap.c')

if get_option('tests')
  # tests/test-config.c includes config.c, which doesn't use fontconfig on macOS
  config_test = executable(
    'test-config',
    '../tests/test-config.c',
    'default-colors.c', 'default-colors.h',
    c_args: ['-DFcInit()=1', '-DFcFini()='],
    link_with: [common, tokenize],
    dependencies: [pixman, xkb, fcft, tllist, utf8proc])

  test('config', config_test)
endif

summary(
  {
    'macOS deployment target': macos_min_version,
    'Themes': get_option('themes'),
    'IME': get_option('ime'),
    'Grapheme clustering': utf8proc.found(),
    'Build terminfo': tic.found(),
    'Terminfo base name': terminfo_base_name,
    'Terminfo install location': terminfo_install_location,
    'Default TERM': get_option('default-terminfo'),
    'Set TERMINFO': get_option('custom-terminfo-install-location') != '',
    'Build tests': get_option('tests'),
  },
  bool_yn: true
)
