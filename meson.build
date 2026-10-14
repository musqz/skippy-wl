project('skippy-wl', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  default_options: ['c_std=c99', 'warning_level=3'])

add_project_arguments('-D_GNU_SOURCE',
  '-DSKIPPY_VERSION="@0@"'.format(meson.project_version()), language: 'c')

wl_client = dependency('wayland-client')
cairo = dependency('cairo')
xkbcommon = dependency('xkbcommon')
pangocairo = dependency('pangocairo')
librsvg = dependency('librsvg-2.0', version: '>=2.52')
gdk_pixbuf = dependency('gdk-pixbuf-2.0')
libheif = dependency('libheif')
scanner = find_program('wayland-scanner')

proto_xml = [
  'ext-foreign-toplevel-list-v1',
  'ext-image-capture-source-v1',
  'ext-image-copy-capture-v1',
  'ext-workspace-v1',
  'wlr-foreign-toplevel-management-unstable-v1',
  'wlr-layer-shell-unstable-v1',
  'xdg-shell',
]

proto_hdrs = []
proto_srcs = []
foreach p : proto_xml
  xml = files('protocols' / p + '.xml')
  hdr = custom_target(p + '-header',
    input: xml, output: p + '-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  proto_hdrs += hdr
  proto_srcs += hdr
  proto_srcs += custom_target(p + '-code',
    input: xml, output: p + '-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach

protocols = static_library('protocols', proto_srcs, dependencies: wl_client)
protocols_dep = declare_dependency(
  link_with: protocols,
  sources: proto_hdrs,
  dependencies: wl_client)

executable('skippy-wl',
  'src/main.c', 'src/capture.c', 'src/toplevels.c', 'src/globals.c', 'src/wlutil.c',
  'src/layout.c', 'src/input.c', 'src/overlay.c', 'src/config.c', 'src/ipc.c', 'src/daemon.c', 'src/filter.c', 'src/icons.c', 'src/bgimage.c',
  dependencies: [protocols_dep, cairo, xkbcommon, pangocairo, librsvg, gdk_pixbuf, libheif],
  install: true)

install_man('man/skippy-wl.1')
install_data('configs/skippy-wl.conf', install_dir: get_option('datadir') / 'skippy-wl')

executable('skippy-wl-probe', 'src/probe.c',
  dependencies: [protocols_dep])

test_layout = executable('test_layout', 'tests/test_layout.c', 'src/layout.c',
  include_directories: include_directories('src'))
test('layout', test_layout)

test_config = executable('test_config', 'tests/test_config.c', 'src/config.c',
  include_directories: include_directories('src'),
  dependencies: [xkbcommon])
test('config', test_config)

test_filter = executable('test_filter', 'tests/test_filter.c', 'src/filter.c',
  include_directories: include_directories('src'))
test('filter', test_filter)

test_icons = executable('test_icons', 'tests/test_icons.c', 'src/icons.c',
  include_directories: include_directories('src'),
  dependencies: [cairo, librsvg])
test('icons', test_icons)
