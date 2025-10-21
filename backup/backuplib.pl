# backuplib.pl
# core common routines
#
# This file is part of chiark backup, a system for backing up GNU/Linux and
# other UN*X-compatible machines, as used on chiark.greenend.org.uk.
#
# chiark backup is:
#  Copyright (C) 1997-1998,2000-2001,2007
#                     Ian Jackson <ian@chiark.greenend.org.uk>
#  Copyright (C) 1999 Peter Maydell <pmaydell@chiark.greenend.org.uk>
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

require IO::File;

$nice='nice ' if !defined $nice;

sub printdate () {
    print scalar(localtime),"\n";
}

# Set status info -- we write the current status to a file 
# so if we hang or crash the last thing written to the file
# will tell us where we were when things went pear-shaped.
sub setstatus ($) {
    open S, ">this-status.new" or die $!;
    print S $_[0],"\n" or die $!;
    close S or die $!;
    rename "this-status.new","this-status" or die $!;
}

# startprocess, endprocesses, killprocesses are 
# used to implement the funky pipeline stuff.
sub startprocess ($$$) {
    my ($i,$o,$c) = @_;
    pboth("  $c\n");
    defined($p= fork) or die $!;
    if ($p) { $processes{$p}= $c; return; }
    open STDIN,"$i" or die "$c stdin $i: $!";
    open STDOUT,"$o" or die "$c stdout $o: $!";
    &closepipes;
    exec $c; die "$c: $!";
}

sub rewind_raw () {
    runsystem("mt -f $tape rewind");
}

sub readtapeid_raw () {
    open T, ">>TAPEID" or die $!; close T;
    unlink 'TAPEID' or die $!;
    rewind_raw();
    system "mt -f $tape setblk $blocksizebytes"; $? and die $?;
    system "dd if=$tape bs=${blocksize}b count=10 ".
	   "| tar -b$blocksize -vvxf - TAPEID";
}

sub runsystem ($) {
    pboth("    $_[0]\n");
    system $_[0];
    $? and die $?;
}

sub pboth ($) {
    my ($str) = @_;
    print LOG $str or die $!;
    print $str or die $!;
}

sub nexttapefile ($) {
    my ($what) = @_;
    $currenttapefilenumber++;
    $currenttapefilename= $what;
    pboth(sprintf "writing tape file #%d (mt fsf %d): %s\n",
	  $currenttapefilenumber, $currenttapefilenumber-1, $what);
}

sub writetapeid ($$) {
    open T, ">TAPEID" or die $!;
    print T "$_[0]\n$_[1]\n" or die $!;
    close T or die $!;

    $currenttapefilenumber= 0;
    nexttapefile('TAPEID');

    system "tar -b$blocksize -vvcf TAPEID.tar TAPEID"; $? and die $?;
    system "dd if=TAPEID.tar of=$ntape bs=${blocksize}b count=10";
    $? and die $?;
}

sub endprocesses () {
    while (keys %processes) {
	$p= waitpid(-1,0) or die "wait: $!";
	if (!exists $processes{$p}) { warn "unknown pid exited: $p, code $?\n"; next; }
	$c= $processes{$p};
	delete $processes{$p};
	$? && die "error: command gave code $?: $c\n";
    }
    pboth("  ok\n");
}

sub killprocesses {
    for $p (keys %processes) {
	kill 15,$p or warn "kill process $p: $!";
    }
    undef %processes;
}

# Read a fsys.foo filesystem group definition file.
# Syntax is: empty lines and those beginning with '#' are ignored.
# Trailing whitespace is ignored. Lines of the form 'prefix foo bar'
# are handled specially, as arex lines 'exclude regexp'; otherwise 
# we just shove the line into @fsys and let parsefsys deal with it.

sub readfsysfile ($) {
    my ($fn) = @_;
    my ($fh,$sfn);
    $fh= new IO::File "$fn", "r" or die "cannot open fsys file $fn ($!).\n";
    for (;;) {
	$!=0; $_= <$fh> or die "unexpected EOF in $fn ($!)\n";
	chomp; s/\s*$//;
	last if m/^end$/;
	next unless m/\S/;
	next if m/^\#/;
	if (m/^prefix\s+(\w+)\s+(\S.*\S)$/) {
	    $prefix{$1}= $2;
	} elsif (m/^prefix\-df\s+(\w+)\s+(\S.*\S)$/) {
	    $prefixdf{$1}= $2;
	} elsif (m/^snap(?:\=(\w+))?\s+(\w+)\s+(\w+)$/) {
            push @excldir,$1;
	} elsif (m/^excludedir\s+(\S.*\S)$/) {
            push @excldir,$1;
        } elsif (m/^exclude\s+(\S.*\S)$/) {
            push @excl,$1;
	} elsif (m/^include\s+(\S.*\S)$/) {
	    $sfn = $1;
	    $sfn =~ s/^\./fsys./;
	    $sfn = "$etc/$sfn" unless $sfn =~ m,^/,;
	    readfsysfile($sfn);
        } else {
	    push @fsys,$_;
	}
    }
    close $fh or die $!;
}

