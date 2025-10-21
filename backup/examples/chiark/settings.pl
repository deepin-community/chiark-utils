#
chdir '/var/lib/chiark-backup' or die $!;
push(@INC,'/usr/share/chiark-backup');
$ENV{'PATH'} =~ s,^/usr/share/chiark-backup:,,;
$ENV{'PATH'}= '/usr/share/chiark-backup:'.$ENV{'PATH'};
$blocksize= 1;
$blocksizebytes= 512*$blocksize;
$softblocksizekb= 1;
$softblocksizebytes= 1024*$softblocksizekb;
$tapename= 'st0';
$tape= "/dev/$tapename";
$ntape= "/dev/n$tapename";
1;
