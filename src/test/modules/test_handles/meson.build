# src/test/modules/test_handles/meson.build

if host_system == 'windows'
  handle_info = executable('handle_info',
    'handle_info.c',
    dependencies: [frontend_code],
    kwargs: default_bin_args + {
      'install': false,
    },
  )

  tests += {
    'name': 'test_handles',
    'sd': meson.current_source_dir(),
    'bd': meson.current_build_dir(),
    'tap': {
      'tests': [
        't/001_shared_memory_inheritance.pl',
      ],
      'env': {
        'HANDLE_INFO': handle_info.full_path(),
      },
    },
  }
endif