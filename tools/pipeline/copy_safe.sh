#!/usr/bin/env bash
# Never write in place into a file a running program may have mapped (T-304, journal 018).
#
# `cp -f` onto a library or an executable that a running process has mapped truncates the file the
# process's pages come from: the kernel discards those pages, private modified ones included, and the
# next access faults the *new* bytes in - for a shared library that means its relocated data (the table
# its public entry points jump through) is gone, and the next call jumps to a link-time address. That is
# the crash at 0x66F80 (SDL_SavePNG's link-time address, in the run that was taking a screenshot when a
# build copied libSDL3.so.0 over itself). Copy beside the target and *rename* over it: a running process
# keeps its own inode. And do nothing at all when the two files are already identical.
#
# Source this file and use copy_safe <src> <dst> / patchelf_safe <file> <rpath>.
# Works on the host and inside the build container (the repository is mounted at the same path).

copy_safe() {
	local src=$1 dst=$2
	if [[ -f "$dst" ]] && cmp -s "$src" "$dst"; then
		return 0
	fi
	cp -f "$src" "$dst.new.$$" && mv -f "$dst.new.$$" "$dst"
}

patchelf_safe() {
	local file=$1 rpath=$2
	if [[ "$(patchelf --print-rpath "$file")" == "$rpath" ]]; then
		return 0
	fi
	cp -f "$file" "$file.new.$$" && patchelf --set-rpath "$rpath" "$file.new.$$" && mv -f "$file.new.$$" "$file"
}
