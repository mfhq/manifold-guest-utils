# manifold-guest-utils

The programs that [Manifold](https://github.com/mfhq/manifold) expects in the
mount tree of an application. A core image installs them into `/usr/bin`, so
every runtime and application built on that core has them.

| Program      | What it does                                                   |
|--------------|----------------------------------------------------------------|
| `mfhost-run` | Runs a program of the host.                                    |
| `xdg-open`   | Opens a file or URL with the application the host uses for it. |
| `xdg-email`  | Composes an e-mail in the mail client of the host.             |

## mfhost-run

An application that Manifold runs lives in a mount tree made of its runtime
images. It can see the programs of the host, but it can not execute them, as
they need the libraries of the host. `mfhost-run` asks the host command runner
of Manifold to run such a program, and then behaves as if it were that
program: the program gets its standard input and output, its working directory
and its environment, signals are passed on, and `mfhost-run` exits with the
status of the program.

```sh
mfhost-run gcc --version
```

Applications rarely have to call it by name. Manifold gives every application
with the `host-commands` permission a directory of links to `mfhost-run` at
the end of its `PATH`, one for each program of the host. Called through such a
link, `mfhost-run` runs the program of that name, so `gcc` just works if the
runtime of the application does not have a `gcc` of its own.

The program runs in the session of the user and not on the terminal of the
application. Its output, colours and Ctrl-C work as usual, but it has no
controlling terminal, so password prompts of `sudo` or `ssh` and the job
control of a shell do not work.

The working directory has to exist on the host. A directory that only the
application has, like its private `/tmp`, is refused.

## xdg-open and xdg-email

The `xdg-open` and `xdg-email` of xdg-utils look for a browser or a mail
client among the programs they can see, which in a mount tree are the ones of
the runtime, and not the ones the user has set up. The two programs here ask
the desktop portals of the host instead (`org.freedesktop.portal.OpenURI` and
`org.freedesktop.portal.Email`), so the user gets the applications of the
host. They are taken verbatim from
[flatpak-xdg-utils](https://github.com/flatpak/flatpak-xdg-utils) and accept
the options of the originals.

## Building

`mfhost-run` needs libsystemd 257 or later, the other two need GLib:

```sh
meson setup build
ninja -C build
meson test -C build
```
