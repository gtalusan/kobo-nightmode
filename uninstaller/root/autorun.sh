#!/bin/sh

FW1_ORIG="/usr/local/Kobo/nickel -qws \&"
FW1_NEW="LD_PRELOAD=/root/screenInv.so /usr/local/Kobo/nickel -qws \&"

FW2_ORIG="/usr/local/Kobo/nickel -qws -skipFontLoad \&"
FW2_NEW="LD_PRELOAD=/root/screenInv.so /usr/local/Kobo/nickel -qws -skipFontLoad \&"

FW3_ORIG="/usr/local/Kobo/nickel -platform kobo -skipFontLoad \&"
FW3_NEW="LD_PRELOAD=/root/screenInv.so /usr/local/Kobo/nickel -platform kobo -skipFontLoad \&"

FW4_ORIG="LD_PRELOAD=/root/screenInv.so LIBC_FATAL_STDERR_=1 /usr/local/Kobo/nickel -platform kobo -skipFontLoad \&"
FW4_NEW="LIBC_FATAL_STDERR_=1 /usr/local/Kobo/nickel -platform kobo -skipFontLoad \&"

sed -i "s;^$FW1_NEW;$FW1_ORIG;" /etc/init.d/rcS
sed -i "s;^$FW2_NEW;$FW2_ORIG;" /etc/init.d/rcS
sed -i "s;^$FW3_NEW;$FW3_ORIG;" /etc/init.d/rcS
sed -i "s;^$FW4_NEW;$FW4_ORIG;" /etc/init.d/rcS

rm /root/screenInv.so
rm /mnt/onboard/.kobo/nightmode.sh
rm /mnt/onboard/.kobo/nightmode.ini
rm /mnt/onboard/.kobo/screenInvertLog
rm /mnt/onboard/.kobo/screenInvertLogFB

#cleanup
rm /etc/udev/rules.d/98-autorunHook.rules
rm /root/autorun.sh
sync
reboot
