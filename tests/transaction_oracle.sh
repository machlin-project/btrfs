#!/bin/busybox sh
# Guest init of the Linux transaction oracle. tests/prepare_transactions_linux.py
# substitutes @DEVICE_BYTES@ and @NAMESPACE_CHECKS@ and places the exported
# scenarios in /transaction; /dev/vda is a disposable working disk and /dev/vdb
# the pristine fixture, which is never written.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /dev /proc /sys /tmp /mnt
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
trap 'echo BTRFS_TRANSACTION_FAIL; dmesg | tail -60; sync; poweroff -f' EXIT
for module in virtio_blk xor-neon xor raid6_pq crc32c_generic libcrc32c btrfs; do
    insmod /modules/$module.ko
done
uname -r
btrfs --version
test "$(blockdev --getsize64 /dev/vda)" = @DEVICE_BYTES@
test "$(blockdev --getsize64 /dev/vdb)" = @DEVICE_BYTES@
cmp /dev/vda /dev/vdb

refresh() {
    sync
    blockdev --flushbufs /dev/vda
}

restore() {
    while IFS="$(printf '\t')" read -r seek count; do
        dd if=/dev/vdb of=/dev/vda bs=512 skip=$seek seek=$seek count=$count conv=notrunc 2>/dev/null
    done < "$1/restore.tsv"
    refresh
}

apply() {
    while IFS="$(printf '\t')" read -r seek count payload skip; do
        dd if="$1/$payload" of=/dev/vda bs=512 seek=$seek skip=$skip count=$count conv=notrunc 2>/dev/null
    done < "$1/$2"
    refresh
}

generation() {
    awk -v stage="$2" -F '\t' '$1 == stage { print $2; exit }' "$1/stages.tsv"
}

primary_generation() {
    btrfs inspect-internal dump-super /dev/vda | awk '$1 == "generation" { print $2; exit }'
}

# MASK:VALUE: the flags of the path's inode item in the top-level tree equal
# VALUE in the bits of MASK. Busybox lsattr shows no NOCOMPRESS flag, and
# `btrfs inspect-internal rootid` needs a writable mount.
check_flags() {
    inode=$(stat -c '%i' "$1")
    flags=$(btrfs inspect-internal dump-tree -t 5 /dev/vda | awk -v key="key ($inode INODE_ITEM 0)" '
        index($0, key) && index($0, "itemoff") { found = 1 }
        found && match($0, /flags 0x[0-9a-f]+/) { print substr($0, RSTART + 6, RLENGTH - 6); exit }')
    test -n "$flags" && test $((flags & ${2%%:*})) -eq $((${2#*:}))
}

namespace_checks=0

check_namespace() {
    [ -f "$1/namespace.tsv" ] || return 0
    while IFS="$(printf '\t')" read -r stage kind path arg payload; do
        [ "$stage" = "$2" ] || continue
        namespace_checks=$((namespace_checks + 1))
        target="/mnt$path"
        case "$kind" in
        absent) test ! -e "$target" && test ! -L "$target" ;;
        file) test -f "$target" && test ! -L "$target" && cmp "$target" "$1/$payload" ;;
        symlink) test -L "$target" && test "$(readlink "$target")" = "$(cat "$1/$payload")" ;;
        dir) test -d "$target" && ls -A1 "$target" | LC_ALL=C sort > /tmp/listing &&
            cmp /tmp/listing "$1/$payload" && test "$(stat -c '%s' "$target")" = "$arg" ;;
        same) test "$(stat -c '%d:%i' "$target")" = "$(stat -c '%d:%i' "/mnt$arg")" ;;
        xattr) getfattr --only-values -n "$arg" "$target" > /tmp/value &&
            cmp /tmp/value "$1/$payload" ;;
        noxattr) ! getfattr -n "$arg" "$target" > /dev/null 2>&1 ;;
        stat) test "$(stat -c '%f:%u:%g:%h' "$target")" = "$arg" ;;
        device) test "$(stat -c '%t:%T' "$target")" = "$arg" ;;
        flags) check_flags "$target" "$arg" ;;
        feature) btrfs inspect-internal dump-super /dev/vda | grep -qw "$arg" ;;
        *) false ;;
        esac || { echo "Namespace check failed: $kind $path"; exit 1; }
    done < "$1/namespace.tsv"
}

