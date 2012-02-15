#!/bin/ksh

DEV="/dev/sdd"
IDIR=/ptmp/usb
ISOF=/bradytech/src/boot_files
BINDIR=/bradytech/src/bin
NSECT=64	# Number of sectors
NHDS=32		# Number of heads
NCYL=1		# Number of cylinders
NBLKS=2050	# Number of blocks (NSECT*NHDS*NCYL+2)

dir=/bradytech/src/memtest86-3.6

print "Insert USB Key"
print "Creating image on device $DEV"
print "Is this correct?"
read ans
if [ $ans != "y" ]
then
	exit
fi

$BINDIR/mkdiskimage -4 $DEV $NCYL $NSECT $NHDS
sync
sleep 2

mkfs.msdos ${DEV}4 
$BINDIR/syslinux ${DEV}4 
mkdir $IDIR
mount ${DEV}4 $IDIR

# Copy boot files
cd $ISOF
cp boot.txt $IDIR
cp boot.cfg $IDIR/syslinux.cfg
cd -

# Create std boot images
cd $dir
make
cp memtest.bin $IDIR/memtest
make smp
cp memtest.bin $IDIR/memtest.smp
cd -

umount $IDIR

# Create image
dd if=$DEV count=$NBLKS of=memtest86-3.6.usb
cat <<EOF>README
Installation instructions for the Memtest86 USB key image (Linux Only).
=======================================================================

1) Insert a USB key into a USB slot.
2) Determine which device the USB key is assigned as (ie. /dev/sdc).
3) As root type: dd if=memtest86-3.5.usb of=dev where dev is the device
the key is assigned to. Use the base device (ie. /dev/sdc) not a partition
designation (ie. /dev/sdc1).

Warning: all data on the USB key will be lost. 
Warning Warning: Make sure that the device used in the dd command above
is correct!!!
EOF
tar cvzf memtest86-3.5.usb.tgz memtest86-3.5.usb README
chown cbrady memtest86-3.5.usb.tgz
rm memtest86-3.5.usb README

cd -
# Clean up
rm -rf $IDIR
cd $dir
make clean
print "## Done!"

