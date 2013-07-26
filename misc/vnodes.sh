#!/bin/sh

#
# Copyright (c) 2012 Peter Holm <pho@FreeBSD.org>
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

# Demonstrate vnode leak

# Test scenario by Petr Salinger <Petr Salinger seznam cz>

. ../default.cfg

here=`pwd`
cd /tmp
sed '1,/^EOF/d' < $here/$0 > vnodes.c
cc -o vnodes -Wall -O2 vnodes.c
rm -f vnodes.c

old=`sysctl vfs.numvnodes | tail -1 | sed 's/.*: //'`
/tmp/vnodes
new=`sysctl vfs.numvnodes | tail -1 | sed 's/.*: //'`
[ $((new - old)) -gt 100 ] && echo "FAIL vnode leak"

rm -f /tmp/vnodes
exit 0
EOF
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>


char dname[]= "/dev/ptyrX";

void leak(void)
{
	int i, fd;

	if (fork() == 0) {
		for (i = '0'; i < '9'; i ++) {
			dname[9] = i;
			fd = open(dname, O_RDWR);
		}
		exit(0);
	}
	wait(NULL);
}

int main()
{
	int i;

	for (i = 0 ;i < 100000; i++) {
		leak();
	}
	return 0;
}
