# Copyright (c) 2022-2026, PostgreSQL Global Development Group

backend_sources += files(
  'assert.c',
  'csvlog.c',
  'elog.c',
  'jsonlog.c',
)

if host_system == 'windows'
  backend_sources += files('nls_mo_reader.c')
endif
