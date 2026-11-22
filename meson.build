project('manifold-guest-utils', 'c',
  meson_version: '>=1.4',
  default_options: ['c_std=gnu23', 'b_pie=true', 'warning_level=2'],

  license: 'LGPL-2.1-or-later',
  version: '0.0.1',
)

cc = meson.get_compiler('c')

add_project_arguments(
  cc.get_supported_arguments([
    '-Werror=shadow',
    '-Werror=strict-prototypes',
    '-Werror=missing-prototypes',
    '-Werror=implicit-function-declaration',
    '-Werror=return-type',
    '-Werror=int-conversion',
    '-Werror=incompatible-pointer-types',
    '-Werror=format-security',
    '-Wno-unused-parameter',
  ]),
  language: 'c',
)

version_c_args = ['-DPACKAGE_VERSION="@0@"'.format(meson.project_version())]

# sd-varlink is part of libsystemd since version 257
libsystemd_dep = dependency('libsystemd', version: '>= 257')
gio_unix_dep = dependency('gio-unix-2.0', version: '>= 2.44')

subdir('src')
subdir('tests')
