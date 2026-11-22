#
# mfhost-run: Runs a program of the host
#
mfhost_run_exe = executable('mfhost-run',
    ['mfhost-run.c'],
    c_args: version_c_args,
    dependencies: [libsystemd_dep],
    install: true,
    install_dir: get_option('bindir'),
)

#
# xdg-open and xdg-email from flatpak-xdg-utils, which ask the desktop portals
# of the host instead of looking for a browser or mail client in the mount tree
#
# upstream builds them with a config.h that defines this
xdg_utils_c_args = version_c_args + ['-D_GNU_SOURCE']

xdg_open_exe = executable('xdg-open',
    ['xdg-open.c'],
    c_args: xdg_utils_c_args,
    dependencies: [gio_unix_dep],
    install: true,
    install_dir: get_option('bindir'),
)

xdg_email_exe = executable('xdg-email',
    ['xdg-email.c'],
    c_args: xdg_utils_c_args,
    dependencies: [gio_unix_dep],
    install: true,
    install_dir: get_option('bindir'),
)
