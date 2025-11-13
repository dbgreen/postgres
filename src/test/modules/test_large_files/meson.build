# src/test/modules/test_large_files/meson.build

test_large_files_sources = files(
  'test_large_files.c',
)

if host_system == 'windows'
  test_large_files = shared_module('test_large_files',
    test_large_files_sources,
    kwargs: pg_test_mod_args,
  )
  test_install_libs += test_large_files

  test_install_data += files(
    'test_large_files.control',
    'test_large_files--1.0.sql',
  )

  tests += {
    'name': 'test_large_files',
    'sd': meson.current_source_dir(),
    'bd': meson.current_build_dir(),
    'tap': {
      'tests': [
        't/001_windows_large_files.pl',
      ],
    },
  }
endif
