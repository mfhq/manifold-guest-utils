/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <systemd/sd-json.h>
#include <systemd/sd-varlink.h>

/*
 * mfhost-run runs a program on the host, for an application that Manifold
 * runs in a mount tree. The guest application can not execute a program of the
 * host itself, as such a program needs the libraries of the host. So we ask
 * the host command runner of Manifold to run it, with our standard input and
 * output, our working directory and our environment, and behave as if we
 * were that program: we pass on the signals we get, and exit like it does.
 *
 * We are called once for every program an application runs on the host, so
 * we do as little as possible: we do not read any file, and leave all
 * decisions about the environment of the program to the runner.
 */

#define PROGRAM_NAME "mfhost-run"

/* the interface of the host command runner, and its socket in the runtime directory of the user */
#define HOST_INTERFACE   "org.freedesktop.manifold.host"
#define HOST_SOCKET_NAME "manifold/" HOST_INTERFACE

#define HOST_METHOD_RUN                      HOST_INTERFACE ".Run"
#define HOST_ERROR_NOT_PERMITTED             HOST_INTERFACE ".NotPermitted"
#define HOST_ERROR_NO_SUCH_PROGRAM           HOST_INTERFACE ".NoSuchProgram"
#define HOST_ERROR_NO_SUCH_WORKING_DIRECTORY HOST_INTERFACE ".NoSuchWorkingDirectory"
#define HOST_ERROR_SPAWN_FAILED              HOST_INTERFACE ".SpawnFailed"

/*
 * The exit statuses we use when we can not run the program, the same ones
 * that shells and env(1) use. Once the program runs, its exit status is ours.
 */
enum {
    MFHR_EXIT_FAILED = 125,         /* we failed ourselves, e.g. the runner can not be reached */
    MFHR_EXIT_CANNOT_EXECUTE = 126, /* the host refused to run the program, or failed to */
    MFHR_EXIT_NOT_FOUND = 127,      /* the host has no such program */
};

/* the signals we pass on to the program */
static const int FORWARDED_SIGNALS[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2, SIGTSTP, SIGCONT};

/* the pipe the runner reads the signals for the program from, which our signal handler writes to */
static int signal_pipe_fd = -1;

/**
 * MfhrResult:
 *
 * How the call to the runner ended.
 */
typedef struct {
    bool done;       /* whether we got the last reply */
    int exit_status; /* our exit status */
    int signal;      /* the signal that ended the program, or 0 */
} MfhrResult;

/**
 * mfhr_print_usage:
 */
static void
mfhr_print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: %s [OPTIONS] <program> [args...]\n"
            "\n"
            "Run a program on the host, from within a Manifold application.\n"
            "\n"
            "Options:\n"
            "  --socket=PATH  Connect to the host command runner at PATH\n"
            "                 (default: $XDG_RUNTIME_DIR/%s)\n"
            "  --help         Show this help\n"
            "  --version      Show the program version\n"
            "\n"
            "Exit status:\n"
            "  The exit status of the program, or\n"
            "  125  if %s failed itself\n"
            "  126  if the host can not run the program\n"
            "  127  if the host has no such program\n",
            PROGRAM_NAME,
            HOST_SOCKET_NAME,
            PROGRAM_NAME);
}

/**
 * mfhr_on_signal:
 *
 * Pass a signal on to the program.
 */
static void
mfhr_on_signal(int signal_number)
{
    int saved_errno = errno;
    uint8_t byte = (uint8_t) signal_number;

    /* the pipe can only be full if the runner does not read it, and then the signal is lost anyway */
    ssize_t written = write(signal_pipe_fd, &byte, 1);
    (void) written;

    /* The program is not a job of the shell we were started from, so the shell does not
     * see it stop. We stop ourselves in its place, and get SIGCONT when the user resumes us. */
    if (signal_number == SIGTSTP)
        raise(SIGSTOP);
    errno = saved_errno;
}

/**
 * mfhr_setup_signals:
 * @ret_read_fd: (out): the end of the signal pipe that the runner gets
 *
 * Set up passing signals on to the program.
 *
 * Returns: 0 on success, or a negative errno
 */
static int
mfhr_setup_signals(int *ret_read_fd)
{
    struct sigaction action = {.sa_handler = mfhr_on_signal, .sa_flags = SA_RESTART};
    int fds[2];

    if (pipe2(fds, O_CLOEXEC) != 0)
        return -errno;
    /* our signal handler must never wait for the runner to read */
    if (fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0)
        return -errno;
    *ret_read_fd = fds[0];
    signal_pipe_fd = fds[1];

    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(FORWARDED_SIGNALS) / sizeof(FORWARDED_SIGNALS[0]); i++) {
        if (sigaction(FORWARDED_SIGNALS[i], &action, nullptr) != 0)
            return -errno;
    }
    return 0;
}

/**
 * mfhr_push_standard_fd:
 *
 * Queue one of our standard file descriptors for the runner. If we were
 * started without it, the program gets /dev/null in its place.
 *
 * Returns: the index of the file descriptor in the call, or a negative errno
 */
