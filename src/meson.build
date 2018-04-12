api_version = '0.1'

gtk_frdp_sources = [
  'frdp-display.c',
]

gtk_frdp_headers = [
  'frdp-display.h',
  'gtk-frdp.h',
]

version_split = meson.project_version().split('.')
MAJOR_VERSION = version_split[0]
MINOR_VERSION = version_split[1]
MICRO_VERSION = version_split[2]

version_conf = configuration_data()
version_conf.set('VERSION', meson.project_version())
version_conf.set('MAJOR_VERSION', MAJOR_VERSION)
version_conf.set('MINOR_VERSION', MINOR_VERSION)
version_conf.set('MICRO_VERSION', MICRO_VERSION)

configure_file(
  input: 'gtk-frdp-version.h.in',
  output: 'gtk-frdp-version.h',
  configuration: version_conf,
  install: true,
  install_dir: join_paths(get_option('includedir'), 'gtk-frdp')
)

gtk_frdp_deps = [
  dependency('freerdp', version: '>= 1.1'),
  dependency('gio-2.0', version: '>= 2.50'),
  dependency('gtk+-3.0'),
]

gtk_frdp_lib = shared_library('gtk-frdp-' + api_version,
  gtk_frdp_sources,
  dependencies: gtk_frdp_deps,
  install: true,
)

gtk_frdp_dep = declare_dependency(
  sources: gtk_frdp_headers,
  dependencies: gtk_frdp_deps,
  link_with: gtk_frdp_lib,
  include_directories: include_directories('.'),
)

install_headers(gtk_frdp_headers, subdir: 'gtk-frdp')

pkg = import('pkgconfig')

pkg.generate(
  description: 'A shared library for ...',
    libraries: gtk_frdp_lib,
         name: 'gtk-frdp',
     filebase: 'gtk-frdp-' + api_version,
      version: meson.project_version(),
      subdirs: 'gtk-frdp',
     requires: 'glib-2.0',
  install_dir: join_paths(get_option('libdir'), 'pkgconfig')
)
