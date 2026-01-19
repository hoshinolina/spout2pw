project('spout2pw', 'c')

if not meson.is_cross_build()
    error('must be invoked with "meson setup --cross-file=./x86_64-w64-mingw32.txt"')
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
  'spout2pw.c',
]

unix_src = [
  'spout2pw_unix.c',
]

windows_compiler_args = [ '-Wno-error', '-std=c99', '-mcmodel=small', '-fno-omit-frame-pointer', '-O2', '-march=nocona', '-mtune=core-avx2', '-mfpmath=sse', '-D__WINE_PE_BUILD', '-mcx16', '-fno-strict-aliasing', '-ffunction-sections']
windows_link_args = [ '-Wno-error', '-std=c99' ]
windows_inc = [
  '/usr/include/wine/windows',
]

unix_compiler_args = [ '-DWINE_UNIX_LIB' ]
unix_inc = windows_inc

wine_lib_path = '/usr/lib64/wine/x86_64-windows/'
unix_lib_path = '/usr/lib64/wine/x86_64-unix/'

windows_deps = [
  c.find_library('d3d11', dirs: wine_lib_path),
  c.find_library('ntdll', dirs: wine_lib_path),
  c.find_library('winecrt0', dirs: wine_lib_path),
  spoutdxtoc_dep,
]

unix_deps = [
  funnel,
  funnel_vk,
]

unix_link_args=[
  '-L' + unix_lib_path,
  '-l:ntdll.so',
  '-Wl,--whole-archive',
  '-Wl,' + get_option('libpipewire_static_lib'),
  '-Wl,--no-whole-archive',
]

spout2pw_bin = executable(
  'spout2pw',
  windows_src,
  include_directories: windows_inc,
  name_prefix: '',
  c_args: windows_compiler_args,
  link_args: windows_link_args,
  dependencies: [windows_deps],
  win_subsystem: 'console',
)

vulkan = dependency('vulkan', native: true)
egl = dependency('egl', native: true)
x11 = dependency('x11', native: true)
gl = dependency('GL', native: true)

unixlib = shared_library(
  'spout2pw',
  unix_src,
  include_directories: unix_inc,
  name_prefix: '',
  c_args: unix_compiler_args,
  link_args: unix_link_args,
  dependencies: [unix_deps, vulkan, gl, egl, x11],
  native: true,
)

fs = import('fs')

meson.add_install_script('package.sh')