static int
mfhr_push_standard_fd(sd_varlink *link, int fd)
{
    int r = sd_varlink_push_dup_fd(link, fd);

    if (r != -EBADF)
        return r;

    int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null_fd < 0)
        return -errno;
    r = sd_varlink_push_fd(link, null_fd);
    if (r < 0)
        close(null_fd);
    return r;
}

/**
 * mfhr_on_reply:
 *
 * Called for every reply of the runner: one when the program was started,
 * and one when it has ended.
 */
static int
mfhr_on_reply(sd_varlink *link,
              sd_json_variant *parameters,
              const char *error_id,
              sd_varlink_reply_flags_t flags,
              void *userdata)
{
    MfhrResult *result = userdata;
    sd_json_variant *value;

    if (error_id == nullptr) {
        if ((flags & SD_VARLINK_REPLY_CONTINUES) != 0)
            return 0;

        value = sd_json_variant_by_key(parameters, "exitStatus");
        if (value != nullptr && sd_json_variant_is_integer(value)) {
            result->exit_status = (int) sd_json_variant_integer(value);
        } else {
            value = sd_json_variant_by_key(parameters, "signal");
            result->signal = value != nullptr && sd_json_variant_is_integer(value)
                                 ? (int) sd_json_variant_integer(value)
                                 : SIGKILL;
        }
        result->done = true;
        return 0;
    }

    result->done = true;
    result->exit_status = MFHR_EXIT_CANNOT_EXECUTE;
    if (strcmp(error_id, HOST_ERROR_NO_SUCH_PROGRAM) == 0) {
        value = sd_json_variant_by_key(parameters, "program");
        fprintf(stderr,
                "%s: The host has no program %s\n",
                PROGRAM_NAME,
                value != nullptr && sd_json_variant_is_string(value) ? sd_json_variant_string(value) : "");
        result->exit_status = MFHR_EXIT_NOT_FOUND;
    } else if (strcmp(error_id, HOST_ERROR_NOT_PERMITTED) == 0) {
        fprintf(stderr,
                "%s: This application does not have the permission to run programs of the host\n",
                PROGRAM_NAME);
    } else if (strcmp(error_id, HOST_ERROR_NO_SUCH_WORKING_DIRECTORY) == 0) {
        value = sd_json_variant_by_key(parameters, "path");
        fprintf(stderr,
                "%s: The current directory %s does not exist on the host\n",
                PROGRAM_NAME,
                value != nullptr && sd_json_variant_is_string(value) ? sd_json_variant_string(value) : "");
    } else if (strcmp(error_id, HOST_ERROR_SPAWN_FAILED) == 0) {
        value = sd_json_variant_by_key(parameters, "message");
        fprintf(stderr,
                "%s: The host is unable to run the program: %s\n",
                PROGRAM_NAME,
                value != nullptr && sd_json_variant_is_string(value) ? sd_json_variant_string(value)
                                                                     : "unknown error");
    } else if (strcmp(error_id, SD_VARLINK_ERROR_DISCONNECTED) == 0) {
        fprintf(stderr, "%s: The host command runner closed the connection\n", PROGRAM_NAME);
        result->exit_status = MFHR_EXIT_FAILED;
    } else {
        fprintf(stderr, "%s: The host command runner failed: %s\n", PROGRAM_NAME, error_id);
        result->exit_status = MFHR_EXIT_FAILED;
    }
    return 0;
}

/**
 * mfhr_run_on_host:
 * @socket_path: the socket of the host command runner
 * @args: the program and its arguments
 * @result: (out): how the program ended
 *
 * Ask the runner to run a program, and wait until the program has ended.
 *
 * Returns: 0 on success, or a negative value once we printed why we failed
 */
