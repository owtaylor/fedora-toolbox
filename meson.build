project(
  'toolbox', 'c',
  version: '0.0.6',
  license: 'ASL 2.0',
  meson_version: '>= 0.40.0',
)

go_md2man = find_program('go-md2man')

systemd_dep = dependency('systemd')
tmpfilesdir = systemd_dep.get_pkgconfig_variable('tmpfilesdir')

gnome = import('gnome')
gdbus_src = gnome.gdbus_codegen('toolbox-dbus',
  'src/org.fedoraproject.Toolbox.xml',
  interface_prefix : 'org.fedoraproject.Toolbox',
  namespace : 'Toolbox',
)

giodep = dependency('gio-unix-2.0')
jsondep = dependency('json-glib-1.0')
executable(
  'toolboxd',
  'src/main.c',
  'src/manager.c',
  'src/container.c',
  'src/util.c',
  gdbus_src,
  dependencies: [giodep, jsondep],
  install: true,
  install_dir: get_option('libexecdir'))

fusedep = dependency('fuse')
glibdep = dependency('glib-2.0')
executable(
  'toolbox-envfs',
  'src/envfs.c',
  dependencies: [fusedep, glibdep],
  install: true,
  install_dir: get_option('libexecdir'))

install_data(
  'toolbox',
  install_dir: get_option('bindir'),
)

install_data(
  'toolbox-run',
  install_dir: get_option('libexecdir'),
)

subdir('data')
subdir('doc')
