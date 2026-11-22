#!/usr/bin/env python3
#
# Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
Tests of mfhost-run. The host command runner of Manifold is played by this
test, which answers the calls of mfhost-run on a socket of its own.

Usage: test-mfhost-run.py --program mfhost-run
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

INTERFACE = 'org.freedesktop.manifold.host'

ARGS = None


class FakeRunner(threading.Thread):
    """Answers one call of Run like manifold-hostrun does. What it does for a
    call depends on the program it is asked to run."""

    def __init__(self, socket_path):
        super().__init__(daemon=True)
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(socket_path)
        self.listener.listen()
        self.parameters = None
        self.cwd_matches = None
        self.error = None

    def run(self):
        try:
            connection, _ = self.listener.accept()
            with connection:
                self.serve(connection)
        except Exception as e:  # pylint: disable=broad-except
            self.error = e

    def reply(self, connection, parameters=None, error=None, continues=False):
        message = {'parameters': parameters or {}}
        if error is not None:
            message['error'] = f'{INTERFACE}.{error}'
        if continues:
            message['continues'] = True
        connection.sendall(json.dumps(message).encode() + b'\0')

    def serve(self, connection):
        data, fds, _, _ = socket.recv_fds(connection, 1024 * 1024, 16)
        while not data.endswith(b'\0'):
            data += connection.recv(1024 * 1024)
        call = json.loads(data.rstrip(b'\0'))
        assert call['method'] == f'{INTERFACE}.Run', call
        assert call['more'], call
        self.parameters = p = call['parameters']
        program = p['argv'][0]
        output_fd = fds[p['fileDescriptors'][1]]
        cwd_fd = fds[p['workingDirectoryFileDescriptor']]
        signal_fd = fds[p['signalFileDescriptor']]
        self.cwd_matches = os.path.samestat(os.stat(p['workingDirectory']), os.fstat(cwd_fd))

        if program == 'missing':
            self.reply(connection, {'program': program}, error='NoSuchProgram')
        elif program == 'forbidden':
            self.reply(connection, {'componentId': 'com.example.app'}, error='NotPermitted')
        elif program == 'wait-for-signal':
            self.reply(connection, {'pid': 1234}, continues=True)
            os.write(output_fd, b'started\n')
            self.reply(connection, {'signal': os.read(signal_fd, 1)[0]})
        else:
            self.reply(connection, {'pid': 1234}, continues=True)
            os.write(output_fd, os.read(fds[p['fileDescriptors'][0]], 4096))
            self.reply(connection, {'exitStatus': 3})
        for fd in fds:
            os.close(fd)


class MfhostRunTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.mkdtemp(prefix='mfhost-run-test-')
        self.addCleanup(shutil.rmtree, self.work)
        self.socket_path = f'{self.work}/manifold/{INTERFACE}'
        os.makedirs(os.path.dirname(self.socket_path))
        self.runner = FakeRunner(self.socket_path)
        self.runner.start()
        self.addCleanup(self.runner.listener.close)
        # the socket is found in the runtime directory of the user
        self.environment = dict(os.environ, XDG_RUNTIME_DIR=self.work, MF_TEST_VAR='hello')

    def run_program(self, *argv, program=None, **kwargs):
        result = subprocess.run(
            [program or ARGS.program, *argv],
            capture_output=True,
            env=self.environment,
            check=False,
            **kwargs,
        )
        self.runner.join(timeout=10)
        if self.runner.error is not None:
            raise self.runner.error
        return result

    def test_program_runs_with_our_files(self):
        result = self.run_program('tool', '--flag', 'argument', input=b'from stdin', cwd=self.work)
        self.assertEqual(result.returncode, 3, result)
        self.assertEqual(result.stdout, b'from stdin')
        self.assertEqual(self.runner.parameters['argv'], ['tool', '--flag', 'argument'])
        self.assertEqual(self.runner.parameters['workingDirectory'], os.path.realpath(self.work))
        self.assertTrue(self.runner.cwd_matches)
        self.assertIn('MF_TEST_VAR=hello', self.runner.parameters['environment'])

    def test_called_by_the_name_of_a_program(self):
        """A link to us with the name of a program runs that program, with all arguments."""
        link = f'{self.work}/tool'
        os.symlink(ARGS.program, link)
        result = self.run_program('--help', '--socket=x', program=link, input=b'')
        self.assertEqual(result.returncode, 3, result)
        self.assertEqual(self.runner.parameters['argv'], ['tool', '--help', '--socket=x'])

    def test_no_such_program(self):
        result = self.run_program('missing')
        self.assertEqual(result.returncode, 127, result)
        self.assertIn(b'The host has no program missing', result.stderr)

    def test_not_permitted(self):
        result = self.run_program('forbidden')
        self.assertEqual(result.returncode, 126, result)
        self.assertIn(b'does not have the permission', result.stderr)

    def test_signals_are_passed_on(self):
        """A signal we get is sent to the program, and we end by the signal that ended it."""
        process = subprocess.Popen(
            [ARGS.program, 'wait-for-signal'], stdout=subprocess.PIPE, env=self.environment
        )
        self.assertEqual(process.stdout.readline(), b'started\n')
        process.send_signal(signal.SIGTERM)
        self.assertEqual(process.wait(timeout=10), -signal.SIGTERM)
        process.stdout.close()
        self.runner.join(timeout=10)
        self.assertIsNone(self.runner.error)

    def test_no_runner(self):
        self.environment['XDG_RUNTIME_DIR'] = f'{self.work}/missing'
        result = subprocess.run(
            [ARGS.program, 'tool'], capture_output=True, env=self.environment, check=False
        )
        self.assertEqual(result.returncode, 125, result)
        self.assertIn(b'Unable to reach the host command runner', result.stderr)

        del self.environment['XDG_RUNTIME_DIR']
        result = subprocess.run(
            [ARGS.program, 'tool'], capture_output=True, env=self.environment, check=False
        )
        self.assertEqual(result.returncode, 125, result)
        self.assertIn(b'there is no user session', result.stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--program', required=True)
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
