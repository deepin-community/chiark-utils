# Copyright 2022 Ian Jackson and contributors to chiark-utils
# SPDX-License-Identifier: GPL-3.0-or-later
# There is NO WARRANTY.

package Proc::Prefork::Interp;
require Exporter;
our @ISA = qw(Exporter);
our @EXPORT = qw(
		  prefork_initialisation_complete 
		  prefork_autoreload_also_check
	       );

use strict;

use Carp;
use Fcntl qw(F_GETFL F_SETFL O_NONBLOCK);
use IO::FDPass;
use POSIX qw(_exit setsid :sys_wait_h :errno_h :signal_h);
use Sys::Syslog qw(openlog syslog LOG_INFO LOG_ERR LOG_WARNING);
use Time::HiRes qw();

our $logger;

our $env_name = 'PREFORK_INTERP';

our @call_fds;
our $socket_path;
our $fail_log = 0;
our $startup_mtime;

our @autoreload_extra_files = ();

sub prefork_autoreload_also_check {
  push @autoreload_extra_files, @_;
}

sub fail_log ($) {
  my ($m) = @_;
  if ($fail_log) {
    syslog(LOG_ERR, "$0: prefork: error: $m");
  } else {
    carp "$0: prefork: initialisation error: $m";
  }
  _exit 127;
}

sub server_quit ($) {
  my ($m) = @_;
  syslog(LOG_INFO, "$0 prefork: $m, quitting");
  _exit(0);
}

# Returns in the executor process
sub become_monitor () {
  close LISTEN;
  close WATCHI;
  close WATCHE;

  # Make a process group for this call
  setpgrp or fail_log("setpgrp failed: $!");

  eval { protocol_exchange(); 1; }
    or fail_log("protocol exchange failed: $@");

  pipe EXECTERM, EXECTERMW or fail_log("pipe: $!");

  my $child = fork // fail_log("fork executor: $!");
  if (!$child) {
    #---- executor ----
    open ::STDIN , "<& $call_fds[0]" or fail_log("dup for fd0");
    open ::STDOUT, ">& $call_fds[1]" or fail_log("dup for fd1");
    open ::STDERR, ">& $call_fds[2]" or fail_log("dup for fd2");
    close EXECTERM;
    close_call_fds();
    $! = 0;
    return;
  }
  close EXECTERMW;

  #---- monitor [2] ----

  for (;;) {
    my $rbits = '';
    vec($rbits, fileno(CALL), 1) = 1;
    vec($rbits, fileno(EXECTERM), 1) = 1;
    my $ebits = $rbits;
    my $nfound = select($rbits, '', $ebits, undef);
    last if $nfound > 0;
    next if $! == EINTR;
    fail_log("monitor select() failed: $!");
  }

  # Either the child has just died, or the caller has gone away

  $SIG{INT} = 'IGN';
  kill 'INT', 0 or fail_log("kill executor [$child]: $!");

  my $got = waitpid $child, 0;
  $got >= 0 // fail_log("wait for executor [$child] (2): $!");
  $got == $child or fail_log("wait for esecutor [$child] gave [$got]");

  protocol_write(pack "N", $?);
  _exit(0);
}

sub close_call_fds () {
  foreach (@call_fds) {
    POSIX::close($_);
  }
  close CALL;
}

sub protocol_write ($) {
  my ($d) = @_;
  return if (print CALL $d and flush CALL);
  _exit(0) if $!==EPIPE || $!==ECONNRESET;
  fail_log("protocol write: $!");
}

sub eintr_retry ($) {
  my ($f) = @_;
  for (;;) {
    my $r = $f->();
    return $r if defined $r;
    next if $!==EINTR;
    return $r;
  }
}

sub protocol_read_fail ($) {
  my ($what) = @_;
  _exit(0) if $!==ECONNRESET;
  die("recv $what: $!");
}

