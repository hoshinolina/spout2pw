project('spout2pw', 'c',
    meson_version: '>=1.1'
)

if not meson.is_cross_build()
    error('must be invoked with "meson setup --cross-file=./x86_64-w64-mingw32.txt"')
endif

message('Checking wine PE library path...')
r = run_command(join_paths(meson.project_source_root(), 'tools/get_wine_path.sh'),
    'x86_64-w64-mingw32', '^/.*/libwinecrt0.a$', check: true)
out = r.stdout().strip().split('\n')[0]
if out.endswith('libwinecrt0.a')
    wine_lib_paths = [out.substring(0, -13)]
    message('Wine PE library paths: ' + ' '.join(wine_lib_paths))
else
    error('Wine library path not found')
endif

message('Checking wine UNIX library path...')
r = run_command(join_paths(meson.project_source_root(), 'tools/get_wine_path.sh'),
    '', '^/.*/libwinecrt0.a$', check: true)
out = r.stdout().strip().split('\n')[0]
if out.endswith('libwinecrt0.a')
    unix_lib_paths = [out.substring(0, -13)]
    message('Wine UNIX library paths: ' + ' '.join(unix_lib_paths))
else
    error('Wine library path not found')
endif

message('Checking wine PE include paths...')
r = run_command(join_paths(meson.project_source_root(), 'tools/get_wine_path.sh'),
    'x86_64-w64-mingw32', '^-i(system|dirafter)/', check: true)
wine_include_args = r.stdout().strip().split('\n')
if wine_include_args.length() > 0
    message('Wine PE include args: ' + ' '.join(wine_include_args))
else
    error('Wine include args not found')
endif

message('Checking wine UNIX include paths...')
r = run_command(join_paths(meson.project_source_root(), 'tools/get_wine_path.sh'),
    '', '^-i(system|dirafter)/', check: true)
unix_include_args = r.stdout().strip().split('\n')
if unix_include_args.length() > 0
    message('Wine UNIX include args: ' + ' '.join(unix_include_args))
else
    error('Wine include args not found')
endif

c = meson.get_compiler('c')

spoutdxtoc_proj = subproject('spoutdxtoc')
spoutdxtoc_dep = spoutdxtoc_proj.get_variable('spoutdxtoc_dep')

libfunnel_proj = subproject(
  'libfunnel',
  default_options: [
    'native=true',
    'test_apps=false',
    'egl=false',
    'default_library=static',
    'static_pipewire=true',
  ]
)

funnel = libfunnel_proj.get_variable('funnel')
funnel_vk = libfunnel_proj.get_variable('funnel_vk')

windows_src = [
  'src/spout2pw.c',
]

unix_src = [
  'src/spout2pw_unix.c',
]

windows_compiler_args = [
    '-Wno-error', '-std=c99', '-mcmodel=small',
    '-fno-omit-frame-pointer', '-O2', '-march=nocona',
    '-mtune=core-avx2', '-mfpmath=sse', '-D__WINE_PE_BUILD',
    '-mcx16', '-fno-strict-aliasing', '-ffunction-sections'
] + wine_include_args

windows_link_args = [ ]
unix_compiler_args = [ '-DWINE_UNIX_LIB' ] + unix_include_args
unix_link_args = [
  '-l:ntdll.so',
  '-Wl,--whole-archive',
  '-Wl,' + get_option('libpipewire_static_lib'),
  '-Wl,--no-whole-archive',
]

windows_deps = [
  c.find_library('d3d11', dirs: wine_lib_paths),
  c.find_library('ntdll', dirs: wine_lib_paths),
  c.find_library('ucrt', dirs: wine_lib_paths),
  c.find_library('winecrt0', dirs: wine_lib_paths),
  spoutdxtoc_dep,
]

vulkan = dependency('vulkan', native: true)

unix_deps = [
  funnel,
  funnel_vk,
  vulkan,
]

foreach p : unix_lib_paths
  unix_link_args += ['-L' + p]
endforeach

spout2pw_bin = executable(
  'spout2pw',
  windows_src,
  name_prefix: '',
  c_args: windows_compiler_args,
  link_args: windows_link_args,
  dependencies: [windows_deps],
  win_subsystem: 'console',
)

unixlib = shared_library(
  'spout2pw',
  unix_src,
  name_prefix: '',
  c_args: unix_compiler_args,
  link_args: unix_link_args,
  dependencies: unix_deps,
  native: true,
)

fs = import('fs')

meson.add_install_script('tools/package.sh')
