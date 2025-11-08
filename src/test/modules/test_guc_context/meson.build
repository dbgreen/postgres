# src/test/modules/test_guc_context/meson.build

test_guc_context_sources = files(
  'test_guc_context.c',
)

if host_system == 'windows'
  test_guc_context_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_guc_context',
    '--FILEDESC', 'test_guc_context - test GUC_EXTRA_IS_CONTEXT functionality',])
endif

test_guc_context = shared_module('test_guc_context',
  test_guc_context_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_guc_context
test_install_data += files(
  'test_guc_context.control',
  'test_guc_context--1.0.sql',
)
tests += {
  'name': 'test_guc_context',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_guc_context.pl',
    ],
  },
}