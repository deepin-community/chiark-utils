# configuration, for putting in /etc/chiark-backup

chdir '/var/lib/chiark-backup' or die $!;
push(@INC,'/usr/share/chiark-backup');
$ENV{'PATH'} =~ s,^/usr/share/chiark-backup,,;
$ENV{'PATH'}= '/usr/share/chiark-backup:'.$ENV{'PATH'};

# This sets both blocksizes to 512b. Note that both must be the
# same if using the zftape floppy tape driver, since that requires
# blocks to be the right size, but dd with the obs=10k option
# doesn't pad the final block to the blocksize...

$blocksize= 1;
$blocksizebytes= 512*$blocksize;
$softblocksizekb= 1;
$softblocksizebytes= 1024*$softblocksizekb;
$tapename= 'st0';
$tape= "/dev/$tapename";
$ntape= "/dev/n$tapename";
1;
