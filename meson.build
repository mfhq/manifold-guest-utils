project('mfhost-run', 'c',
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

# sd-varlink is part of libsystemd since version 257
libsystemd_dep = dependency('libsystemd', version: '>= 257')

mfhost_run_exe = executable('mfhost-run',
    ['mfhost-run.c'],
    c_args: ['-DMFHOST_RUN_VERSION="@0@"'.format(meson.project_version())],
    dependencies: [libsystemd_dep],
    install: true,
    install_dir: get_option('bindir'),
)

python_exe = find_program('python3', required: false)
if python_exe.found()
    test('mfhost-run',
        python_exe,
        args: [files('tests/test-mfhost-run.py'), '--program', mfhost_run_exe.full_path()],
        depends: [mfhost_run_exe],
        timeout: 60,
    )
endif
