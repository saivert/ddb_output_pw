project('ddb_output_pw', 'c',
        version: '0.1',
        default_options: [
          'buildtype=debugoptimized',
          'warning_level=1',
        ],
        license: 'GPLv2.0+')

conf_data = configuration_data()
conf_data.set('DDBPW_VERSION_MAJOR', '0')
conf_data.set('DDBPW_VERSION_MINOR', '1')


buildtype = get_option('buildtype')
if buildtype.startswith('debug')
  conf_data.set('DDBPW_DEBUG', true)
endif

cc = meson.get_compiler('c')
if not cc.has_header('deadbeef/deadbeef.h')
  error('missing header: deadbeef.h')
endif

pw_dep = dependency('libpipewire-0.3', version: '>=0.3.15')
pw_dep_19 = dependency('libpipewire-0.3', version: '>=0.3.19', required : false)

if pw_dep_19.found()
  conf_data.set('HAVE_UPDATE_STRING', true)
endif

configure_file(output : 'config.h',
               configuration : conf_data)

add_project_arguments('-DHAVE_CONFIG_H', language: 'c')

shared_library('ddb_out_pw', 'pw.c', dependencies : [pw_dep], name_prefix: '',
  install: true, install_dir: 'lib/deadbeef')
