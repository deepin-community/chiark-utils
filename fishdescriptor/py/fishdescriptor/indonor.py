
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

# class for use inside gdb which is debugging the donor process

from __future__ import print_function

import gdb
import copy
import os
import sys
import socket
import errno

def _string_bytearray(s):
    # gets us bytes in py2 and py3
    if not isinstance(s, bytes):
        s = s.encode('utf-8') # sigh, python 2/3 compat
    return bytearray(s)

def _string_escape_for_c(s):
    out = ''
    for c in _string_bytearray(s):
        if c == ord('\\') or c == ord('"') or c < 32 or c > 126:
            out += '\\x%02x' % c
        else:
            out += chr(c)
    return out

# constructing values

def _lit_integer(v):
    return '%d' % v

def _lit_aggregate_uncasted(val_lit_strs):
    return '{' + ', '.join(['(%s)' % v for v in val_lit_strs]) + ' }'

def _lit_string_uncasted(s):
    b = _string_bytearray(s)
    return _lit_aggregate_uncasted([_lit_integer(x) for x in b] + [ '0' ])

def _lit_array(elemtype, val_lit_strs):
    return (
        '((%s[%d])%s)' %
        (elemtype, len(val_lit_strs), _lit_aggregate_uncasted(val_lit_strs))
    )

def _lit_addressof(v):
    return '&(char[])(%s)' % v

def _make_lit(v):
    if isinstance(v, int):
        return _lit_integer(v)
    else:
        return v # should already be an integer

def parse_eval(expr):
    sys.stderr.write("##  EVAL %s\n" % repr(expr))
    x = gdb.parse_and_eval(expr)
    sys.stderr.write('##  => %s\n' % x)
    sys.stderr.flush()
    return x

class DonorStructLayout():
    def __init__(l, typename):
        x = gdb.lookup_type(typename)
        l._typename = typename
        l._template = [ ]
        l._posns = { }
        for f in x.fields():
            l._posns[f.name] = len(l._template)
            try: f.type.fields();  blank = '{ }'
            except TypeError:      blank = '0'
            except AttributeError: blank = '0'
            l._template.append(blank)
        sys.stderr.write('##  STRUCT %s template %s fields %s\n'
                         % (typename, l._template, l._posns))

    def substitute(l, values):
        build = copy.deepcopy(l._template)
        for (k,v) in values.items():
            build[ l._posns[k] ] = _make_lit(v)
        return '((%s)%s)' % (l._typename, _lit_aggregate_uncasted(build))

