# fish.py

# This file is part of chiark-utils, a collection of useful programs
# used on chiark.greenend.org.uk.
#
# This file is:
#  Copyright 2018 Citrix Systems Ltd
#
# This is free software; you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation; either version 3, or (at your option) any later version.
#
# This is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, consult the Free Software Foundation's
# website at www.fsf.org, or the GNU Project website at www.gnu.org.

# python 3 only

import socket
import subprocess
import os
import pwd
import struct
import tempfile
import shutil
import sys
import errno

def _shuffle_fd3():
    os.dup2(1,3)
    os.dup2(2,1)

class Error(Exception): pass

class Donor():
    def __init__(d, pid, debug=None):
        d.pid = pid
        if debug is None:
            d._stderr = tempfile.TemporaryFile(mode='w+')
        else:
            d._stderr = None
        d._sp = subprocess.Popen(
            preexec_fn = _shuffle_fd3,
            stdin = subprocess.PIPE,
            stdout = subprocess.PIPE,
            stderr = d._stderr,
            close_fds = False,
            args = ['gdb', '-p', str(pid), '-batch', '-ex',
                    'python import fishdescriptor.indonor as id;'+
                    ' id.DonorImplementation().eval_loop()'
                ]
        )            

    def _eval_integer(d, expr):
        try:
            l = d._sp.stdout.readline()
            if not len(l): raise Error('gdb process donor python repl quit')
            if l != b'!\n': raise RuntimeError("indonor said %s" % repr(l))
            d._sp.stdin.write(expr.encode('utf-8') + b'\n')
            d._sp.stdin.flush()
            l = d._sp.stdout.readline().rstrip(b'\n')
            return int(l)
        except Exception as e:
            if d._stderr is not None:
                d._stderr.seek(0)
                shutil.copyfileobj(d._stderr, sys.stderr)
                d._stderr.seek(0)
                d._stderr.truncate()
            raise e

    def _eval_success(d, expr):
        r = d._eval_integer(expr)
        if r != 1: raise RuntimeError("eval of %s gave %d" % (expr, r))

    def _geteuid(d):
        return d._eval_integer('di.geteuid()')

    def _ancilmsg(d, fds):
        perl_script = '''
            use strict;
            use Socket;
            use Socket::MsgHdr;
            my $fds = pack "i*", @ARGV;
            my $m = Socket::MsgHdr::pack_cmsghdr SOL_SOCKET, SCM_RIGHTS, $fds;
            print join ", ", unpack "C*", $m;
        '''
        ap = subprocess.Popen(
            stdin = subprocess.DEVNULL,
            stdout = subprocess.PIPE,
            args = ['perl','-we',perl_script] + [str(x) for x in fds]
        )
        (output, dummy) = ap.communicate()
        return output.decode('utf-8')

    def donate(d, path, fds):
        ancil = d._ancilmsg(fds)
        d._eval_success('di.donate(%s, [ %s ])'
                        % (repr(path), ancil))
        return len(ancil.split(','))

    def mkdir(d, path):
        d._eval_integer('di.mkdir(%s)'
                        % (repr(path)))

    def _exists(d, path):
        try:
            os.stat(path)
            return True
        except OSError as oe:
            if oe.errno != errno.ENOENT: raise oe
            return False

    def _sock_dir(d, target_euid, target_root):
        run_dir = '/run/user/%d' % target_euid
        if d._exists(target_root + run_dir):
            return run_dir + '/fishdescriptor'

        try:
            pw = pwd.getpwuid(target_euid)
            return pw.pw_dir + '/.fishdescriptor'
        except KeyError:
            pass

        raise RuntimeError(
 'cannot find good socket path - no /run/user/UID nor pw entry for target process euid %d'
            % target_euid
        )

    def fish(d, fds):
        # -> list of fds in our process

        target_root = '/proc/%d/root' % d.pid
        if not d._exists(target_root):
            target_root = ''

        euid = d._geteuid()
        sockdir = d._sock_dir(euid, target_root)
        d.mkdir(sockdir)

        sockname = '%s/%s,%d' % (sockdir, os.uname().nodename, d.pid)

        our_sockname = target_root + sockname

        s = None
        s2 = None

        try:
            try: os.remove(our_sockname)
            except FileNotFoundError: pass

            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.bind(our_sockname)
            os.chmod(our_sockname, 666)
            s.listen(1)

            ancil_len = d.donate(sockname, fds)
            (s2, dummy) = s.accept()
            (msg, ancil, flags, sender) = s2.recvmsg(1, ancil_len)

            got_fds = None
            unpack_fmt = '%di' % len(fds)

            for clvl, ctype, cdata in ancil:
                if clvl == socket.SOL_SOCKET and ctype == socket.SCM_RIGHTS:
                    assert(got_fds is None)
                    got_fds = struct.unpack_from(unpack_fmt, cdata)

        finally:
            if s is not None: s.close()
            if s2 is not None: s2.close()

            try: os.remove(our_sockname)
            except FileNotFoundError: pass

        return list(got_fds)

    def detach(d):
        d._sp.stdin.close()
