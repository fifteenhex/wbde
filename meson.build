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
need = ['smolinput']
if get_option('soft')
  need += ['smol2d', 'smoldrm']
endif
smol = {}
foreach name : need
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
  if not fs.is_dir(dir)
    error('@0@ not found at @1@ (-D@0@=DIR)'.format(name, dir))
  endif
  smol += {name: dir}
endforeach

inc = include_directories('src')
smol_inc = [include_directories(smol['smolinput'], is_system: true)]

readers = files('src/rom.c', 'src/font.c', 'src/gfx.c', 'src/iff.c', 'src/icon.c',
                'src/hunk.c')
core = readers + files('src/prefs.c', 'src/wm.c', 'src/menu.c', 'src/wb.c',
                       'src/input.c', 'src/con.c', 'src/main.c')

# One display, picked at build time: smol2d on DRM, or memory only
if get_option('soft')
  disp = files('src/disp_soft.c', smol['smol2d'] / 'libsmol2ddrm/src/smol2ddrm.c')
  smol_inc += include_directories(smol['smol2d'] / 'include', smol['smoldrm'],
                                  is_system: true)
else
  disp = files('src/disp_file.c')
endif

util = cc.find_library('util')

executable('wbde', core + disp,
  include_directories: [inc, smol_inc],
  dependencies: util,
  install: true)

executable('wbrom', 'tools/wbrom.c', readers, include_directories: inc)

install_data('scripts/wbde.sh', install_dir: get_option('bindir'),
             install_mode: 'rwxr-xr-x')
