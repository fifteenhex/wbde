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

# Debian's gcc 16 for m68k spills a float that is live across a call as
# 80-bit extended and reloads it as a 4-byte single (wrong code at -O2).
# tools/floatcheck.c reproduces it; when it fails, or cannot be run,
# -ffloat-store avoids it.
if meson.can_run_host_binaries()
  r = cc.run(files('tools/floatcheck.c'), args: cpu_args + ['-O2', '-lm'],
             name: 'floats survive a call without -ffloat-store')
  need_float_store = not r.compiled() or r.returncode() != 0
else
  need_float_store = true
  message('cannot run host binaries: assuming -ffloat-store is needed')
endif
if need_float_store
  add_project_arguments('-ffloat-store', language: 'c')
endif

gpu = get_option('gpu')
soft = get_option('soft')
fake_gpu = get_option('fake_gpu')
if fake_gpu and gpu != 'rdn'
  error('fake_gpu is a fake Radeon: it needs -Dgpu=rdn')
endif

# The smol* libraries are used from their source checkouts
need = ['smolinput']
if soft
  need += ['smol2d', 'smoldrm']
endif
if gpu != 'none'
  need += ['smolgpu', 'smoldrm', gpu == 'rdn' ? 'smolrdn' : 'smolnvda']
endif
if fake_gpu
  need += ['smolgl']
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

inc = include_directories('src', 'include')
smol_inc = [include_directories(smol['smolinput'], is_system: true)]

readers = files('src/rom.c', 'src/font.c', 'src/gfx.c', 'src/iff.c', 'src/icon.c',
                'src/hunk.c')
core = readers + files('src/prefs.c', 'src/wm.c', 'src/menu.c', 'src/wb.c',
                       'src/input.c', 'src/server.c', 'src/con.c', 'src/main.c',
                       'src/disp.c', 'src/disp_file.c')

# The displays built in, tried at run time in this order: gpu, soft, the
# memory display (always there)
disp_args = []
disp_srcs = []
disp_objs = []
if soft
  disp_args += '-DHAVE_DISP_SOFT'
  disp_srcs += files('src/disp_soft.c', smol['smol2d'] / 'libsmol2ddrm/src/smol2ddrm.c')
  smol_inc += include_directories(smol['smol2d'] / 'include', smol['smoldrm'],
                                  is_system: true)
endif

if gpu != 'none'
  # disp_gpu.c is the only source that sees smolgpu, and has flags of its own
  gpu_args = ['-DHAVE_DISP_GPU', '-Wno-unused-function', '-Wno-missing-field-initializers']
  if gpu == 'rdn'
    gpu_args += '-DSGPU_RDN=1'
  else
    gpu_args += '-DSGPU_NVDA=1'
  endif
  gpu_inc = include_directories(smol['smolgpu'], smol[gpu == 'rdn' ? 'smolrdn' : 'smolnvda'],
                                smol['smoldrm'], is_system: true)
  gpu_lib = static_library('disp_gpu', 'src/disp_gpu.c',
    c_args: gpu_args,
    include_directories: [inc, smol_inc, gpu_inc],
    pic: false)
  disp_objs += gpu_lib.extract_all_objects(recursive: false)
  disp_args += '-DHAVE_DISP_GPU'
endif

# smolgl's fake Radeon checks every command stream as the kernel would
if fake_gpu
  fake_lib = static_library('fakedrm', smol['smolgl'] / 'tests/fakedrm.c',
    override_options: ['warning_level=0', 'optimization=1'],
    pic: false)
  disp_objs += fake_lib.extract_all_objects(recursive: false)
endif

util = cc.find_library('util')
libm = cc.find_library('m', required: false)

executable('wbde', core + disp_srcs,
  c_args: disp_args,
  include_directories: [inc, smol_inc],
  objects: disp_objs,
  dependencies: [util, libm],
  install: true)

executable('wbrom', 'tools/wbrom.c', readers, include_directories: inc)
executable('wbclient-demo', 'tools/wbclient-demo.c', include_directories: inc)

install_data('scripts/wbde.sh', install_dir: get_option('bindir'),
             install_mode: 'rwxr-xr-x')
install_headers('include/wbde_proto.h', 'include/wbclient.h', subdir: 'wbde')
install_data('README.md', 'doc/client-protocol.md',
             install_dir: get_option('datadir') / 'doc' / 'wbde')
