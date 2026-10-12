project('skippy-wl', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  default_options: ['c_std=c99', 'warning_level=3'])

add_project_arguments('-D_GNU_SOURCE',
  '-DSKIPPY_VERSION="@0@"'.format(meson.project_version()), language: 'c')

wl_client = dependency('wayland-client')
cairo = dependency('cairo')
xkbcommon = dependency('xkbcommon')
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

executable('skippy-wl', 'src/main.c',
  dependencies: [protocols_dep, cairo, xkbcommon],
  install: false)
