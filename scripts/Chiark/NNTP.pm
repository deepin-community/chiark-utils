#!/usr/bin/perl

# Originally by Simon Tatham
# Modified by Richard Kettlewell, Colin Watson, Ian Jackson
#
# Copyright -2011 Simon Tatham
# Copyright 2011 Richard Kettlewell
# Copyright 2011 Colin Watson
# Copyright 2011 Ian Jackson
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# SOFTWARE IN THE PUBLIC INTEREST, INC. BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

use strict qw(subs);
use warnings;

require 5.002;
use Socket;
use FileHandle;


BEGIN {
    use Exporter   ();
    our ($VERSION, @ISA, @EXPORT, @EXPORT_OK, %EXPORT_TAGS);

    # set the version for version checking
    $VERSION     = 1.00;

    @ISA         = qw(Exporter);
    @EXPORT      = qw(cnntp_connect);
    %EXPORT_TAGS = ( );     # eg: TAG => [ qw!name1 name2! ],
    
    @EXPORT_OK   = qw();
}
our @EXPORT_OK;

sub cnntp_connect ($) {
    my ($verbose) = @_;

    my $ns=$ENV{'NNTPSERVER'};
    if (!defined $ns or !length $ns) {
	$? = 0;
	$ns = `cat /etc/nntpserver`;
	die if $?;
	chomp($ns);
    }
    my $port = (getservbyname("nntp", "tcp"))[2];
    $ns = inet_aton($ns);
    my $proto = getprotobyname("tcp");
    my $paddr = sockaddr_in($port, $ns);

    my $sock = new IO::Handle;
    socket($sock,PF_INET,SOCK_STREAM,$proto) or die "socket: $!";
    connect($sock,$paddr) or die "connect: $!";

    $sock->autoflush(1);

    return bless { S => $sock, V => $verbose };
}

sub banner_reader ($) {
    my ($c) = @_;
    my ($code,$l) = $c->getline();
    $code =~ /^2\d\d/ or die "no initial greeting from server\n";
    $c->docmd("MODE READER");
}

sub disconnect ($) {
    my ($c) = @_;
    close $c->{S};
}

sub putline ($$) {
    my ($c, $line) = @_;
    my $s = $c->{S};
    my $v = $c->{V};
    print $v ">>> $line\n" if $v;
    print $s "$line\r\n";
}

sub getline_raw ($) {
    my ($c) = @_;
    my $s = $c->{S};
    my $l = <$s>;
    return $l;
}

sub getline ($) {
    my ($c) = @_;
    my $v = $c->{V};
    my $l = $c->getline_raw();
    $l =~ s/[\r\n]*$//s;
    my $code = substr($l,0,3);
    print $v "<<< $l\n" if $v;
    return ($code,$l);
}

sub docmd ($$;$) {
    my ($c,$cmd,$nocheck) = @_;
    my ($code,$l);
    for my $n (0,1) {
	$c->putline($cmd);
	($code,$l) = $c->getline();
	if ($code eq "480") { $c->auth(); } else { last; }
    }
    if (!$nocheck) {
	$code =~ /^2\d\d/ or die "failed on `$cmd':\n$l\n";
    }
    return ($code,$l);
}

sub auth ($) {
    my ($c) = @_;
    # Authentication.
    return if $c->{Authed}++;
    my $auth = $ENV{"NNTPAUTH"};
    if (defined $auth) {
	$c->putline("AUTHINFO GENERIC $auth");
	pipe AUTHSTDIN, TOAUTH or die "unable to create pipes";
	pipe FROMAUTH, AUTHSTDOUT or die "unable to create pipes";
	flush STDOUT;
	my $pid = fork;
	if (!defined $pid) {
	    die "unable to fork for authentication helper";
	} elsif ($pid == 0) {
	    # we are child
	    $c->{V} = undef if $c->{V} eq 'STDOUT';
	    $ENV{"NNTP_AUTH_FDS"} = "0.1";
	    open STDIN, "<&AUTHSTDIN";
	    open STDOUT, ">&AUTHSTDOUT";
	    close $c->{S};
	    exec $auth;
	    die $!;
	}
	# we are parent
	close AUTHSTDIN;
	close AUTHSTDOUT;
	autoflush TOAUTH 1;
	my ($code,$l) = $c->getline(); print TOAUTH "$l\n";
	while (<FROMAUTH>) {
	    s/[\r\n]*$//s;
	    $c->putline($_);
	    ($code,$l) = $c->getline();
	    print TOAUTH "$l\n";
	}
	die "failed authentication\n" unless $? == 0;
    }
}

1;
