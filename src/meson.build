api_version = '0.2'

gtk_frdp_public_sources = [
  'frdp-display.c',
  'frdp-session.c'
]

gtk_frdp_private_sources = [
  'frdp-channel.c',
  'frdp-channel-display-control.c',
  'frdp-channel-clipboard.c'
]

gtk_frdp_public_headers = [
  'frdp-display.h',
  'frdp-session.h',
  'gtk-frdp.h'
]

gtk_frdp_private_headers = [
  'frdp-channel.h',
  'frdp-channel-display-control.h',
  'frdp-channel-clipboard.h',
  'frdp-context.h'
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

gtk_frdp_header_subdir = join_paths(package_subdir, 'gtk-frdp')
gtk_frdp_header_dir = join_paths(get_option('includedir'), gtk_frdp_header_subdir)

configure_file(
  input: 'gtk-frdp-version.h.in',
  output: 'gtk-frdp-version.h',
  configuration: version_conf,
  install: true,
  install_dir: gtk_frdp_header_dir
)

cc = meson.get_compiler('c')
vala = meson.get_compiler('vala')

gtk_frdp_deps = [
  dependency('gio-2.0', version: '>= 2.50'),
  dependency('gtk+-3.0'),
  dependency('fuse3', version: '>= 3.9.1'),
  cc.find_library('m'),
]

# Keep compatibility with FreeRDP 2 for next few releases
freerdp3_dep = dependency('freerdp3', required: false)
if freerdp3_dep.found()
  add_project_arguments('-DHAVE_FREERDP3', language: 'c')
  if freerdp3_dep.version().version_compare('>= 3.11.0')
    add_project_arguments('-DHAVE_FREERDP_3_11_0', language: 'c')
  endif
  gtk_frdp_deps += freerdp3_dep
  gtk_frdp_deps += dependency('winpr3', version: '>= 3.0.0')
  gtk_frdp_deps += dependency('freerdp-client3')
else
  # The 2.0.0-rc4 version is needed at least, but there is no easy way to detect this.
  freerdp2_dep = dependency('freerdp2', version: '>= 2.0.0')
  if freerdp2_dep.found()
    gtk_frdp_deps += freerdp2_dep
    gtk_frdp_deps += dependency('winpr2', version: '>= 2.0.0')
    gtk_frdp_deps += dependency('freerdp-client2')
  endif
endif

gtk_frdp_lib = shared_library('gtk-frdp-' + api_version,
  gtk_frdp_public_sources + gtk_frdp_private_sources,
  dependencies: gtk_frdp_deps,
  install: true,
  install_dir: libdir
)

gtk_frdp_dep = declare_dependency(
  sources: gtk_frdp_public_headers,
  dependencies: gtk_frdp_deps,
  link_with: gtk_frdp_lib,
  include_directories: include_directories('.'),
)

install_headers(gtk_frdp_public_headers, subdir: gtk_frdp_header_subdir)

gtk_frdp_gir = gnome.generate_gir(gtk_frdp_lib,
  sources: gtk_frdp_public_sources + gtk_frdp_public_headers,
  nsversion: api_version,
  namespace: 'GtkFrdp',
  symbol_prefix: 'frdp',
  identifier_prefix: 'Frdp',
  link_with: gtk_frdp_lib,
  includes: ['Gio-2.0', 'Gtk-3.0'],
  install: true,
  install_dir_gir: girdir,
  install_dir_typelib: typelibdir,
  extra_args: [ '--c-include=gtk-frdp.h', '--quiet' ],
)

gtk_frdp_vapi = gnome.generate_vapi('gtk-frdp-' + api_version,
  sources: gtk_frdp_gir[0],
  packages: [ 'gio-2.0', 'gtk+-3.0' ],
  install: true,
  install_dir: vapidir,
  metadata_dirs: [ meson.current_source_dir() ],
)

pkg = import('pkgconfig')

pkg.generate(
  description: 'A shared library for ...',
    libraries: gtk_frdp_lib,
         name: 'gtk-frdp',
     filebase: 'gtk-frdp-' + api_version,
      version: meson.project_version(),
      subdirs: gtk_frdp_header_subdir,
     requires: 'glib-2.0',
  install_dir: join_paths(libdir, 'pkgconfig')
)
