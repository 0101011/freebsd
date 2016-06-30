#!/bin/sh
#-
# Copyright (c) 2016 The FreeBSD Foundation
# All rights reserved.
#
# This software was developed by Björn Zeeb under
# the sponsorship from the FreeBSD Foundation.
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

case `id -u` in
0)	;;
*)	echo "ERROR: Must be run as superuser." >&2
	exit 2
esac

wait_enter()
{
	local line
	
	echo -n "Press Enter to continue.."
	read line
}

# Private debugging.
sysctl security.jail.vimage_debug_memory=2

id=`jail -i -c vnet persist`
jexec ${id} ifconfig lo0 up
jexec ${id} ifconfig lo0 127.0.0.1/8 up
jexec ${id} route add 192.0.20/24 127.0.0.1
jexec ${id} route add 10.0.0.0/8 127.0.0.1
jexec ${id} route add 172.16.0.0/16 127.0.0.1
jexec ${id} route add 192.168.0.0/16 127.0.0.1
jexec ${id} ifconfig lo0 down
jail -r ${id}

# end
