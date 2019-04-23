#!/usr/local/bin/python2
#
# Copyright (c) 2014 The FreeBSD Foundation
# All rights reserved.
#
# This software was developed by John-Mark Gurney under
# the sponsorship from the FreeBSD Foundation.
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1.  Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.
# 2.  Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
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

from __future__ import print_function

import binascii
import errno
import cryptodev
import itertools
import os
import struct
import unittest
from cryptodev import *
from glob import iglob

katdir = '/usr/local/share/nist-kat'

def katg(base, glob):
    assert os.path.exists(os.path.join(katdir, base)), "Please 'pkg install nist-kat'"
    return iglob(os.path.join(katdir, base, glob))

aesmodules = [ 'cryptosoft0', 'aesni0', 'ccr0', 'ccp0' ]
desmodules = [ 'cryptosoft0', ]
shamodules = [ 'cryptosoft0', 'aesni0', 'ccr0', 'ccp0' ]

def GenTestCase(cname):
    try:
        crid = cryptodev.Crypto.findcrid(cname)
    except IOError:
        return None

    class GendCryptoTestCase(unittest.TestCase):
        ###############
        ##### AES #####
        ###############
        @unittest.skipIf(cname not in aesmodules, 'skipping AES on %s' % (cname))
        def test_xts(self):
            for i in katg('XTSTestVectors/format tweak value input - data unit seq no', '*.rsp'):
                self.runXTS(i, cryptodev.CRYPTO_AES_XTS)

        @unittest.skipIf(cname not in aesmodules, 'skipping AES on %s' % (cname))
        def test_cbc(self):
            for i in katg('KAT_AES', 'CBC[GKV]*.rsp'):
                self.runCBC(i)

        @unittest.skipIf(cname not in aesmodules, 'skipping AES on %s' % (cname))
        def test_gcm(self):
            for i in katg('gcmtestvectors', 'gcmEncrypt*'):
                self.runGCM(i, 'ENCRYPT')

            for i in katg('gcmtestvectors', 'gcmDecrypt*'):
                self.runGCM(i, 'DECRYPT')

        _gmacsizes = { 32: cryptodev.CRYPTO_AES_256_NIST_GMAC,
            24: cryptodev.CRYPTO_AES_192_NIST_GMAC,
            16: cryptodev.CRYPTO_AES_128_NIST_GMAC,
        }
        def runGCM(self, fname, mode):
            curfun = None
            if mode == 'ENCRYPT':
                swapptct = False
                curfun = Crypto.encrypt
            elif mode == 'DECRYPT':
                swapptct = True
                curfun = Crypto.decrypt
            else:
                raise RuntimeError('unknown mode: %r' % repr(mode))

            columns = [ 'Count', 'Key', 'IV', 'CT', 'AAD', 'Tag', 'PT', ]
            with cryptodev.KATParser(fname, columns) as parser:
                self.runGCMWithParser(parser, mode)

        def runGCMWithParser(self, parser, mode):
            for _, lines in next(parser):
                for data in lines:
                    curcnt = int(data['Count'])
                    cipherkey = binascii.unhexlify(data['Key'])
                    iv = binascii.unhexlify(data['IV'])
                    aad = binascii.unhexlify(data['AAD'])
                    tag = binascii.unhexlify(data['Tag'])
                    if 'FAIL' not in data:
                        pt = binascii.unhexlify(data['PT'])
                    ct = binascii.unhexlify(data['CT'])

                    if len(iv) != 12:
                        # XXX - isn't supported
                        continue

                    try:
                        c = Crypto(cryptodev.CRYPTO_AES_NIST_GCM_16,
                            cipherkey,
                            mac=self._gmacsizes[len(cipherkey)],
                            mackey=cipherkey, crid=crid)
                    except EnvironmentError as e:
                        # Can't test algorithms the driver does not support.
                        if e.errno != errno.EOPNOTSUPP:
                            raise
                        continue

                    if mode == 'ENCRYPT':
                        try:
                            rct, rtag = c.encrypt(pt, iv, aad)
                        except EnvironmentError as e:
                            # Can't test inputs the driver does not support.
                            if e.errno != errno.EINVAL:
                                raise
                            continue
                        rtag = rtag[:len(tag)]
                        data['rct'] = binascii.hexlify(rct)
                        data['rtag'] = binascii.hexlify(rtag)
                        self.assertEqual(rct, ct, repr(data))
                        self.assertEqual(rtag, tag, repr(data))
                    else:
                        if len(tag) != 16:
                            continue
                        args = (ct, iv, aad, tag)
                        if 'FAIL' in data:
                            self.assertRaises(IOError,
                                c.decrypt, *args)
                        else:
                            try:
                                rpt, rtag = c.decrypt(*args)
                            except EnvironmentError as e:
                                # Can't test inputs the driver does not support.
                                if e.errno != errno.EINVAL:
                                    raise
                                continue
                            data['rpt'] = binascii.unhexlify(rpt)
                            data['rtag'] = binascii.unhexlify(rtag)
                            self.assertEqual(rpt, pt,
                                repr(data))

        def runCBC(self, fname):
            columns = [ 'COUNT', 'KEY', 'IV', 'PLAINTEXT', 'CIPHERTEXT', ]
            with cryptodev.KATParser(fname, columns) as parser:
                self.runCBCWithParser(parser)

        def runCBCWithParser(self, parser):
            curfun = None
            for mode, lines in next(parser):
                if mode == 'ENCRYPT':
                    swapptct = False
                    curfun = Crypto.encrypt
                elif mode == 'DECRYPT':
                    swapptct = True
                    curfun = Crypto.decrypt
                else:
                    raise RuntimeError('unknown mode: %r' % repr(mode))

                for data in lines:
                    curcnt = int(data['COUNT'])
                    cipherkey = binascii.unhexlify(data['KEY'])
                    iv = binascii.unhexlify(data['IV'])
                    pt = binascii.unhexlify(data['PLAINTEXT'])
                    ct = binascii.unhexlify(data['CIPHERTEXT'])

                    if swapptct:
                        pt, ct = ct, pt
                    # run the fun
                    c = Crypto(cryptodev.CRYPTO_AES_CBC, cipherkey, crid=crid)
                    r = curfun(c, pt, iv)
                    self.assertEqual(r, ct)

        def runXTS(self, fname, meth):
            columns = [ 'COUNT', 'DataUnitLen', 'Key', 'DataUnitSeqNumber', 'PT',
                        'CT']
            with cryptodev.KATParser(fname, columns) as parser:
                self.runXTSWithParser(parser, meth)

        def runXTSWithParser(self, parser, meth):
            curfun = None
            for mode, lines in next(parser):
                if mode == 'ENCRYPT':
                    swapptct = False
                    curfun = Crypto.encrypt
                elif mode == 'DECRYPT':
                    swapptct = True
                    curfun = Crypto.decrypt
                else:
                    raise RuntimeError('unknown mode: %r' % repr(mode))

                for data in lines:
                    curcnt = int(data['COUNT'])
                    nbits = int(data['DataUnitLen'])
                    cipherkey = binascii.unhexlify(data['Key'])
                    iv = struct.pack('QQ', int(data['DataUnitSeqNumber']), 0)
                    pt = binascii.unhexlify(data['PT'])
                    ct = binascii.unhexlify(data['CT'])

                    if nbits % 128 != 0:
                        # XXX - mark as skipped
                        continue
                    if swapptct:
                        pt, ct = ct, pt
                    # run the fun
                    try:
                        c = Crypto(meth, cipherkey, crid=crid)
                        r = curfun(c, pt, iv)
                    except EnvironmentError as e:
                        # Can't test hashes the driver does not support.
                        if e.errno != errno.EOPNOTSUPP:
                            raise
                        continue
                    self.assertEqual(r, ct)

        ###############
        ##### DES #####
        ###############
        @unittest.skipIf(cname not in desmodules, 'skipping DES on %s' % (cname))
        def test_tdes(self):
            for i in katg('KAT_TDES', 'TCBC[a-z]*.rsp'):
                self.runTDES(i)

        def runTDES(self, fname):
            columns = [ 'COUNT', 'KEYs', 'IV', 'PLAINTEXT', 'CIPHERTEXT', ]
            with cryptodev.KATParser(fname, columns) as parser:
                self.runTDESWithParser(parser)

        def runTDESWithParser(self, parser):
            curfun = None
            for mode, lines in next(parser):
                if mode == 'ENCRYPT':
                    swapptct = False
                    curfun = Crypto.encrypt
                elif mode == 'DECRYPT':
                    swapptct = True
                    curfun = Crypto.decrypt
                else:
                    raise RuntimeError('unknown mode: %r' % repr(mode))

                for data in lines:
                    curcnt = int(data['COUNT'])
                    key = data['KEYs'] * 3
                    cipherkey = binascii.unhexlify(key)
                    iv = binascii.unhexlify(data['IV'])
                    pt = binascii.unhexlify(data['PLAINTEXT'])
                    ct = binascii.unhexlify(data['CIPHERTEXT'])

                    if swapptct:
                        pt, ct = ct, pt
                    # run the fun
                    c = Crypto(cryptodev.CRYPTO_3DES_CBC, cipherkey, crid=crid)
                    r = curfun(c, pt, iv)
                    self.assertEqual(r, ct)

        ###############
        ##### SHA #####
        ###############
        @unittest.skipIf(cname not in shamodules, 'skipping SHA on %s' % str(cname))
        def test_sha(self):
            # SHA not available in software
            pass
            #for i in iglob('SHA1*'):
            #    self.runSHA(i)

        @unittest.skipIf(cname not in shamodules, 'skipping SHA on %s' % str(cname))
        def test_sha1hmac(self):
            for i in katg('hmactestvectors', 'HMAC.rsp'):
                self.runSHA1HMAC(i)

        def runSHA1HMAC(self, fname):
            columns = [ 'Count', 'Klen', 'Tlen', 'Key', 'Msg', 'Mac' ]
            with cryptodev.KATParser(fname, columns) as parser:
                self.runSHA1HMACWithParser(parser)

        def runSHA1HMACWithParser(self, parser):
            for hashlength, lines in next(parser):
                # E.g., hashlength will be "L=20" (bytes)
                hashlen = int(hashlength.split("=")[1])

                blocksize = None
                if hashlen == 20:
                    alg = cryptodev.CRYPTO_SHA1_HMAC
                    blocksize = 64
                elif hashlen == 28:
                    alg = cryptodev.CRYPTO_SHA2_224_HMAC
                    blocksize = 64
                elif hashlen == 32:
                    alg = cryptodev.CRYPTO_SHA2_256_HMAC
                    blocksize = 64
                elif hashlen == 48:
                    alg = cryptodev.CRYPTO_SHA2_384_HMAC
                    blocksize = 128
                elif hashlen == 64:
                    alg = cryptodev.CRYPTO_SHA2_512_HMAC
                    blocksize = 128
                else:
                    # Skip unsupported hashes
                    # Slurp remaining input in section
                    for data in lines:
                        continue
                    continue

                for data in lines:
                    key = binascii.unhexlify(data['Key'])
                    msg = binascii.unhexlify(data['Msg'])
                    mac = binascii.unhexlify(data['Mac'])
                    tlen = int(data['Tlen'])

                    if len(key) > blocksize:
                        continue

                    try:
                        c = Crypto(mac=alg, mackey=key,
                            crid=crid)
                    except EnvironmentError as e:
                        # Can't test hashes the driver does not support.
                        if e.errno != errno.EOPNOTSUPP:
                            raise
                        continue

                    _, r = c.encrypt(msg, iv="")

                    # A limitation in cryptodev.py means we
                    # can only store MACs up to 16 bytes.
                    # That's good enough to validate the
                    # correct behavior, more or less.
                    maclen = min(tlen, 16)
                    self.assertEqual(r[:maclen], mac[:maclen], "Actual: " + \
                        repr(r[:maclen].encode("hex")) + " Expected: " + repr(data))

    return GendCryptoTestCase

cryptosoft = GenTestCase('cryptosoft0')
aesni = GenTestCase('aesni0')
ccr = GenTestCase('ccr0')
ccp = GenTestCase('ccp0')

if __name__ == '__main__':
    unittest.main()
