# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# pgpatch is macOS/arm64-only for now (it uses mach_vm code patching, dyld and
# Mach-O symbol search, and .dSYM DWARF).  Skip it cleanly on other platforms
# so it doesn't break the rest of the build; relax this when a port lands.
if host_machine.system() != 'darwin' or host_machine.cpu_family() != 'aarch64'
  subdir_done()
endif

# Build the vendored Janet amalgamation on its own with warnings disabled: it
# is third-party C99 code and trips many of Postgres's stricter warnings.
janet_inc = include_directories('vendor/janet')
janet_lib = static_library('pgpatch_janet',
  files('vendor/janet/janet.c'),
  include_directories: janet_inc,
  c_args: ['-w'],
  pic: true,
  build_by_default: false,
)

pgpatch_sources = files(
  'src/pgpatch.c',
  'src/janet_embed.c',
  'src/repl.c',
  'src/patch.c',
  'src/trampoline_arm64.c',
  'src/introspect.c',
  'src/dwarf.c',
)

pgpatch = shared_module('pgpatch',
  pgpatch_sources,
  include_directories: [include_directories('src'), janet_inc],
  link_with: janet_lib,
  link_args: ['-lffi'],
  kwargs: contrib_mod_args,
)
contrib_targets += pgpatch

install_data(
  'pgpatch.control',
  'pgpatch--1.0.sql',
  kwargs: contrib_data_args,
)
