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

# The id of the subvolume holding a path: its nearest directory with the
# subvolume root inode number, as btrfs subvolume show reports it.
tree_of() {
    holder=$1
    while [ "$(stat -c '%i' "$holder")" != 256 ]; do
        holder=$(dirname "$holder")
    done
    btrfs subvolume show "$holder" | awk '$1 == "Subvolume" && $2 == "ID:" { print $3 }'
}

# MASK:VALUE: the flags of the path's inode item in its subvolume's tree equal
# VALUE in the bits of MASK. Busybox lsattr shows no NOCOMPRESS flag, and
# `btrfs inspect-internal rootid` needs a writable mount.
check_flags() {
    inode=$(stat -c '%i' "$1")
    flags=$(btrfs inspect-internal dump-tree -t "$(tree_of "$1")" /dev/vda |
        awk -v key="key ($inode INODE_ITEM 0)" '
        index($0, key) && index($0, "itemoff") { found = 1 }
        found && match($0, /flags 0x[0-9a-f]+/) { print substr($0, RSTART + 6, RLENGTH - 6); exit }')
    test -n "$flags" && test $((flags & ${2%%:*})) -eq $((${2#*:}))
}

# inode|extended: the path's name is held by its inode's INODE_REF item for
# the parent directory, or by an INODE_EXTREF item naming that parent.
check_reference() {
    inode=$(stat -c '%i' "$1")
    parent=$(stat -c '%i' "$(dirname "$1")")
    kind=$(btrfs inspect-internal dump-tree -t "$(tree_of "$1")" /dev/vda | awk -v inode="$inode" \
        -v parent="$parent" -v name="$(basename "$1")" '
        $1 == "item" && $3 == "key" {
            object = substr($4, 2); type = $5; offset = $6; sub(/\)$/, "", offset); next }
        $1 == "index" && object == inode && $NF == name && $(NF - 1) == "name:" {
            if (type == "INODE_REF" && offset == parent) { print "inode"; exit }
            if (type == "INODE_EXTREF" && $3 == "parent" && $4 == parent) {
                print "extended"; exit } }')
    test "$kind" = "$2"
}

# ro|rw:SOURCE: the path is a subvolume with that read-only flag, a snapshot
# of the subvolume at SOURCE or of none (-), as btrfs subvolume show reports.
check_subvolume() {
    btrfs subvolume show "$1" > /tmp/show || return 1
    flags=$(awk '$1 == "Flags:" { print $2 }' /tmp/show)
    parent=$(awk '$1 == "Parent" && $2 == "UUID:" { print $3 }' /tmp/show)
    case "${2%%:*}" in
    ro) test "$flags" = readonly ;;
    rw) test "$flags" = - ;;
    *) false ;;
    esac || return 1
    source=${2#*:}
    if [ "$source" = - ]; then
        test "$parent" = -
    else
        btrfs subvolume show "/mnt$source" > /tmp/source &&
            test "$parent" = "$(awk '$1 == "UUID:" { print $2 }' /tmp/source)"
    fi
}

# A directory's names exactly as stored, one per line in byte order. Shell
# globbing keeps every byte; BusyBox ls rewrites names it cannot print.
list_names() {
    for name in "$1"/* "$1"/.[!.]* "$1"/..?*; do
        if [ -e "$name" ] || [ -L "$name" ]; then
            printf '%s\n' "${name##*/}"
        fi
    done | LC_ALL=C sort
}

# CODEC:REGULAR:INLINE: the file's extents compressed with CODEC, by kind, in
# Linux's own tree dump, and none with another codec.
check_compressed() {
    inode=$(stat -c '%i' "$1")
    codec=1
    [ "${2%%:*}" = zstd ] && codec=3
    counts=$(btrfs inspect-internal dump-tree -t "$(tree_of "$1")" /dev/vda |
        awk -v inode="$inode" -v codec="$codec" '
        $1 == "item" && $3 == "key" { inside = substr($4, 2) == inode && $5 == "EXTENT_DATA"; next }
        inside && /inline extent data size/ && match($0, /compression [0-9]+/) {
            c = substr($0, RSTART + 12, RLENGTH - 12) + 0
            if (c == codec) inline++; else if (c != 0) other++ }
        inside && $1 == "extent" && $2 == "compression" {
            c = $3 + 0
            if (c == codec) regular++; else if (c != 0) other++ }
        END { printf "%d:%d:%d", regular, inline, other }')
    test "$counts" = "${2#*:}:0"
}

# REGULAR:PREALLOC:DISTINCT: the file's regular and preallocated extent items
# and its distinct disk extents in Linux's own tree dump, which prints the disk
# range of a preallocated item as "prealloc data disk byte".
check_extents() {
    inode=$(stat -c '%i' "$1")
    counts=$(btrfs inspect-internal dump-tree -t "$(tree_of "$1")" /dev/vda |
        awk -v inode="$inode" '
        $1 == "item" && $3 == "key" { inside = substr($4, 2) == inode && $5 == "EXTENT_DATA"; next }
        inside && $1 == "generation" && $3 == "type" { type = $4 }
        inside && ($1 == "extent" || $1 == "prealloc") && $2 == "data" && $3 == "disk" &&
            $4 == "byte" && $5 != 0 {
            if (type == 1) regular++; else if (type == 2) prealloc++
            disks[$5] = 1 }
        END { for (d in disks) distinct++; printf "%d:%d:%d", regular, prealloc, distinct }')
    test "$counts" = "$2" || { echo "extents of $1: $counts" >&2; return 1; }
}

# A NODATACOW overwrite reaches the disk before its commit: a state of commit
# $commit resolving to the previous stage may hold, device sector by sector, the
# old or the new bytes inside the scenario's volatile ranges (volatile.tsv).
volatile_file() {
    [ -f "$1/volatile.tsv" ] && [ -n "$commit" ] && [ "$2" = $((commit - 1)) ] || return 1
    new=$(awk -F '\t' -v stage="$commit" -v path="$3" \
        '$1 == stage && $2 == "file" && $3 == path { print $5; exit }' "$1/namespace.tsv")
    [ -n "$new" ] && test "$(stat -c %s "$4")" = "$(stat -c %s "$1/$5")" || return 1
    cmp -l "$4" "$1/$5" > /tmp/old-diff || true
    cmp -l "$4" "$1/$new" > /tmp/new-diff || true
    awk -v commit="$commit" -v path="$3" -v sector=512 '
        BEGIN { n = 0 }
        FILENAME ~ /volatile.tsv$/ {
            if ($1 == commit && $2 == path) { start[n] = $3; end[n] = $3 + $4; n++ }
            next }
        FILENAME == "/tmp/old-diff" {
            byte = $1 - 1; inside = 0
            for (i = 0; i < n; i++) if (byte >= start[i] && byte < end[i]) inside = 1
            if (!inside) bad = 1
            old[int(byte / sector)] = 1; next }
        { changed[int(($1 - 1) / sector)] = 1 }
        END { for (s in old) if (s in changed) bad = 1; exit bad }' \
        "$1/volatile.tsv" /tmp/old-diff /tmp/new-diff
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
        file) test -f "$target" && test ! -L "$target" &&
            { cmp -s "$target" "$1/$payload" || volatile_file "$1" "$2" "$path" "$target" "$payload"; } ;;
        symlink) test -L "$target" && test "$(readlink "$target")" = "$(cat "$1/$payload")" ;;
        dir) test -d "$target" && list_names "$target" > /tmp/listing &&
            cmp /tmp/listing "$1/$payload" && test "$(stat -c '%s' "$target")" = "$arg" ;;
        same) test "$(stat -c '%d:%i' "$target")" = "$(stat -c '%d:%i' "/mnt$arg")" ;;
        xattr) getfattr --only-values -n "$arg" "$target" > /tmp/value &&
            cmp /tmp/value "$1/$payload" ;;
        noxattr) ! getfattr -n "$arg" "$target" > /dev/null 2>&1 ;;
        stat) test "$(stat -c '%f:%u:%g:%h' "$target")" = "$arg" ;;
        device) test "$(stat -c '%t:%T' "$target")" = "$arg" ;;
        flags) check_flags "$target" "$arg" ;;
        feature) btrfs inspect-internal dump-super /dev/vda | grep -qw "$arg" ;;
        times) test "$(stat -c '%X:%Y' "$target")" = "$arg" ;;
        reference) check_reference "$target" "$arg" ;;
        subvolume) check_subvolume "$target" "$arg" ;;
        subvolumes) btrfs subvolume list /mnt | awk '{ print $NF }' | LC_ALL=C sort > /tmp/listing &&
            cmp /tmp/listing "$1/$payload" ;;
        deleted) test "$(btrfs subvolume list -d /mnt | wc -l)" -eq "$arg" ;;
        compressed) check_compressed "$target" "$arg" ;;
        extents) check_extents "$target" "$arg" ;;
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
commit=
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
    commit=
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
    # Linux's cleaner drops what this implementation's deletions left.
    btrfs subvolume sync /mnt
    test "$(btrfs subvolume list -d /mnt | wc -l)" -eq 0
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