sub protocol_exchange () {
  my $greeting = "PFI\n\0\0\0\0";
  protocol_write($greeting);

  my $ibyte = 0;
  my $r;
  for (;;) {
    $r = sysread CALL, $ibyte, 1;
    last if $r > 0;
    $!==EINTR or protocol_read_fail("signalling byte");
  }
  $r == 1 or _exit(0);
  $ibyte = ord $ibyte;
  $ibyte and die(sprintf "signalling byte is 0x%02x, not zero", $ibyte);

  @call_fds = map {
    my $r;
    for (;;) {
      $! = 0;
      $r = IO::FDPass::recv(fileno(CALL));
      last if $r >= 0;
      _exit(0) if $!==0;
      protocol_read_fail("fd $_");
    }
    $r;
  } 0..2;

  my $len;
  $r = read(CALL, $len, 4) // protocol_read_fail("message length");
  $r == 4 or _exit(0);

  $len = unpack "N", $len;
  my $data;
  $r = read(CALL, $data, $len) // protocol_read_fail("message data ($len)");
  $r == $len or _exit(0);

  @ARGV = split /\0/, $data, -1;
  @ARGV >= 2 or die("message data has too few strings (".(scalar @ARGV).")");
  length(pop(@ARGV)) and die("message data missing trailing nul");
  %ENV = ();
  while (my $s = shift @ARGV) {
    last if !length $s;
    $s =~ m/=/ or die("message data env var missing equals");
    $ENV{$`} = $';
  }
}

sub autoreload_check ($) {
  my ($f) = @_;
  my @s = Time::HiRes::stat($f);
  if (!@s) {
    $!==ENOENT or fail_log("autoreload check: stat failed: $f: $!");
    return;
  }
  if ($s[9] > $startup_mtime) {
    syslog(LOG_INFO, "$0 prefork: reloading; due to $f");
    _exit(0);
  }
}

sub prefork_initialisation_complete {
  my %opts = @_;

  push @autoreload_extra_files, $0;

  # if env var not set, we're not running under prefork-interp
  my @env_data = split / /, ($ENV{$env_name} // return);
  croak "$env_name has too few words" unless @env_data >= 2;
  my (@vsns) = split /,/, $env_data[0];
  croak "$env_name doesn't specify v1" unless @vsns >= 2 && $vsns[0] eq 'v1';
  $startup_mtime = $vsns[1];
  my @env_fds = split /,/, $env_data[1];
  croak "$env_name has too few fds" unless @env_fds >= 4;;
  $#env_fds = 3;

  my $num_servers = $opts{max_servers} // 4;

  #---- setup (pm) [1] ----

  foreach (@env_fds) {
    $_ eq ($_+0) or croak "$env_name contains $_, not a number";
  }
  open LISTEN, "+>&=$env_fds[0]" or croak "listen fd: $!";
  open CALL,   "+>&=$env_fds[1]" or croak "call fd: $!";
  open WATCHI, "+>&=$env_fds[2]" or croak "call fd: $!";
  open WATCHE, "+>&=$env_fds[3]" or croak "watch stderr fd: $!";

  my $log_facility = $opts{log_facility} // 'LOG_USER';
  if (length $log_facility) {
    openlog("prefork-interp $0", 'ndelay,nofatal,pid', $log_facility);
  }

  open NULL, "+>/dev/null" or croak "open /dev/null: $!";

  #---- fork for server ----

  my $child = fork // croak "first fork failed: $!";
  if ($child) {
    #---- setup (pm) [2], exits ----
    _exit(0);
  }
  setsid() > 0 or fail_log("setsid: $!");
  # The server will be a session leader, but it won't open ttys,
  # so that is ok.

  #---- server(pm) [1] ----

  $child = fork // croak "second fork failed: $!";
  if (!$child) {
    # we are the child, i.e. the one fa-monitor
    local $0 = "$0 [monitor(init)]";
    return become_monitor();
  }
  close CALL;

  our %children;
  $children{$child} = 1;
  
  # --- server(pm) [2] ----

  local $0 = "$0 [server]";

  $fail_log = 1;
  open STDIN, "<&NULL" or fail_log("dup null onto stdin: $!");
  open STDOUT, ">&NULL" or fail_log("dup null onto stdout: $!");
  open STDERR, ">&NULL" or fail_log("dup null onto stderr: $!");
  close NULL;

  my $errcount = 0;

  for (;;) {
    # reap children
    if (%children) {
      my $full = $num_servers >= 0 ? %children >= $num_servers : 0;
      my $got = waitpid -1, ($full ? 0 : WNOHANG);
      $got >= 0 or fail_log("failed to wait for monitor(s): $!");
      if ($got) {
	if ($? && $? != SIGPIPE) {
	  syslog(LOG_WARNING,
 "$0 prefork: monitor process [$got] failed with wait status $?");
	}
	if (!exists $children{$got}) {
	  syslog(LOG_WARNING,
 "$0 prefork: monitor process [$got] wasn't one of ours?!");
	}
	delete $children{$got};
	next;
      }
    }

    # select for accepting or housekeeping timeout
    my $rbits = '';
    vec($rbits, fileno(LISTEN), 1) = 1;
    vec($rbits, fileno(WATCHE), 1) = 1;
    my $ebits = $rbits;
    my $idle_timeout = $opts{idle_timeout} // 1000000;
    $idle_timeout = undef if $idle_timeout < 0;
    my $nfound = select($rbits, '', $ebits, $idle_timeout);

    # Idle timeout?
    last if $nfound == 0;
    if ($nfound < 0) {
      next if $! == EINTR;
      fail_log("select failed: $!");
    }

    # Has the watcher told us to shut down, or died with a message ?
    my $msgbuf = '';
    my $r = sysread WATCHE, $msgbuf, 2048;
    if ($r > 0) {
      chomp $msgbuf;
      fail_log("watcher: $msgbuf");
    } elsif (defined $r) {
      syslog(LOG_INFO,
 "$0 prefork: lost socket (fresh start or cleanup?), quitting");
      last;
    } elsif ($! == EINTR || $! == EAGAIN || $! == EWOULDBLOCK) {
    } else {
      fail_log("watcher stderr read: $!");
    }

    if (%opts{autoreload_inc} // 1) {
      foreach my $f (values %INC) {
	autoreload_check($f);
      }
    }
    foreach my $f (@autoreload_extra_files) {
      autoreload_check($f);
    }
    foreach my $f (@{ %opts{autoreload_extra} // [] }) {
      autoreload_check($f);
    }

    # Anything to accept ?
    if (accept(CALL, LISTEN)) {
      $child = fork // fail_log("fork for accepted call failed: $!");
      if (!$child) {
	#---- monitor [1] ----
	$0 =~ s{ \[server\]$}{ [monitor]};
	return become_monitor();
      }
      close(CALL);
      $errcount = 0;
      $children{$child} = 1;
    } elsif ($! == EINTR || $! == EAGAIN || $! == EWOULDBLOCK) {
    } else {
      syslog(LOG_WARNING, "$0 prefork: accept failed: $!");
      if ($errcount > ($opts{max_errors} // 100)) {
	fail_log("too many accept failures, quitting");
      }
    }
  }
  _exit(0);
}

1;

__END__

=head1 NAME

Proc::Prefork::Interp - script-side handler for prefork-interp

=head1 SYNOPSYS

    #!/usr/bin/prefork-interp -U,perl,-w
    # -*- perl -*-
    use strict;
    use Proc::Prefork::Interp;

    ... generic initialisation code, use statements ...

    prefork_initialisation_complete();

    ... per-execution code ...

=head1 DESCRIPTION

Proc::Prefork::Interp implements the script-side protocol
expected by the preform-interp C wrapper program.

The combination arranges that the startup overhead of your script
is paid once, and then the initialised script can service multiple
requests, quickly and in parallel.

C<prefork_initialisation_complete> actually daemonises the program,
forking twice, and returning in the grandchild.

It returns once for each associated invocation of C<prefork-interp>
(ie, each invocation of the script which starts C<#!/usr/bin/prefork-interp>),
each time in a separate process.

=head1 PRE-INITIALISATION STATE, CONTEXT AND ACTIONS

During initialisation, the program may load perl modules, and do
other kinds of pre-computation and pre-loading.

Where files are read during pre-loading, consider calling
C<prefork_autoreload_also_check> to arrange that the script will
automatically be restarted when the files change.
See L</"AUTOMATIC RELOADING">.

Before C<prefork_initialisation_complete>,
the script will stdin connected to /dev/null,
and stdout connected to its stderr.

It should avoid accessing its command line arguments
- or, at least, those which will vary from call to call.

Likewise it should not pay attention to environment variables
which are expected to change from one invocation to the next.
For example, if the program is a CGI script, it ought not to
read the CGI environment variables until after initialisation.

It is I<NOT> safe to open a connection to a database,
or other kind of server, before initialisation is complete.
This is because the db connection would end up being shared
by all of the individual executions.

=head1 POST-INITIALISATION STATE, CONTEXT AND ACTIONS

Each time C<prefork_initialisation_complete> returns,
corresponds to one invocation of C<prefork-interp>.

On return the script will have its stdin, stdout and stderr
connected to those provided by C<prefork-interp>'s caller
for this invocation.
Likewise C<@ARGV> and C<%ENV> will have been adjusted to
copy the arguments and environment of the particular call.

By this time, the process has forked twice.
Its parent is not the original caller,
and it is in a session and a process group
set up for this shared script and this particular invocation,
respectively.

Signals sent to the C<prefork-interp> will not be received
by the script.
if C<prefork-interp> is killed, the script will receive a C<SIGINT>;
when that happens it ought to die promptly,
without doing further IO on stdin/stdout/stderr.

The exit status of the script will be reproduced
as the exit status of C<prefork-interp>,
so that the caller sees the right exit status.

=head1 DESCRIPTORS AND OTHER INHERITED PROCESS PROPERTIES

The per-invocation child inherits everything that is
set up before C<prefork_initialisation_complete>.

This includes ulimits, signal dispositions, uids and gids,
and of course file descriptors (other than 0/1/2).

The prefork-interp system
uses C<SIGINT> to terminate services when needed
and relies on C<SIGPIPE> to have a default disposition.
Do not mess with these.

It is not generally safe to open a connection to some kind of service
during initialisation.
Each invocation will share the socket,
which can cause terrible confusion (even security holes).
For example, do not open a database handle during initialisation.

=head1 AUTOMATIC RELOADING

The prefork-interp system supports automatic reloading/restarting,
when a script, or files it loads, are modified.

Files mentioned in C<$0> and C<%INC> will automatically be checked;
if any are found to be newer than the original invocation,
a fressh "server" will created -
re-running the script again from the top, as for an initial call.

The set of files checked in this way can be modified
via initialisation-complete options,
or by calling C<prefork_autoreload_also_check>.

=head1 STANDALONE OPERATION

A script which loads Proc::Prefork::Interp
and calls C<prefork_initialisation_complete>
can also be run standalone.
This can be useful for testing.

When not run under C<prefork-interp>, C<prefork_initialisation_complete>
does nothing and returns in the same process.

=head1 FUNCTIONS

=over

=item C<< prefork_initialisation_complete( I<%options> ) >>

Turns this script into a server,
which can be reused for subsequent invocations.
Returns multiple times,
each time in a different process,
one per invocation.

When not run under C<prefork-interp>, this is a no-op.

C<%options> is an even-length list of options,
in the format used for initalising a Perl hash:

=over

=item C<< max_servers => I<MAX> >>

Allow I<MAX> (an integer) concurrent invocations at once.
If too many invocations arrive at once,
new ones won't be served until some of them complete.

If I<MAX> is negative, there is no limit.
The limit is only applied somewhat approximately.
Default is 4.

=item C<< idle_timeout => I<TIMEOUT> >>

If no invocations occur for this length of time, we quit;
future invocations would involve a restart.

If I<TIMEOUT> is negative, we don't time out.

=item C<< autoreload_inc => I<BOOL> >>

If set falseish,
we don't automatically check files in C<%INC> for reloads.
See L</"AUTOMATIC RELOADING">.

=item C<< autoreload_extra => [ I<PATHS> ] >>

Additional paths to check for reloads
(as an arrayref of strings).
(This is in addition to paths passed to C<prefork_autoreload_also_check>.)
See L</"AUTOMATIC RELOADING">.
Default is 1 megasecond.

=item C<< max_errors => I<NUMBER> >>

If our server loop experiences more errors than this, we quit.
(If this happens,
a future invocation would restart the script from the top.)
Default is 100.

=item C<< log_facility => I<BOOL> >>

The syslog facility to use,
for messages from the persistent server.

The value is in the format expected by C<Sys::Syslog::openlog>;
the empty string means not to use syslog at all,
in which case errors experienced by the psersistent server
will not be reported anywhere, impeding debugging.

Default is C<LOG_USER>.

=back

=item C<< prefork_autoreload_also_check I<PATHS> >>

Also check each path in I<PATHS> for being out of date;
if any exists and has an mtime after our setup,
we consider ourselves out of date and arrange for a reload.

It is not an error for a I<PATH> to not exist,
but it is an error if it can't be checked.

=back

=head1 AUTHORS AND COPYRIGHT

The prefork-interp system was designed and implemented by Ian Jackson
and is distributed as part of chiark-utils.

prefork-interp and Proc::Prefork::Interp are
Copyright 2022 Ian Jackson and contributors to chiark-utils.

=head1 LIMITATIONS

A function which works and returns in the grant parent,
having readjusted many important process properties,
is inherently rather weird.
Scripts using this facility must take some care.

Signal propagation, from caller to actual service, is lacking.

If the service continues to access its descriptors after receiving SIGINT,
the ultimate callers can experience maulfunctions
(eg, stolen terminal keystrokes!)

=head1 FUTURE POSSIBILITIES

This system should work for Python too.
I would welcome contribution of the approriate Python code.
Please get in touch so I can help you.

=head1 SEE ALSO

=over

=item C<prefork-interp.txt>

Usage and options for the C<prefork-interp>
invocation wrapper program.

=item C<prefork-interp.c>

Design and protocol information is in the comments
at the top of the source file.

=back