class DonorImplementation():
    def __init__(di):
        di._structs = { }
        di._saved_errno = None
        di._result_stream = os.fdopen(3, 'w')
        di._errno_workaround = None

    # assembling structs
    # sigh, we have to record the order of the arguments!
    def _find_fields(di, typename):
        try:
            fields = di._structs[typename]
        except KeyError:
            fields = DonorStructLayout(typename)
            di._structs[typename] = fields
        return fields

    def _make(di, typename, values):
        fields = di._find_fields(typename)
        return fields.substitute(values)

    # hideous workaround

    def _parse_eval_errno(di, expr_pat):
        # evaluates  expr_pat % 'errno'
        if di._errno_workaround is not True:
            try:
                x = parse_eval(expr_pat % 'errno')
                di._errno_workaround = False
                return x
            except gdb.error as e:
                if di._errno_workaround is False:
                    raise e
            di._errno_workaround = True
        # Incomprehensibly, gdb.parse_and_eval('errno') can sometimes
        # fail with
        #   gdb.error: Cannot find thread-local variables on this target
        # even though plain gdb `print errno' works while `print errno = 25'
        # doesn't.   OMG.  This may be related to:
        #  https://github.com/cloudburst/libheap/issues/24
        # although I can't find it in the gdb bug db (which is half-broken
        # in my browser).  Also the error is very nonspecific :-/.
        # This seems to happen on jessie, and is fixed in stretch.
        # Anyway:
        return parse_eval(expr_pat % '(*((int*(*)(void))__errno_location)())')

    # calling functions (need to cast the function name to the right
    # type in case maybe gdb doesn't know the type)

    def _func(di, functype, funcname, realargs):
        expr = '((%s) %s) %s' % (functype, funcname, realargs)
        return parse_eval(expr)

    def _must_func(di, functype, funcname, realargs):
        retval = di._func(functype, funcname, realargs)
        if retval < 0:
            errnoval = di._parse_eval_errno('%s')
            raise RuntimeError("%s gave errno=%d `%s'" %
                               (funcname, errnoval, os.strerror(errnoval)))
        return retval

    # wrappers for the syscalls that do what we want

    def _sendmsg(di, carrier, control_msg):
        iov_base = _lit_array('char', [1])
        iov = di._make('struct iovec', {
            'iov_base': iov_base,
            'iov_len' : 1,
        })

        msg = di._make('struct msghdr', {
            'msg_iov'       : _lit_addressof(iov),
            'msg_iovlen'    : 1,
            'msg_control'   : _lit_array('char', control_msg),
            'msg_controllen': len(control_msg),
        })

        di._must_func(
            'ssize_t (*)(int, const struct msghdr*, int)',
            'sendmsg',
            '(%s, %s, 0)' % (carrier, _lit_addressof(msg))
        )

    def _socket(di):
        return di._must_func(
            'int (*)(int, int, int)',
            'socket',
            '(%d, %d, 0)' % (socket.AF_UNIX, socket.SOCK_STREAM)
        )

    def _connect(di, fd, path):
        addr = di._make('struct sockaddr_un', {
            'sun_family' : _lit_integer(socket.AF_UNIX),
            'sun_path'   : _lit_string_uncasted(path),
        })

        di._must_func(
            'int (*)(int, const struct sockaddr*, socklen_t)',
            'connect',
            '(%d, (const struct sockaddr*)%s, sizeof(struct sockaddr_un))'
            % (fd, _lit_addressof(addr))
        )

    def _close(di, fd):
        di._must_func('int (*)(int)', 'close', '(%d)' % fd)

    def _mkdir(di, path, mode):
        r = di._func(
            'int (*)(const char*, mode_t)',
            'mkdir',
            '("%s", %d)' % (_string_escape_for_c(path), mode)
        )
        if r < 0:
            errnoval = di._parse_eval_errno('%s')
            if errnoval != errno.EEXIST:
                raise RuntimeError("mkdir %s failed: `%s'" %
                                   (repr(path), os.strerror(errnoval)))
            return 0
        return 1

    def _errno_save(di):
        di._saved_errno = di._parse_eval_errno('%s')

    def _errno_restore(di):
        to_restore = di._saved_errno
        di._saved_errno = None
        if to_restore is not None:
            di._parse_eval_errno('%%s = %d' % to_restore)

    def _result(di, output):
        sys.stderr.write("#> %s" % output)
        di._result_stream.write(output)
        di._result_stream.flush()

    # main entrypoints

    def donate(di, path, control_msg):
        # control_msg is an array of integers being the ancillary data
        # array ("control") for sendmsg, and hence specifies which fds
        # to pass

        carrier = None
        try:
            di._errno_save()
            carrier = di._socket()
            di._connect(carrier, path)
            di._sendmsg(carrier, control_msg)
            di._close(carrier)
            carrier = None
        finally:
            if carrier is not None:
                try: di._close(carrier)
                except Exception: pass
            di._errno_restore()

        di._result('1\n')

    def geteuid(di):
        try:
            di._errno_save()
            val = di._must_func('uid_t (*)(void)', 'geteuid', '()')
        finally:
            di._errno_restore()
        
        di._result('%d\n' % val)

    def mkdir(di, path):
        try:
            di._errno_save()
            val = di._mkdir(path, int('0700', 8))
        finally:
            di._errno_restore()

        di._result('%d\n' % val)

    def _protocol_read(di):
        input = sys.stdin.readline()
        if input == '': return None
        input = input.rstrip('\n')
        sys.stderr.write("#< %s\n" % input)
        return input

    def eval_loop(di):
        if not gdb.selected_inferior().was_attached:
            print('gdb inferior not attached', file=sys.stderr)
            sys.exit(0)
        while True:
            di._result('!\n')
            cmd = di._protocol_read()
            if cmd is None: break
            eval(cmd)
