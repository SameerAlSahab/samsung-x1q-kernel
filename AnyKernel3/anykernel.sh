# AnyKernel3 Ramdisk Mod Script
# osm0sis @ xda-developers

## AnyKernel setup
# begin properties
properties() { '
kernel.string=-YacineKernel 
do.devicecheck=1
do.modules=0
do.systemless=1
do.cleanup=1
do.cleanuponabort=0
device.name1=x1q
device.name2=x1qxx
device.name3=y2q
supported.versions=11 - 15
supported.patchlevels=
'; } # end properties

# shell variables
block=/dev/block/platform/soc/1d84000.ufshc/by-name/boot;
is_slot_device=0;
ramdisk_compression=auto;

## AnyKernel methods (DO NOT CHANGE)
# import patching functions/variables - see for reference
. tools/ak3-core.sh;

## AnyKernel file attributes
# set permissions/ownership for included ramdisk files
set_perm_recursive 0 0 755 644 $ramdisk/*;
set_perm_recursive 0 0 750 750 $ramdisk/init* $ramdisk/sbin;

## AnyKernel boot install
dump_boot;

# Flash DTBO

flash_dtbo() {
  if [ -f $ZIPFILE/dtbo.img ]; then
    dd if=$ZIPFILE/dtbo.img of=/dev/block/by-name/dtbo
  fi
}
# begin kernel/dtb/dtbo changes
oneui=$(file_getprop /system/build.prop ro.build.version.oneui);
gsi=$(file_getprop /system/build.prop ro.product.system.device);
if [ -n "$oneui" ]; then
   ui_print " "
   ui_print " • OneUI ROM detected! • " 
   ui_print " "
   ui_print " • Patching Fingerprint Sensor... • "
   patch_cmdline "android.is_aosp" "android.is_aosp=0";
elif [ $gsi == generic ]; then
   ui_print " "
   ui_print " • GSI ROM detected! • "
   ui_print " "
   ui_print " • Patching Fingerprint Sensor... • "
   patch_cmdline "android.is_aosp" "android.is_aosp=0";
else
   ui_print " "
   ui_print " • AOSP ROM detected! • " 
   ui_print " "
   ui_print " • Patching CMDline... • "
   patch_cmdline "androidboot.verifiedbootstate=orange" "androidboot.verifiedbootstate=green"
   ui_print " "
   ui_print " • Patching Fingerprint Sensor... • "
   patch_cmdline "android.is_aosp" "android.is_aosp=1";
fi

write_boot;
## end boot install
