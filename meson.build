project('wbde', 'c',
  version: '0.1',
  meson_version: '>= 1.1',
  default_options: ['buildtype=debugoptimized', 'warning_level=2'])

add_project_arguments('-Wno-unused-parameter', '-Wno-sign-compare', language: 'c')

# m68k: a static binary for the chosen CPU
cpu_args = []
if host_machine.cpu_family() == 'm68k'
  cpu_args = ['-mcpu=' + get_option('cpu')]
  add_project_arguments(cpu_args, '-fno-PIE', '-fno-stack-protector',
                        '-falign-functions=16', '-D_TIME_BITS=64', language: 'c')
  add_project_link_arguments('-no-pie', '-static', language: 'c')
endif

inc = include_directories('src')

readers = files('src/rom.c', 'src/font.c', 'src/gfx.c', 'src/iff.c', 'src/icon.c',
                'src/hunk.c')

executable('wbrom', 'tools/wbrom.c', readers, include_directories: inc)
