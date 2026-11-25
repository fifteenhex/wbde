project('wbde', 'c',
  version: '0.1',
  meson_version: '>= 1.1',
  default_options: ['buildtype=debugoptimized', 'warning_level=2'])

fs = import('fs')
cc = meson.get_compiler('c')

add_project_arguments('-Wno-unused-parameter', '-Wno-sign-compare', language: 'c')

# m68k: a static binary for the chosen CPU
cpu_args = []
if host_machine.cpu_family() == 'm68k'
  cpu_args = ['-mcpu=' + get_option('cpu')]
  add_project_arguments(cpu_args, '-fno-PIE', '-fno-stack-protector',
                        '-falign-functions=16', '-D_TIME_BITS=64', language: 'c')
  add_project_link_arguments('-no-pie', '-static', language: 'c')
endif

# The smol* libraries are used from their source checkouts
smol = {}
foreach name : ['smol2d', 'smoldrm', 'smolinput']
  # resolved here: meson refuses a path into the source tree that only
  # leaves it again through '..', as the default '../<name>' does
  dir = get_option(name)
  if not dir.startswith('/')
    p = meson.project_source_root()
    foreach part : dir.split('/')
      if part == '..'
        p = fs.parent(p)
      elif part not in ['', '.']
        p = p / part
      endif
    endforeach
    dir = p
  endif
  smol += {name: dir}
endforeach

inc = include_directories('src')

readers = files('src/rom.c', 'src/font.c', 'src/gfx.c', 'src/iff.c', 'src/icon.c',
                'src/hunk.c')
core = readers + files('src/prefs.c', 'src/wm.c', 'src/menu.c', 'src/con.c',
                       'src/main.c')

# One display, picked at build time: smol2d on DRM, or memory only
if get_option('soft')
  foreach name : ['smol2d', 'smoldrm', 'smolinput']
    if not fs.is_dir(smol[name])
      error('@0@ not found at @1@ (-D@0@=DIR)'.format(name, smol[name]))
    endif
  endforeach
  disp = files('src/disp_soft.c', smol['smol2d'] / 'libsmol2ddrm/src/smol2ddrm.c')
  disp_inc = include_directories(smol['smol2d'] / 'include', smol['smoldrm'],
                                 smol['smolinput'], is_system: true)
else
  disp = files('src/disp_file.c')
  disp_inc = []
endif

util = cc.find_library('util')

executable('wbde', core + disp,
  include_directories: [inc, disp_inc],
  dependencies: util,
  install: true)

executable('wbrom', 'tools/wbrom.c', readers, include_directories: inc)
