#!/system/bin/sh
# KernelSU / Magisk installer for gbl-slotflag.

SKIPUNZIP=0
ui_print "- GBL Slot Flag"

if [ "$ARCH" != "arm64" ]; then
  abort "! arm64 only (found: $ARCH)"
fi

# kernel module.prop id must match the directory the manager installs into
ui_print "- module id: $(grep_prop id "$MODPATH/module.prop")"

mkdir -p /data/adb/gbl-slotflag/backup
set_perm /data/adb/gbl-slotflag 0 0 0755
set_perm /data/adb/gbl-slotflag/backup 0 0 0755

# Only our own executables. webroot/ is left alone on purpose: the manager
# sets its mode and SELinux context, and overriding them breaks the WebUI.
set_perm "$MODPATH/bin/gbl" 0 0 0755
set_perm "$MODPATH/bin/slotctl" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755

ui_print "- binary: $MODPATH/bin/gbl"
ui_print "- wrapper: $MODPATH/bin/slotctl"
