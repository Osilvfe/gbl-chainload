#!/system/bin/sh
# KernelSU action button.
MODDIR=${0%/*}
"$MODDIR/bin/slotctl" state
