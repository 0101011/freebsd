#!/bin/sh

#
# Copyright (c) 2013 EMC Corp.
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

# "panic: handle_written_inodeblock: live inodedep 0xcc731200" seen.
# http://people.freebsd.org/~pho/stress/log/umountf7.txt
# Problem only seen with SU+J.

[ `id -u ` -ne 0 ] && echo "Must be root!" && exit 1

. ../default.cfg

dir=/tmp
odir=`pwd`
cd $dir
sed '1,/^EOF/d' < $odir/$0 > $dir/mv4.c
cc -o mv4  -Wall -Wextra mv4.c -lpthread || exit 1
rm -f mv4.c
cd $odir

mount | grep "on $mntpoint " | grep -q /dev/md && umount -f $mntpoint
mdconfig -l | grep -q md$mdstart &&  mdconfig -d -u $mdstart
mdconfig -a -t swap -s 3g -u $mdstart || exit 1
bsdlabel -w md$mdstart auto
newfs -j md${mdstart}$part > /dev/null
mount /dev/md${mdstart}$part $mntpoint

daemon sh -c '(cd ../testcases/swap; ./swap -t 2m -i 4)'
parallel=4
for j in `jot $parallel`; do
	[ -d $mntpoint/$j ] || mkdir $mntpoint/$j
done
for j in `jot $parallel`; do
	(cd $mntpoint/$j; /tmp/mv4 100000) &
done
sleep 60
umount -f $mntpoint
killall mv4
for j in `jot $parallel`; do
	wait
done
while ps auxww | grep -v grep | grep -qw swap; do
	killall -9 swap 2>/dev/null
done
find $mntpoint -type f

while mount | grep $mntpoint | grep -q /dev/md; do
	umount $mntpoint || sleep 1
done
mdconfig -d -u $mdstart
rm -f /tmp/mv4
exit

EOF
#include <err.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

pid_t pid;
volatile int n, n2;
int mx;

void *
cr(void *arg __unused)
{
	char file[80];
	int fd, i;

	for (i = 0; i < mx; i++) {
		snprintf(file, sizeof(file), "f%06d.%06d", pid, i);
		if ((fd = open(file, O_RDWR | O_CREAT | O_TRUNC, 0600)) < 0)
			err(1, "open(%s)", file);
		close(fd);
		n++;
	}
        return (0);

}

void *
mv(void *arg __unused)
{
	char from[80], to[80];
	int i;

	for (i = 0; i < mx; i++) {
		while (n == -1 || i > n)
			pthread_yield();
		snprintf(from, sizeof(from), "f%06d.%06d", pid, i);
		snprintf(to  , sizeof(to  ), "g%06d.%06d", pid, i);
		if (rename(from, to) == -1)
			warn("rename(%s, %s)", from, to);
		n2++;
	}

        return (0);
}

void *
rm(void *arg __unused)
{
	char file[80];
	int i;

	for (i = 0; i < mx; i++) {
		while (n2 == -1 || i > n2)
			pthread_yield();
		snprintf(file, sizeof(file), "g%06d.%06d", pid, i);
		if (unlink(file) == -1)
			warn("unlink(%s)", file);
	}

        return (0);
}

int
main(int argc, char **argv)
{
        pthread_t rp[3];
	int i;

	if (argc != 2)
		errx(1, "Usage: %s <number of files>", argv[0]);
	mx = atoi(argv[1]);
	n = n2 = -1;
	pid = getpid();

	if (pthread_create(&rp[0], NULL, cr, NULL) != 0)
		err(1, "pthread_create");
	usleep(arc4random() % 1000);
	if (pthread_create(&rp[1], NULL, mv, NULL) != 0)
		err(1, "pthread_mv");
	usleep(arc4random() % 1000);
	if (pthread_create(&rp[2], NULL, rm, NULL) != 0)
		perror("pthread_rm");

        for (i = 0; i < 3; i++)
                pthread_join(rp[i], NULL);

	return (0);
}
