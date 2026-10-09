#!/bin/busybox sh
# Guest init of the Linux check of an image written by a native macOS mount.
# tests/prepare_native_linux.py places the manifest the guest write test printed
# in /native/manifest.tsv; /dev/vda is a disposable copy of the written image.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /dev /proc /sys /tmp /mnt
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
trap 'echo BTRFS_NATIVE_FAIL; dmesg | tail -60; sync; poweroff -f' EXIT
for module in $(cat /modules/order); do
    insmod /modules/$module.ko
done
exec < /dev/hvc0 > /dev/hvc0 2>&1
uname -r
btrfs --version
test "$(blockdev --getsize64 /dev/vda)" = @DEVICE_BYTES@
btrfs check --readonly /dev/vda < /dev/null
mount -t btrfs -o ro,rescue=nologreplay /dev/vda /mnt
# A directory's names exactly as stored, one per line in byte order. Shell
# globbing keeps every byte; BusyBox ls rewrites names it cannot print.
list_names() {
    for name in "$1"/* "$1"/.[!.]* "$1"/..?*; do
        if [ -e "$name" ] || [ -L "$name" ]; then
            printf '%s\n' "${name##*/}"
        fi
    done | LC_ALL=C sort
}

# Each name of a directory with its inode number, or "-" for an object of
# another subvolume (another device), sorted; native walks list the same,
# without the volume metadata directories macOS services own at the root
# (tests/mounted_walk.c).
root_device=$(stat -c '%d' /mnt)
inode_lines() {
    directory=$1
    list_names "$directory" | while IFS= read -r name; do
        if [ "$directory" = /mnt/. ]; then
            case "$name" in
            .fseventsd|.Spotlight-V100|.Trashes|.TemporaryItems|.DocumentRevisions-V100)
                continue ;;
            esac
        fi
        set -- $(stat -c '%d %i' -- "$directory/$name")
        if [ "$1" = "$root_device" ]; then
            printf '%s\t%s\n' "$name" "$2"
        else
            printf '%s\t-\n' "$name"
        fi
    done | LC_ALL=C sort
}

checks=0
while IFS="$(printf '\t')" read -r kind path a b c d e; do
    target="/mnt/$path"
    case "$kind" in
    file)
        test -f "$target" && test ! -L "$target" &&
            test "$(sha256sum "$target" | cut -d' ' -f1)" = "$a" &&
            test "$(stat -c '%a %u %g %h' "$target")" = "$b $c $d $e" ;;
    dir) test -d "$target" &&
        test "$(list_names "$target" | sha256sum | cut -d' ' -f1)" = "$a" ;;
    inodes) test -d "$target" &&
        test "$(inode_lines "$target" | sha256sum | cut -d' ' -f1)" = "$a" ;;
    symlink) test -L "$target" && test "$(readlink "$target")" = "$a" ;;
    xattr) test "$(getfattr --only-values -n "$a" "$target")" = "$b" ;;
    mtime) test "$(stat -c '%Y' "$target")" = "$a" ;;
    absent) test ! -e "$target" && test ! -L "$target" ;;
    # MASK VALUE: the flags of the top-level tree's inode item.
    flags) inode=$(stat -c '%i' "$target") &&
        value=$(btrfs inspect-internal dump-tree -t 5 /dev/vda |
            awk -v key="key ($inode INODE_ITEM 0)" '
            index($0, key) && index($0, "itemoff") { found = 1 }
            found && match($0, /flags 0x[0-9a-f]+/) { print substr($0, RSTART + 6, RLENGTH - 6); exit }') &&
        test -n "$value" && test $((value & a)) -eq $((b)) ;;
    *) false ;;
    esac || { echo "Native check failed: $kind $path"; exit 1; }
    checks=$((checks + 1))
done < /native/manifest.tsv
test "$checks" = @CHECKS@
echo "BTRFS_NATIVE_CHECKS:$checks"
umount /mnt
# Linux continues read-write from the root set macOS committed.
options=nospace_cache
if btrfs inspect-internal dump-super /dev/vda | grep -q FREE_SPACE_TREE_VALID; then
    options=defaults
fi
mount -t btrfs -o "$options" /dev/vda /mnt
printf 'Linux accepted the macOS root\n' > /mnt/after-macos
btrfs filesystem sync /mnt
umount /mnt
btrfs check --readonly /dev/vda < /dev/null
test "$(btrfs inspect-internal dump-tree -t 5 /dev/vda | grep -c ORPHAN_ITEM || true)" = 0
echo BTRFS_NATIVE_PASS
trap - EXIT
poweroff -f