verify() {
    btrfs check --readonly /dev/vda < /dev/null
    test "$(primary_generation)" = "$(generation "$1" "$2")"
    mount -t btrfs -o ro,nologreplay /dev/vda /mnt
    while IFS="$(printf '\t')" read -r stage generation path expected; do
        if [ "$stage" = "$2" ]; then
            cmp "/mnt$path" "$1/$expected"
        fi
    done < "$1/stages.tsv"
    test "$(stat -c '%a:%u:%g:%h' /mnt/greeting)" = '640:1001:1002:2'
    test "$(stat -c '%i' /mnt/greeting)" = "$(stat -c '%i' /mnt/hardlink)"
    test "$(cat /mnt/snapshot/value)" = 'snapshot original'
    test "$(cat /mnt/subvol/value)" = 'subvolume changed'
    test "$(getfattr --only-values -n user.text /mnt/greeting 2>/dev/null)" = 'Linux xattr'
    check_namespace "$1" "$2"
    umount /mnt
}

total=0
for scenario in /transaction/*; do
    name=${scenario##*/}
    while IFS="$(printf '\t')" read -r case commit kind mounted resolved recovered; do
        restore "$scenario"
        apply "$scenario" "case-$case.tsv"
        echo "BTRFS_TRANSACTION_CASE:$name:$case:$kind:$mounted:$resolved:$recovered"
        if [ "$mounted" = - ]; then
            if mount -t btrfs -o ro,nologreplay /dev/vda /mnt 2>/dev/null; then
                umount /mnt
                echo "Linux mounted a primary superblock this implementation rejects"
                exit 1
            fi
        else
            verify "$scenario" "$mounted"
        fi
        if [ "$recovered" = 1 ]; then
            status=0
            btrfs rescue super-recover -y /dev/vda < /dev/null || status=$?
            test "$status" -eq 2
            refresh
            verify "$scenario" "$resolved"
            restore "$scenario"
            apply "$scenario" "case-$case.tsv"
            apply "$scenario" "recover-$case.tsv"
            verify "$scenario" "$resolved"
            status=0
            btrfs rescue super-recover -y /dev/vda < /dev/null || status=$?
            test "$status" -eq 0
        fi
        total=$((total + 1))
    done < "$scenario/cases.tsv"
    restore "$scenario"
    cmp /dev/vda /dev/vdb
    # Linux continues read-write from the scenario's newest root, cleaning any
    # orphans it left. A free-space tree must stay enabled; the other profiles
    # keep no space cache.
    awk -F '\t' '{ printf "%d\t%d\t%s\t0\n", $2 / 512, $3 / 512, $4 }' "$scenario/writes.tsv" > "$scenario/all.tsv"
    apply "$scenario" all.tsv
    verify "$scenario" "$(cat "$scenario/final.txt")"
    options=nospace_cache
    if btrfs inspect-internal dump-super /dev/vda | grep -q FREE_SPACE_TREE_VALID; then
        options=defaults
    fi
    mount -t btrfs -o "$options" /dev/vda /mnt
    printf 'Linux accepted the new root\n' > /mnt/after-machlin
    btrfs filesystem sync /mnt
    umount /mnt
    btrfs check --readonly /dev/vda < /dev/null
    test "$(btrfs inspect-internal dump-tree -t 5 /dev/vda | grep -c ORPHAN_ITEM || true)" = 0
    dd if=/dev/vdb of=/dev/vda bs=1048576 conv=notrunc 2>/dev/null
    refresh
    cmp /dev/vda /dev/vdb
    echo "BTRFS_TRANSACTION_SCENARIO:$name"
done
echo "BTRFS_TRANSACTION_NAMESPACE_CHECKS:$namespace_checks"
test "$namespace_checks" = @NAMESPACE_CHECKS@
echo BTRFS_TRANSACTION_PASS:$total
trap - EXIT
poweroff -f