sub readfsys ($) {
    my ($fsnm) = @_;
    my ($fsf);
    $fsf= "$etc/fsys.$fsnm";
    stat $fsf or die "Filesystems $fsnm unknown ($!).\n";
    readfsysfile($fsf);
}

# Parse a line from a filesystem definition file. We expect the line
# to be in $tf.
sub parsefsys () {
    my ($dopts,$dopt);
    if ($tf =~ m#^(/\S*)\s+(\w+)([,=0-9a-z]*)$#) {
        # Line of form '[/device:]/file/system	dumptype[,options]'
	$atf= $1;
	$tm= $2;
	$dopts= $3;
	$prefix= '<local>';
	$pcstr= '';
	$rstr= '';
    } elsif ($tf =~ m#^(/\S*)\s+(\w+)([,=0-9a-z]*)\s+(\w+)$#) {
        # Line of form '[/device:]/file/system dumptype[,options] prefix'
        # (used for remote backups)
	$atf= $1;
	$tm= $2;
	$dopts= $3;
	$prefix= $4;
	$pcstr= "$prefix:";
	defined($prefix{$prefix}) or die "prefix $prefix in $tf ?\n";
	$rstr= $prefix{$prefix}.' ';
    } else {
	die "fsys $tf ?";
    }

    $fsidstr= $pcstr.$atf;
    $fsidstr =~ s/[,+]/+$&/g;
    $fsidstr =~ s#/#,#g;
    $fsidfile= "/var/lib/chiark-backup/incstamp,$fsidstr";

    $dev = $atf =~ s,^(.*)\:,, ? $1 : '';

    if (!length $pcstr) {
	stat $atf or die "stat $atf: $!";
	-d _ or die "not a dir: $atf";
    }

    undef %dopt;
    foreach $dopt (split /\,/,$dopts) {
	if (grep { $dopt eq $_ } qw(gz noinc)) {
	    $dopt{$dopt}= 'y';
	} elsif (grep { $dopt eq $_ } qw(snap)) {
	    $dopt{$dopt}= $dopt;
	} elsif ($dopt =~ m/\=/ && grep { $` eq $_ } qw(gz snap)) {
	    $dopt{$`}= $';
	} elsif (length $dopt) {
	    die "unknown option $dopt (in $dopts $tf)";
	}
    }

    my ($gzo);
    foreach $gzo (qw(gz gzi)) {
	if ($dopt{$gzo} eq 'y') {
	    $$gzo= '1';
	} elsif ($dopt{$gzo} =~ m/^\d$/) {
	    $$gzo= $dopt{$gzo};
	} elsif (defined $dopt{$gzo}) {
	    die "$tf bad $gzo";
	} else {
	    $$gzo= '';
	}
    }

    if (length $dopt{'snap'}) {
	length $dev or die "$pcstr:$atf no device but needed for snap";
    }
}

sub execute ($) {
    pboth("  $_[0]\n");
    system $_[0]; $? and die "$_[0] $?";
}

sub prepfsys () {
    $dev_print= $dev;
    $atf_print= $atf;
    
    if (length $dopt{'snap'}) {
	
	system('snap-drop'); $? and die $?;
	
	$snapscripts= '/etc/chiark-backup/snap';
	$snapbase= "$rstr $snapscripts/$dopt{'snap'}";
	$snapargs= "/var/lib/chiark-backup";

	$snapsnap= "$snapbase snap $snapargs $dev $atf";
	$snapdrop= "$snapbase drop $snapargs";

	open SD, ">snap-drop.new" or die $!;
	print SD $snapdrop,"\n" or die $!;
	close SD or die $!;
	rename "snap-drop.new","snap-drop" or die $!;

	execute($snapsnap);

	$dev_nosnap= $dev;
	$atf_nosnap= $atf;
	$dev= "/var/lib/chiark-backup/snap-device";
	$atf= "/var/lib/chiark-backup/snap-mount";
    }
}

sub finfsys () {
    if (length $dopt{'snap'}) {
	system('snap-drop'); $? and die $?;
    }
}

sub openlog () {
    unlink 'log';
    $u= umask(007);
    open LOG, ">log" or die $!;
    umask $u;
    select(LOG); $|=1; select(STDOUT);
}

$SIG{'__DIE__'}= 'killprocesses';

1;
