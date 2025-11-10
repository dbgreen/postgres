test_guc_contexts_sources = files(
  'test_guc_contexts.c',
)

if host_system == 'windows'
  test_guc_contexts_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_guc_contexts',
    '--FILEDESC', 'test_guc_contexts - test GUC_EXTRA_IS_CONTEXT with simple and complex structures',])
endif

test_guc_contexts = shared_module('test_guc_contexts',
  test_guc_contexts_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_guc_contexts

test_install_data += files(
  'test_guc_contexts.control',
  'test_guc_contexts--1.0.sql',
)

tests += {
  'name': 'test_guc_contexts',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_simple_contexts.pl',
      't/002_complex_contexts.pl',
    ],
  },
}
