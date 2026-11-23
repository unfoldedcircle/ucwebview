#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unfolded Circle ApS
#
# Writes the golden byte files of the uc.webview channel tests (tests/fixtures/*.bin): method calls as Dart's
# MethodChannel sends them and replies and events as Dart expects them.
#
# The encoder below is an independent implementation of the rules of Flutter's StandardMessageCodec and
# StandardMethodCodec (packages/flutter/lib/src/services/message_codecs.dart): ints as int32 when they fit, else
# int64; float64 aligned to 8 from the start of the message; sizes < 254 in one byte, else 254 + uint16 or
# 255 + uint32; little endian. It is not the C codec under test. The files were also checked against flutter-pi's own
# codec (src/platformchannel.c, platch_encode()/platch_decode()). Once the Dart side exists, a Dart test writes these
# files with the real codec and both sides compare against them.
#
# usage: tests/fixtures/gen-fixtures.py [<dir>]   (default: the directory of this script)
import os
import struct
import sys


class Writer:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v):
        self.b.append(v)

    def align(self, n):
        while len(self.b) % n:
            self.b.append(0)

    def size(self, n):
        if n < 254:
            self.u8(n)
        elif n <= 0xffff:
            self.u8(254)
            self.b += struct.pack('<H', n)
        else:
            self.u8(255)
            self.b += struct.pack('<I', n)

    def value(self, v):
        if v is None:
            self.u8(0)
        elif v is True:
            self.u8(1)
        elif v is False:
            self.u8(2)
        elif isinstance(v, float):
            self.u8(6)
            self.align(8)
            self.b += struct.pack('<d', v)
        elif isinstance(v, int):
            if -2**31 <= v < 2**31:
                self.u8(3)
                self.b += struct.pack('<i', v)
            else:
                self.u8(4)
                self.b += struct.pack('<q', v)
        elif isinstance(v, str):
            e = v.encode('utf-8')
            self.u8(7)
            self.size(len(e))
            self.b += e
        elif isinstance(v, list):
            self.u8(12)
            self.size(len(v))
            for x in v:
                self.value(x)
        elif isinstance(v, dict):
            self.u8(13)
            self.size(len(v))
            for k, x in v.items():
                self.value(k)
                self.value(x)
        else:
            raise TypeError(v)


def call(method, args):
    w = Writer()
    w.value(method)
    w.value(args)
    return bytes(w.b)


def success(v):
    w = Writer()
    w.u8(0)
    w.value(v)
    return bytes(w.b)


def error(code, message, details=None):
    w = Writer()
    w.u8(1)
    w.value(code)
    w.value(message)
    w.value(details)
    return bytes(w.b)


FIXTURES = {
    # calls (Dart -> host)
    'call-hello.bin': call('hello', None),
    'call-create.bin': call('create', {'width': 480, 'height': 800, 'url': 'https://example.com/', 'pixelRatio': 1.0}),
    'call-create-long-url.bin': call('create', {'width': 1, 'height': 1, 'url': 'https://example.com/' + 'a' * 280}),
    'call-touch.bin': call('touch', {'viewId': 1, 'slot': 7, 'type': 1, 'x': 12.5, 'y': 700.25, 'timeMs': 123456}),
    'call-touch-int64.bin': call('touch', {'viewId': 2**40, 'slot': 2**33 + 5, 'type': 3, 'x': -1.0, 'y': 0.0}),
    'call-key.bin': call('key', {'viewId': 3, 'keysym': 0xff0d, 'keycode': 36, 'pressed': True}),
    'call-set-mapped.bin': call('setMapped', {'viewId': 1, 'mapped': False}),
    'call-dispose.bin': call('dispose', {'viewId': 1}),
    'call-listen.bin': call('listen', None),
    # replies and events (host -> Dart)
    'reply-hello-unavailable.bin': success({'protocol': 1, 'available': False, 'reason': 'no library', 'library': None,
                                            'abi': None, 'features': []}),
    'reply-hello-available.bin': success({'protocol': 1, 'available': True, 'reason': None, 'library': None, 'abi': 1,
                                          'features': ['refreshRate', 'newSized']}),
    'reply-create.bin': success({'viewId': 1, 'textureId': 2}),
    'reply-null.bin': success(None),
    'reply-error-no-view.bin': error('noView', 'unknown viewId'),
    'event-load-changed.bin': success({'type': 'loadChanged', 'viewId': 1, 'event': 'finished', 'uri': 'about:blank'}),
    'event-progress.bin': success({'type': 'progress', 'viewId': 1, 'progress': 0.5}),
    'event-frame.bin': success({'type': 'frame', 'viewId': 1, 'width': 480, 'height': 800, 'zeroCopy': True}),
    'event-terminated.bin': success({'type': 'terminated', 'viewId': 2, 'reason': 'crashed'}),
}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    for name, data in FIXTURES.items():
        with open(os.path.join(out, name), 'wb') as f:
            f.write(data)
    print('%d fixtures written to %s' % (len(FIXTURES), out))


if __name__ == '__main__':
    main()
