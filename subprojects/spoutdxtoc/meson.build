project(
  'spoutdxtoc',
  ['c', 'cpp'],
  default_options: [ 'cpp_std=c++11', 'warning_level=2' ]
)

if target_machine.system() != 'windows'
  error('Windows-only library')
endif

cpp = meson.get_compiler('cpp')

link_args = [
  '-static',
  '-static-libgcc',
  '-static-libstdc++',
  # We need to set the section alignment for debug symbols to
  # work properly as well as avoiding a memcpy from the Wine loader.
  '-Wl,--file-alignment=4096',
]

compiler_args = [ '-msse4', '-gdwarf-4' ] # TODO: Put gdwarf as debug-only

spoutdx_include_dir = []

# Windows and case insensitivity…
if build_machine.system() != 'windows'
spoutdx_include_dir += include_directories('./fixups')
endif

spoutdx_include_dir += [
  include_directories('./Spout2/SPOUTSDK/SpoutGL'),
  include_directories('./Spout2/SPOUTSDK/SpoutDirectX/SpoutDX'),
]

spoutdx_src = [
  'Spout2/SPOUTSDK/SpoutGL/SpoutCopy.cpp',
  'Spout2/SPOUTSDK/SpoutGL/SpoutDirectX.cpp',
  'Spout2/SPOUTSDK/SpoutGL/SpoutFrameCount.cpp',
  'Spout2/SPOUTSDK/SpoutGL/SpoutSenderNames.cpp',
  'Spout2/SPOUTSDK/SpoutGL/SpoutSharedMemory.cpp',
  'Spout2/SPOUTSDK/SpoutGL/SpoutUtils.cpp',
]

spoutdx_deps = [
  cpp.find_library('dxgi'),
  cpp.find_library('d3d11'),
  cpp.find_library('version'),
  cpp.find_library('winmm'),
]

# Isolate SpoutDX warnings by building it as a static library
spout_dx = static_library(
  'spoutdx',
  spoutdx_src,
  include_directories: spoutdx_include_dir,
  cpp_args: [
    compiler_args,
    '-w', # Ignore all warnings
  ],
  link_args: link_args,
)

spoutdxtoc_dll = shared_library(
  'spoutdxtoc',
  'spoutdxtoc.cpp',
  name_prefix: '',
  include_directories: spoutdx_include_dir,
  c_args: compiler_args,
  cpp_args: [
    compiler_args,
    '-Wno-unknown-pragmas',
  ],
  vs_module_defs: 'spoutdxtoc.def',
  link_args: link_args,
  link_with: spout_dx,
  dependencies: spoutdx_deps,
)

spoutdxtoc_dep = declare_dependency(
  link_with : spoutdxtoc_dll,
  include_directories : '.',
)
