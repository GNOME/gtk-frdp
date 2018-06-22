project('gtk-frdp',
  ['c', 'vala'],
  version: '0.1.0',
  meson_version: '>= 0.40.0',
)


config_h = configuration_data()

configure_file(
  output: 'gtk-frdp-config.h',
  configuration: config_h,
)
add_project_arguments([
  '-I' + meson.build_root(),
], language: 'c')

package_subdir = get_option('package_subdir')
datadir = join_paths (package_subdir, get_option('datadir'))
libdir = join_paths(package_subdir, get_option('libdir'))
girdir = join_paths(datadir, 'gir-1.0')
typelibdir = join_paths(libdir, 'girepository-1.0')
if package_subdir != ''
  vapidir = join_paths(datadir, 'vapi')
else
  vapidir = join_paths(datadir, 'vala', 'vapi')
endif

gnome = import('gnome')

subdir('src')
subdir('examples')


