config_test = executable(
  'test-config',
  'test-config.c',
  link_with: [common, tokenize],
  dependencies: [pixman, xkb, fcft, tllist, utf8proc])

test('config', config_test)