static int
mfhr_run_on_host(const char *socket_path, char **args, MfhrResult *result)
{
    extern char **environ;
    char working_directory[PATH_MAX];
    sd_varlink *link = nullptr;
    int signal_read_fd = -1;
    int fd_indices[3];
    int working_directory_fd;
    int working_directory_index;
    int signal_index;
    int r;

    if (getcwd(working_directory, sizeof(working_directory)) == nullptr) {
        r = -errno;
        fprintf(stderr, "%s: Unable to get the current directory: %s\n", PROGRAM_NAME, strerror(-r));
        return r;
    }

    r = sd_varlink_connect_address(&link, socket_path);
    if (r < 0) {
        fprintf(stderr,
                "%s: Unable to reach the host command runner of Manifold at %s: %s\n",
                PROGRAM_NAME,
                socket_path,
                strerror(-r));
        return r;
    }
    r = sd_varlink_set_allow_fd_passing_output(link, true);
    if (r < 0)
        goto fail;

    for (int fd = 0; fd < 3; fd++) {
        fd_indices[fd] = mfhr_push_standard_fd(link, fd);
        if (fd_indices[fd] < 0) {
            r = fd_indices[fd];
            goto fail;
        }
    }

    /* the runner uses this to check that it runs the program in the directory we are in */
    working_directory_fd = open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (working_directory_fd < 0) {
        r = -errno;
        goto fail;
    }
    working_directory_index = sd_varlink_push_fd(link, working_directory_fd);
    if (working_directory_index < 0) {
        close(working_directory_fd);
        r = working_directory_index;
        goto fail;
    }

    r = mfhr_setup_signals(&signal_read_fd);
    if (r < 0)
        goto fail;
    signal_index = sd_varlink_push_fd(link, signal_read_fd);
    if (signal_index < 0) {
        close(signal_read_fd);
        r = signal_index;
        goto fail;
    }

    r = sd_varlink_bind_reply(link, mfhr_on_reply);
    if (r < 0)
        goto fail;
    sd_varlink_set_userdata(link, result);
    r = sd_varlink_observebo(
        link,
        HOST_METHOD_RUN,
        SD_JSON_BUILD_PAIR_STRV("argv", args),
        SD_JSON_BUILD_PAIR_STRING("workingDirectory", working_directory),
        SD_JSON_BUILD_PAIR_STRV("environment", environ),
        SD_JSON_BUILD_PAIR("fileDescriptors",
                           SD_JSON_BUILD_ARRAY(SD_JSON_BUILD_INTEGER(fd_indices[0]),
                                               SD_JSON_BUILD_INTEGER(fd_indices[1]),
                                               SD_JSON_BUILD_INTEGER(fd_indices[2]))),
        SD_JSON_BUILD_PAIR_INTEGER("workingDirectoryFileDescriptor", working_directory_index),
        SD_JSON_BUILD_PAIR_INTEGER("signalFileDescriptor", signal_index));
    if (r < 0)
        goto fail;

    while (!result->done) {
        r = sd_varlink_process(link);
        /* a signal we pass on interrupts us, and is no failure */
        if (r == -EINTR || r > 0)
            continue;
        if (r < 0)
            goto fail;
        r = sd_varlink_wait(link, UINT64_MAX);
        if (r < 0 && r != -EINTR)
            goto fail;
    }

    sd_varlink_close_unref(link);
    return 0;

fail:
    fprintf(stderr, "%s: Unable to talk to the host command runner: %s\n", PROGRAM_NAME, strerror(-r));
    sd_varlink_close_unref(link);
    return r;
}

/**
 * mfhr_end_like_program:
 *
 * End the way the program ended.
 */
static int
mfhr_end_like_program(const MfhrResult *result)
{
    if (result->signal > 0) {
        sigset_t signals;

        /* whoever waits for us has to see that a signal ended the program, so we end by the same one */
        signal(result->signal, SIG_DFL);
        sigemptyset(&signals);
        sigaddset(&signals, result->signal);
        sigprocmask(SIG_UNBLOCK, &signals, nullptr);
        raise(result->signal);
        /* a signal that does not end us */
        return 128 + result->signal;
    }
    return result->exit_status;
}

/**
 * main:
 */
int
main(int argc, char **argv)
{
    static const struct option long_options[] = {
        {"socket",  required_argument, nullptr, 's'},
        {"help",    no_argument,       nullptr, 'h'},
        {"version", no_argument,       nullptr, 'v'},
        {nullptr,   0,                 nullptr, 0  },
    };
    const char *name = argc > 0 ? strrchr(argv[0], '/') : nullptr;
    const char *socket_path = nullptr;
    char default_socket_path[PATH_MAX];
    char **args;
    MfhrResult result = {0};

    name = name != nullptr ? name + 1 : (argc > 0 ? argv[0] : PROGRAM_NAME);
    if (strcmp(name, PROGRAM_NAME) != 0) {
        /* we were called through a link with the name of a program, and all arguments are its own */
        argv[0] = (char *) name;
        args = argv;
    } else {
        int opt;

        /* the leading "+" stops parsing at the first non-option argument */
        while ((opt = getopt_long(argc, argv, "+", long_options, nullptr)) != -1) {
            switch (opt) {
            case 's':
                socket_path = optarg;
                break;
            case 'h':
                mfhr_print_usage(stdout);
                return EXIT_SUCCESS;
            case 'v':
                printf(PROGRAM_NAME " " MFHOST_RUN_VERSION "\n");
                return EXIT_SUCCESS;
            default:
                mfhr_print_usage(stderr);
                return MFHR_EXIT_FAILED;
            }
        }
        if (argc - optind < 1) {
            mfhr_print_usage(stderr);
            return MFHR_EXIT_FAILED;
        }
        args = &argv[optind];
    }

    if (socket_path == nullptr) {
        const char *runtime_dir = getenv("XDG_RUNTIME_DIR");

        if (runtime_dir == nullptr || runtime_dir[0] != '/' ||
            snprintf(default_socket_path,
                     sizeof(default_socket_path),
                     "%s/%s",
                     runtime_dir,
                     HOST_SOCKET_NAME) >= (int) sizeof(default_socket_path)) {
            fprintf(
                stderr,
                "%s: Unable to run %s on the host: there is no user session (XDG_RUNTIME_DIR is not set)\n",
                PROGRAM_NAME,
                args[0]);
            return MFHR_EXIT_FAILED;
        }
        socket_path = default_socket_path;
    }

    if (mfhr_run_on_host(socket_path, args, &result) < 0)
        return MFHR_EXIT_FAILED;
    return mfhr_end_like_program(&result);
}
