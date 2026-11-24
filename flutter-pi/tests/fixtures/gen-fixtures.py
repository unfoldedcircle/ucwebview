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
# codec (src/platformchannel.c, platch_encode()/platch_decode()). manifest.json lists every file with its kind,
# channel, method or event and the decoded value as JSON (PROTOCOL.md, "Golden fixtures"): a Dart contract test encodes
# each value with the real codec and compares the bytes, and decodes each file and compares the value.
#
# usage: tests/fixtures/gen-fixtures.py [<dir>]   (default: the directory of this script)
import json
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


HEX = '0123456789abcdef' * 4  # a SHA-256 fingerprint as 64 hex digits

# Every message of the protocol (PROTOCOL.md), name -> (kind, channel, name of the method or event, value).
# kind: "call" (Dart -> host: [method, arguments]), "reply" (host -> Dart, success envelope: the result), "error"
# (host -> Dart, error envelope: [code, message, details]), "event" (host -> Dart on the event channel, a success
# envelope: the event map). The C tests (tests/test-protocol.c, tests/test-host.c) produce or parse each of them.
C = 'uc.webview/control'
E = 'uc.webview/events'
FIXTURES = {
    # ---- calls (Dart -> host) ----
    'call-hello.bin': ('call', C, 'hello', None),
    'call-initialize.bin': ('call', C, 'initialize', {'refreshRate': 48.61}),
    'call-set-refresh-rate.bin': ('call', C, 'setRefreshRate', {'hz': 50.0}),
    'call-set-color-scheme.bin': ('call', C, 'setColorScheme', {'scheme': 'dark'}),
    'call-set-tls-errors-policy.bin': ('call', C, 'setTlsErrorsPolicy', {'ignore': True}),
    'call-allow-certificate.bin': ('call', C, 'allowCertificate', {'host': '192.168.1.20', 'sha256': HEX}),
    'call-set-cookies.bin': ('call', C, 'setCookies', {
        'url': 'https://example.com/',
        'cookies': [
            {'name': 'session', 'value': 's3cr3t-cookie', 'domain': 'example.com', 'path': '/', 'secure': True,
             'httpOnly': True, 'expires': 1900000000},
            {'name': 'lang', 'value': 'en', 'domain': None, 'path': None, 'secure': False, 'httpOnly': False,
             'expires': None},
        ]}),
    'call-clear-data.bin': ('call', C, 'clearData', {'scope': 'all'}),
    'call-stats.bin': ('call', C, 'stats', None),
    'call-create.bin': ('call', C, 'create', {'width': 480, 'height': 800, 'url': 'https://example.com/',
                                              'pixelRatio': 1.0}),
    'call-create-full.bin': ('call', C, 'create', {'width': 480, 'height': 800, 'url': 'https://example.com/',
                                                   'pixelRatio': 1.5, 'refreshRate': 48.61, 'pinchZoom': False,
                                                   'requests': True}),
    'call-create-long-url.bin': ('call', C, 'create', {'width': 1, 'height': 1, 'url': 'https://example.com/' + 'a' * 280}),
    'call-dispose.bin': ('call', C, 'dispose', {'viewId': 1}),
    'call-resize.bin': ('call', C, 'resize', {'viewId': 1, 'width': 800, 'height': 480}),
    'call-set-mapped.bin': ('call', C, 'setMapped', {'viewId': 1, 'mapped': False}),
    'call-set-focused.bin': ('call', C, 'setFocused', {'viewId': 1, 'focused': True}),
    'call-set-scale.bin': ('call', C, 'setScale', {'viewId': 1, 'scale': 1.5}),
    'call-set-background-color.bin': ('call', C, 'setBackgroundColor', {'viewId': 1, 'color': 0xff336699}),
    'call-set-frame-hold.bin': ('call', C, 'setFrameHold', {'viewId': 1, 'hold': True}),
    'call-load-uri.bin': ('call', C, 'loadUri', {'viewId': 1, 'uri': 'https://user:pw@example.com/a?token=t0k#f'}),
    'call-load-html.bin': ('call', C, 'loadHtml', {'viewId': 1, 'html': '<p>Hi</p>', 'baseUri': 'https://example.com/'}),
    'call-reload.bin': ('call', C, 'reload', {'viewId': 1}),
    'call-stop.bin': ('call', C, 'stop', {'viewId': 1}),
    'call-go-back.bin': ('call', C, 'goBack', {'viewId': 1}),
    'call-go-forward.bin': ('call', C, 'goForward', {'viewId': 1}),
    'call-get-state.bin': ('call', C, 'getState', {'viewId': 1}),
    'call-run-javascript.bin': ('call', C, 'runJavaScript', {'viewId': 1, 'script': 'window.scrollTo(0, 0)'}),
    'call-set-zoom.bin': ('call', C, 'setZoom', {'viewId': 1, 'zoom': 1.25}),
    'call-set-magnification.bin': ('call', C, 'setMagnification', {'viewId': 1, 'scale': 2.0, 'x': 240.0, 'y': 400.0}),
    'call-when-rendered.bin': ('call', C, 'whenRendered', {'viewId': 1}),
    'call-touch.bin': ('call', C, 'touch', {'viewId': 1, 'slot': 7, 'type': 1, 'x': 12.5, 'y': 700.25, 'timeMs': 123456}),
    'call-touch-int64.bin': ('call', C, 'touch', {'viewId': 2**40, 'slot': 2**33 + 5, 'type': 3, 'x': -1.0, 'y': 0.0}),
    'call-pointer-button.bin': ('call', C, 'pointerButton', {'viewId': 1, 'button': 1, 'pressed': True, 'x': 10.0,
                                                             'y': 20.0, 'modifiers': 2, 'timeMs': 1000}),
    'call-pointer-move.bin': ('call', C, 'pointerMove', {'viewId': 1, 'x': 11.5, 'y': 21.5, 'modifiers': 256,
                                                         'timeMs': 1010}),
    'call-scroll.bin': ('call', C, 'scroll', {'viewId': 1, 'dx': 0.0, 'dy': 2.0, 'x': 240.0, 'y': 400.0,
                                              'precise': False, 'timeMs': 1020}),
    'call-key.bin': ('call', C, 'key', {'viewId': 3, 'keysym': 0xff0d, 'keycode': 36, 'pressed': True}),
    'call-key-full.bin': ('call', C, 'key', {'viewId': 1, 'keysym': 0xff09, 'keycode': 23, 'pressed': True,
                                             'modifiers': 2, 'timeMs': 1030}),
    'call-ime-commit.bin': ('call', C, 'imeCommit', {'viewId': 1, 'text': 'Gr\u00fcezi \U0001f44b'}),
    'call-ime-preedit.bin': ('call', C, 'imePreedit', {'viewId': 1, 'text': 'ab', 'cursor': 2}),
    'call-ime-delete-surrounding.bin': ('call', C, 'imeDeleteSurrounding', {'viewId': 1, 'offset': -1, 'count': 1}),
    'call-reply-dialog.bin': ('call', C, 'replyDialog', {'viewId': 1, 'requestId': 1, 'accept': True,
                                                         'text': 'Alice'}),
    'call-reply-auth.bin': ('call', C, 'replyAuth', {'viewId': 1, 'requestId': 2, 'user': 'alice',
                                                     'password': 'p4ssw0rd'}),
    'call-set-origin-headers.bin': ('call', C, 'setOriginHeaders', {
        'viewId': 1, 'origin': 'https://example.com', 'headers': {'Authorization': 'Bearer t0ken', 'X-Api-Key': 'k3y'}}),
    'call-set-user-agent.bin': ('call', C, 'setUserAgent', {'viewId': 1, 'preset': 'custom', 'custom': 'MyAgent/1.0'}),
    'call-set-device-api.bin': ('call', C, 'setDeviceApi', {'viewId': 1, 'enabled': True}),
    'call-set-device-state.bin': ('call', C, 'setDeviceState', {'viewId': 1,
                                                                'json': '{"screen":"dim","powerSource":"battery","level":42}'}),
    'call-device-event.bin': ('call', C, 'deviceEvent', {'viewId': 1, 'type': 'resume', 'sleptMs': 3600000}),
    'call-listen.bin': ('call', E, 'listen', None),
    'call-cancel.bin': ('call', E, 'cancel', None),

    # ---- replies (host -> Dart) ----
    'reply-hello-unavailable.bin': ('reply', C, 'hello', {'protocol': 1, 'available': False, 'reason': 'no library',
                                                          'library': None, 'abi': None, 'features': []}),
    'reply-hello-available.bin': ('reply', C, 'hello', {'protocol': 1, 'available': True, 'reason': None,
                                                        'library': None, 'abi': 1,
                                                        'features': ['refreshRate', 'newSized']}),
    'reply-hello-full.bin': ('reply', C, 'hello', {
        'protocol': 1, 'available': True, 'reason': None, 'library': '9.9.9-fake', 'abi': 1,
        'features': ['refreshRate', 'dragThreshold', 'newSized', 'whenRendered', 'pinchZoom', 'requests',
                     'originHeaders', 'userAgent', 'cookies', 'allowCertificate', 'tlsErrorsPolicy', 'colorScheme',
                     'clearData', 'deviceApi']}),
    'reply-initialize.bin': ('reply', C, 'initialize', {'dragThreshold': 16, 'refreshRate': 48.61}),
    'reply-create.bin': ('reply', C, 'create', {'viewId': 1, 'textureId': 2}),
    'reply-state.bin': ('reply', C, 'getState', {'uri': 'https://example.com/', 'title': 'Example', 'canGoBack': True,
                                                 'canGoForward': False, 'magnification': 1.0}),
    'reply-null.bin': ('reply', C, '*', None),
    'reply-true.bin': ('reply', C, 'setCookies', True),
    'reply-false.bin': ('reply', C, 'clearData', False),
    'reply-rendered-done.bin': ('reply', C, 'whenRendered', 'done'),
    'reply-rendered-cancelled.bin': ('reply', C, 'whenRendered', 'cancelled'),
    'reply-error-no-view.bin': ('error', C, '*', ['noView', 'unknown viewId', None]),
    'reply-error-no-request.bin': ('error', C, 'replyDialog', ['noRequest', 'unknown or ended requestId', None]),
    'reply-error-bad-arguments.bin': ('error', C, '*', ['badArguments', 'viewId: a positive int is required', None]),
    'reply-error-unsupported.bin': ('error', C, 'setDeviceApi', ['unsupported',
                                                                 'the library lacks the feature deviceApi', None]),
    'reply-error-refused.bin': ('error', C, 'setDeviceState', ['refused', 'the library refused the device state',
                                                               None]),

    # ---- events (host -> Dart, uc.webview/events) ----
    'event-load-changed.bin': ('event', E, 'loadChanged', {'type': 'loadChanged', 'viewId': 1, 'event': 'finished',
                                                           'uri': 'about:blank'}),
    'event-load-failed.bin': ('event', E, 'loadFailed', {
        'type': 'loadFailed', 'viewId': 1, 'uri': 'https://nx.example/', 'reason': 'hostNotFound',
        'errorDomain': 'g-resolver-error-quark', 'errorCode': 1, 'tlsErrors': 0,
        'message': 'Error resolving \u201cnx.example\u201d: Name or service not known'}),
    'event-progress.bin': ('event', E, 'progress', {'type': 'progress', 'viewId': 1, 'progress': 0.5}),
    'event-title.bin': ('event', E, 'titleChanged', {'type': 'titleChanged', 'viewId': 1, 'title': 'Example'}),
    'event-history.bin': ('event', E, 'historyChanged', {'type': 'historyChanged', 'viewId': 1, 'canGoBack': True,
                                                         'canGoForward': False}),
    'event-frame.bin': ('event', E, 'frame', {'type': 'frame', 'viewId': 1, 'width': 480, 'height': 800,
                                              'zeroCopy': True}),
    'event-terminated.bin': ('event', E, 'terminated', {'type': 'terminated', 'viewId': 2, 'reason': 'crashed'}),
    'event-ime-focus.bin': ('event', E, 'imeFocus', {'type': 'imeFocus', 'viewId': 1, 'focused': True,
                                                     'hints': 0x81, 'purpose': 'email', 'surrounding': 'a@b',
                                                     'cursor': 3}),
    'event-ime-cursor-area.bin': ('event', E, 'imeCursorArea', {'type': 'imeCursorArea', 'viewId': 1, 'x': 10,
                                                                'y': 20, 'width': 2, 'height': 18}),
    'event-ime-surrounding.bin': ('event', E, 'imeSurrounding', {'type': 'imeSurrounding', 'viewId': 1,
                                                                 'text': 'hello', 'cursor': 5, 'anchor': 2}),
    'event-cursor.bin': ('event', E, 'cursorChanged', {'type': 'cursorChanged', 'viewId': 1, 'cursor': 'pointer'}),
    'event-pinch.bin': ('event', E, 'pinch', {'type': 'pinch', 'viewId': 1, 'phase': 'update', 'scale': 1.5,
                                              'x': 240.0, 'y': 400.0}),
    'event-magnification.bin': ('event', E, 'magnificationChanged', {'type': 'magnificationChanged', 'viewId': 1,
                                                                     'magnification': 2.0}),
    'event-script-dialog.bin': ('event', E, 'scriptDialog', {
        'type': 'scriptDialog', 'viewId': 1, 'requestId': 1, 'dialogType': 'prompt', 'origin': 'https://example.com',
        'message': 'Your name?', 'defaultText': 'Bob'}),
    'event-script-dialog-before-unload.bin': ('event', E, 'scriptDialog', {
        'type': 'scriptDialog', 'viewId': 1, 'requestId': 1, 'dialogType': 'beforeUnload',
        'origin': 'https://example.com', 'message': None, 'defaultText': None}),
    'event-authenticate.bin': ('event', E, 'authenticate', {
        'type': 'authenticate', 'viewId': 1, 'requestId': 1, 'origin': 'https://example.com', 'realm': 'Area',
        'scheme': 'basic', 'retry': False, 'proxy': False}),
    'event-request-cancelled.bin': ('event', E, 'requestCancelled', {'type': 'requestCancelled', 'viewId': 1,
                                                                     'requestId': 1, 'reason': 'page'}),
    'event-close-requested.bin': ('event', E, 'closeRequested', {'type': 'closeRequested', 'viewId': 1}),
    'event-new-window.bin': ('event', E, 'newWindow', {'type': 'newWindow', 'viewId': 1,
                                                       'uri': 'https://example.org/', 'loaded': True}),
    'event-response-refused.bin': ('event', E, 'responseRefused', {
        'type': 'responseRefused', 'viewId': 1, 'uri': 'https://example.com/file.zip', 'mimeType': 'application/zip',
        'reason': 'download'}),
}


def encode(kind, value):
    if kind == 'call':
        return call(value[0], value[1])
    if kind == 'error':
        return error(*value)
    return success(value)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    manifest = []
    for name, (kind, channel, what, value) in FIXTURES.items():
        args = (what, value) if kind == 'call' else value
        with open(os.path.join(out, name), 'wb') as f:
            f.write(encode(kind, args))
        entry = {'file': name, 'kind': kind, 'channel': channel}
        entry['method' if kind != 'event' else 'event'] = what
        if kind == 'call':
            entry['arguments'] = value
        elif kind == 'error':
            entry['code'], entry['message'], entry['details'] = value
        else:
            entry['value'] = value
        manifest.append(entry)
    with open(os.path.join(out, 'manifest.json'), 'w', encoding='utf-8') as f:
        json.dump(manifest, f, indent=1, ensure_ascii=False)
        f.write('\n')
    print('%d fixtures and manifest.json written to %s' % (len(FIXTURES), out))


if __name__ == '__main__':
    main()
