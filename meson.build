project('gtk-nm-manager', 'c',
  version: '0.1.0',
  default_options: ['warning_level=2', 'c_std=gnu11'])

gtk4 = dependency('gtk4')
libnm = dependency('libnm')

executable('gtk-nm-manager',
  'src/main.c',
  dependencies: [gtk4, libnm],
  install: true,
  install_dir: get_option('bindir'))
