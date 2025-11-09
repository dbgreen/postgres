# src/test/modules/test_guc_extra_context/meson.build

test_guc_extra_context_sources = files(
  'test_guc_extra_context.c',
)

if host_system == 'windows'
  test_guc_extra_context_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_guc_extra_context',
    '--FILEDESC', 'test_guc_extra_context - test GUC_EXTRA_IS_CONTEXT feature',])
endif

test_guc_extra_context = shared_module('test_guc_extra_context',
  test_guc_extra_context_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_guc_extra_context

test_install_data += files(
  'test_guc_extra_context.control',
  'test_guc_extra_context--1.0.sql',
)

tests += {
  'name': 'test_guc_extra_context',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/test_guc_extra_context.pl',
    ],
  },
}
