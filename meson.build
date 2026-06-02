project('wayland-color-picker', 'c',
  version : '0.1.0',
  default_options : [
    'c_std=c11',
    'warning_level=3',
    'werror=true',
  ],
)

cc = meson.get_compiler('c')

extra_args = cc.get_supported_arguments([
  '-D_GNU_SOURCE',
  '-Wconversion',
  '-Wshadow',
  '-Wundef',
  '-fstack-protector-strong',
])

wayland_client = dependency('wayland-client')
wayland_scanner = find_program('wayland-scanner')

# Generate protocol stubs from vendored XMLs.
protocols = [
  'xdg-shell',
  'ext-image-capture-source-v1',
  'ext-image-copy-capture-v1',
  'wlr-layer-shell-unstable-v1',
]

protocol_sources = []
protocol_headers = []

foreach proto : protocols
  xml = 'protocol' / proto + '.xml'

  header = custom_target(proto + '-header',
    input  : xml,
    output : proto + '-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )

  source = custom_target(proto + '-source',
    input  : xml,
    output : proto + '-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )

  protocol_headers += header
  protocol_sources += source
endforeach

executable('wayland-color-picker',
  sources : ['src/main.c', 'src/stubs.c'] + protocol_sources + protocol_headers,
  dependencies : [wayland_client],
  c_args : extra_args,
  include_directories : include_directories('.'),
)
