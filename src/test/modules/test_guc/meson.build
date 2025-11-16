# Copyright (c) 2025, PostgreSQL Global Development Group

test_guc_sources = files(
  'test_guc.c',
)

if host_system == 'windows'
  test_guc_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_guc',
    '--FILEDESC', 'test_guc - test module for GUC_EXTRA_IS_CONTEXT',])
endif

test_guc = shared_module('test_guc',
  test_guc_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_guc

test_install_data += files(
  'test_guc.control',
  'test_guc--1.0.sql',
)

tests += {
  'name': 'test_guc',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_simple_counter.pl',
      't/002_server_pools.pl',
    ],
  },
}
