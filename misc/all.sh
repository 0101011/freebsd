#!/bin/sh

#
# Copyright (c) 2008-2009, 2012-13 Peter Holm <pho@FreeBSD.org>
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.
#
# $FreeBSD$
#

# Run all the scripts in stress2/misc, except these known problems:

# Start of list	Known problems						Seen

# altbufferflushes.sh snapshots + disk full == know problem		20130617
# backingstore.sh
#		g_vfs_done():md6a[WRITE(offset=...)]error = 28		20111220
# backingstore2.sh
#		panic: 43 vncache entries remaining			20111220
# backingstore3.sh
#		g_vfs_done():md6a[WRITE(offset=...)]error = 28		20111230
# dfull.sh	umount stuck in "mount drain"				20111227
# ext2fs.sh	Deadlock						20120510
# fuse2.sh	Deadlock seen						20121129
# gbde.sh	panic: handle_written_inodeblock: Invalid link count...	20131128
# gjournal.sh	kmem_malloc(131072): kmem_map too small			20120626
# gjournal2.sh	
# gjournal3.sh	panic: Journal overflow					20130729
# newfs.sh	Memory modified after free. ... used by inodedep	20111217
# newfs2.sh	umount stuck in ufs					20111226
# nfs2.sh	panic: wrong diroffset					20140219
# nfs9.sh	panic: lockmgr still held				20130503
# nfs10.sh	Deadlock						20130401
# nfs11.sh	Deadlock						20130429
# pmc.sh	NMI ... going to debugger				20111217
# snap5-1.sh	mksnap_ffs deadlock					20111218
# quota2.sh	panic: dqflush: stray dquot				20120221
# quota3.sh	panic: softdep_deallocate_dependencies: unrecovered ...	20111222
# quota6.sh	panic: softdep_deallocate_dependencies: unrecovered ...	20130206
# quota7.sh	panic: dqflush: stray dquot				20120221
# shm_open.sh	panic: kmem_malloc(4096): kmem_map too small		20130504
# snap3.sh	mksnap_ffs stuck in snaprdb				20111226
# snap5.sh	mksnap_ffs stuck in getblk				20111224
# snap6.sh	panic: softdep_deallocate_dependencies: unrecovered ...	20130630
# snap8.sh	panic: softdep_deallocate_dependencies: unrecovered ...	20120630
# suj11.sh	panic: ufsdirhash_newblk: bad offset			20120118
# suj18.sh	panic: Bad tailq NEXT(0xc1e2a6088->tqh_last_s) != NULL	20120213
# suj30.sh	panic: flush_pagedep_deps: MKDIR_PARENT			20121020
# suj34.sh	Various hangs and panics				20131210
# umountf3.sh	KDB: enter: watchdog timeout				20111217
# umountf7.sh	panic: handle_written_inodeblock: live inodedep ...	20131129
# unionfs.sh	insmntque: non-locked vp: xx is not exclusive locked...	20130909
# unionfs2.sh	insmntque: mp-safe fs and non-locked vp is not ...	20111219
# unionfs3.sh	insmntque: mp-safe fs and non-locked vp is not ...	20111216

# Test not to run for other reasons:

# fuzz.sh	A know issue
# newfs3.sh	OK, but runs for a very long time
# statfs.sh	Not very interesting
# syscall.sh	OK, but runs for a very long time
# syscall2.sh	OK, but runs for a very long time
# vunref.sh	No problems ever seen
# vunref2.sh	No problems ever seen

# Snapshots has been disabled on SU+J
# suj15.sh
# suj16.sh
# suj19.sh
# suj20.sh
# suj21.sh
# suj22.sh
# suj24.sh
# suj25.sh
# suj26.sh
# suj27.sh
# suj28.sh

# kevent8.sh	Deadlock seen.						20131017

# End of list

# Suspects:
# ffs_syncvnode2.sh
#		Memory modified after free. ... used by inodedep	20111224

[ `id -u ` -ne 0 ] && echo "Must be root!" && exit 1

args=`getopt acno $*`
[ $? -ne 0 ] && echo "Usage $0 [-a] [-c] [-n] [tests]" && exit 1
set -- $args
for i; do
	case "$i" in
	-a)	all=1		# Run all tests
		shift
		;;
	-c)	rm -f .all.last	# Clear last know test
		shift
		;;
	-n)	noshuffle=1	# Do not shuffle the list of tests
		shift
		;;
	-o)	once=1		# Only run once
		shift
		;;
	--)
		shift
		break
		;;
	esac
done

> .all.log
find . -maxdepth 1 -name .all.last -mtime +12h -delete
touch .all.last
chmod 555 .all.last .all.log
while true; do
	exclude=`sed -n '/^# Start of list/,/^# End of list/p' < $0 |
		cat - all.exclude 2>/dev/null | 
		grep "\.sh" | awk '{print $2}'`
	list=`ls *.sh | egrep -v "all\.sh|cleanup\.sh"`
	[ $# -ne 0 ] && list=$*

	if [ -n "$noshuffle" -a $# -eq 0 ]; then
		last=`cat .all.last`
		if [ -n "$last" ]; then
			list=`echo "$list" | sed "1,/$last/d"`
			echo "Resuming test at `echo "$list" | head -1`"
		fi
	fi
	[ -n "$noshuffle" ] ||
		list=`echo $list | tr '\n' ' ' | ../tools/shuffle | tr ' ' '\n'`

	lst=""
	for i in $list; do
		[ -z "$all" ] && echo $exclude | grep -qw $i && continue
		lst="$lst $i"
	done
	[ -z "$lst" ] && exit

	n1=0
	n2=`echo $lst | wc -w | sed 's/ //g'`
	for i in $lst; do
		n1=$((n1 + 1))
		echo $i > .all.last
		./cleanup.sh
		echo "`date '+%Y%m%d %T'` all: $i" | tee /dev/tty >> .all.log
		printf "`date '+%Y%m%d %T'` all ($n1/$n2): $i\r\n" > /dev/console
		logger "Starting test all: $i"
		sync;sync;sync
		./$i
	done
	[ -n "$once" ] && break
done
