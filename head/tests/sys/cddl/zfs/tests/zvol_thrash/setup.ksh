#!/usr/local/bin/ksh93 -p
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# ident	"@(#)setup.ksh	1.3	09/05/19 SMI"
#
# $FreeBSD$

. ${STF_SUITE}/include/libtest.kshlib
. ${STF_SUITE}/include/libsas.kshlib

verify_runnable "global"
echo "list of disks: $DISKS"

# Make sure that all of the disks that we've been given are attached to a
# SAS expander, and that we can find the phy they're attached to.  This
# function will cause the script to exit if it fails.
for disk in $DISKS
do
	find_verify_sas_disk $disk
done

# Rotate logs now, because this test can generate a great volume of log entries
newsyslog

log_pass
