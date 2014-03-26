# $FreeBSD$
#
# Copyright 2013 Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
# * Redistributions of source code must retain the above copyright
#   notice, this list of conditions and the following disclaimer.
# * Redistributions in binary form must reproduce the above copyright
#   notice, this list of conditions and the following disclaimer in the
#   documentation and/or other materials provided with the distribution.
# * Neither the name of Google Inc. nor the names of its contributors
#   may be used to endorse or promote products derived from this software
#   without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# \file setup.sh
# Configures the current machine as an autotest node.

shtk_import cli


# Installs any required packages and ensures they are up-to-date.
install_deps() {
    pkg update
    pkg install -y qemu-devel kyua
    pkg upgrade -y
}


# Builds the source code.
#
# \param ... Arguments to make.  Use to pass variable overrides for the host,
#     such as the location of shtk(1).
build() {
    make -C "$(shtk_cli_dirname)" clean
    make -C "$(shtk_cli_dirname)" all "${@}"
}


# Sets up rc.conf to start autotest_node on boot.
enable_daemon() {
    grep "local_startup.*$(shtk_cli_dirname)/rc.d" /etc/rc.conf \
        || echo "local_startup=\"\${local_startup} $(shtk_cli_dirname)/rc.d\"" \
        >>/etc/rc.conf
    grep "autotest_node_enable=yes" /etc/rc.conf \
        || echo "autotest_node_enable=yes" >>/etc/rc.conf

    "$(shtk_cli_dirname)/rc.d/autotest_node" start
}


# Program entry.
main() {
    install_deps
    build "${@}"
    enable_daemon
}
