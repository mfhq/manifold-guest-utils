python_exe = find_program('python3', required: false)
if python_exe.found()
    test('mfhost-run',
        python_exe,
        args: [files('test-mfhost-run.py'), '--program', mfhost_run_exe.full_path()],
        depends: [mfhost_run_exe],
        timeout: 60,
    )
endif

#
# The tests of xdg-open and xdg-email from flatpak-xdg-utils. They play the
# desktop portals on a session bus of their own, which needs dbus-daemon.
#
xdg_utils_test_env = environment()
xdg_utils_test_env.set('G_DEBUG', 'gc-friendly,fatal-criticals')
xdg_utils_test_env.set('GIO_USE_VFS', 'local')
xdg_utils_test_env.set('XDG_OPEN', xdg_open_exe.full_path())
xdg_utils_test_env.set('XDG_EMAIL', xdg_email_exe.full_path())

# upstream's config.h also has the directory of the installed programs, which the
# tests use if they are not told where the programs are. We keep the imported
# tests as they are, so one function without a prototype may not fail the build.
xdg_utils_test_c_args = xdg_utils_c_args + [
    '-DBINDIR="@0@"'.format(get_option('prefix') / get_option('bindir')),
    '-Wno-error=missing-prototypes',
]

foreach test_name : ['test-open', 'test-email']
    test_exe = executable(test_name,
        [test_name + '.c', 'common.c', 'common.h'],
        c_args: xdg_utils_test_c_args,
        dependencies: [gio_unix_dep],
    )
    test(test_name,
        test_exe,
        args: ['--tap'],
        env: xdg_utils_test_env,
        depends: [xdg_open_exe, xdg_email_exe],
        protocol: 'tap',
        timeout: 60,
    )
endforeach
