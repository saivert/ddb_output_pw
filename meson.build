project('ddb_output_pw', 'c',
        version: '0.1',
        default_options: [
          'buildtype=debugoptimized',
          'warning_level=1',
        ],
        license: 'GPLv2.0+')


debug_cflags = []
buildtype = get_option('buildtype')
if buildtype.startswith('debug')
  debug_cflags += '-DDDBPW_DEBUG'
endif

add_project_arguments(debug_cflags, language: 'c')

add_project_arguments('-DPW_ENABLE_DEPRECATED', language: 'c')


cc = meson.get_compiler('c')
if not cc.has_header('deadbeef/deadbeef.h')
  error('missing header: deadbeef.h')
endif

pw_dep = dependency('libpipewire-0.3')

shared_library('ddb_out_pw', 'pw.c', dependencies : [pw_dep], name_prefix: '',
  install: true, install_dir: 'lib/deadbeef')
