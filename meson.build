project('gtk-frdp',
  ['c', 'vala'],
  version: '0.2.0',
  meson_version: '>= 0.50.0',
)


config_h = configuration_data()

configure_file(
  output: 'gtk-frdp-config.h',
  configuration: config_h,
)
add_project_arguments([
  '-I' + meson.current_build_dir(),
], language: 'c')

package_subdir = get_option('package_subdir')
libdir = join_paths(get_option('libdir'), package_subdir)
girdir = join_paths(get_option('datadir'), package_subdir, 'gir-1.0')
typelibdir = join_paths(libdir, 'girepository-1.0')
if package_subdir != ''
  vapidir = join_paths(get_option('datadir'), package_subdir, 'vapi')
else
  vapidir = join_paths(get_option('datadir'), 'vala', 'vapi')
endif

gnome = import('gnome')

subdir('src')

if get_option('examples')
  subdir('examples')
endif
