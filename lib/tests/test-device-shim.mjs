// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Unfolded Circle ApS
//
// device-host-shim.js (library 0.9.0) in Node's vm with a minimal DOM: the file as the extension uses it (one function
// expression, called with the state, returning the update function and leaving nothing else on window), the page API,
// the start state, changes and their events in the order of the Core-API's device API simulator shim, delivery in a
// task of its own with functions taken before the page's scripts, the event handler attributes, validation and the file
// rules. Run: node tests/test-device-shim.mjs (make check-js); needs Node 19 or newer (EventTarget, Event, CustomEvent,
// structuredClone, node:test).
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const here = dirname(fileURLToPath(import.meta.url));
const SHIM = readFileSync(join(here, '..', 'device-host-shim.js'), 'utf8');

class FakeElement {
    constructor() {
        this.attributes = new Map();
    }
    setAttribute(name, value) {
        this.attributes.set(name, String(value));
    }
    getAttribute(name) {
        return this.attributes.has(name) ? this.attributes.get(name) : null;
    }
}

// a page: window (the vm's global), document, navigator and a MessageChannel whose messages (structured clones, like a
// browser's) run when the test says
function page({ withRoot = true, withChannel = true } = {}) {
    const pending = [];
    class Document extends EventTarget {
        constructor() {
            super();
            this.documentElement = withRoot ? new FakeElement() : null;
        }
    }
    class Navigator {}
    class MessagePort {
        postMessage(data) {
            pending.push([this.peer, structuredClone(data)]);
        }
    }
    class MessageChannel {
        constructor() {
            this.port1 = new MessagePort();
            this.port2 = new MessagePort();
            this.port1.onmessage = null;
            this.port2.peer = this.port1;
            this.port1.peer = this.port2;
        }
    }
    const sandbox = {
        EventTarget, Event, CustomEvent, Document, Navigator, MessagePort,
        document: new Document(),
        navigator: new Navigator(),
    };
    if (withChannel) {
        sandbox.MessageChannel = MessageChannel;
    }
    vm.createContext(sandbox);
    vm.runInContext('var window = globalThis;', sandbox);
    return {
        window: sandbox,
        // what the extension does (uc_ext_device.c): evaluate the file, call it with the state, keep the result
        start(init = {}) {
            const factory = vm.runInContext(SHIM, sandbox);
            this.update = factory(vm.runInContext('(' + JSON.stringify(init) + ')', sandbox));
            return this.update;
        },
        eval(code) {
            return vm.runInContext(code, sandbox);
        },
        pending,
        // the tasks of the channel, in order
        flush() {
            while (pending.length > 0) {
                const [port, data] = pending.shift();
                port.onmessage({ data });
            }
        },
        // every event of deviceHost, the battery and the document, as [target, type, detail] (detail copied into this
        // realm, so that deepStrictEqual compares plain objects; log.classes: Event or CustomEvent)
        record() {
            const log = [];
            Object.defineProperty(log, 'classes', { value: [] });
            const add = (target, type, detail, cls) => {
                log.classes.push(cls);
                log.push([target, type, detail === undefined ? undefined : structuredClone(detail)]);
            };
            vm.runInContext(`(function (add) {
                const host = window.deviceHost;
                ['suspend', 'resume', 'on-ac', 'on-battery', 'screenchange', 'powerprofilechange'].forEach(t =>
                    host.addEventListener(t, e => add('host', t, e.detail, e.constructor.name)));
                navigator.getBattery().then(b => {
                    ['chargingchange', 'levelchange', 'chargingtimechange', 'dischargingtimechange'].forEach(t =>
                        b.addEventListener(t, () => add('battery', t, undefined)));
                });
                ['freeze', 'resume'].forEach(t => document.addEventListener(t, () => add('document', t, undefined)));
            })`, sandbox)(add);
            return log;
        },
    };
}

// lets the promise of getBattery() settle
const tick = () => new Promise(resolve => setImmediate(resolve));

const u = (p, change) => p.update(change);

test('the file: ASCII only, no tabs, one function expression', () => {
    assert.ok(/^[\x0a\x20-\x7e]*$/.test(SHIM), 'only printable ASCII and newlines');
    const p = page();
    const factory = vm.runInContext(SHIM, p.window);
    assert.equal(typeof factory, 'function');
    assert.equal(factory.length, 1);
    assert.equal('deviceHost' in p.window, false, 'nothing happens before the call');
});

test('the call returns the update function and leaves only deviceHost on window', () => {
    const p = page();
    const before = new Set(Object.getOwnPropertyNames(p.window));
    const update = p.start();
    assert.equal(typeof update, 'function');
    assert.equal(update.length, 1);
    const added = Object.getOwnPropertyNames(p.window).filter(n => !before.has(n));
    assert.deepEqual(added, ['deviceHost']);
    assert.equal('__ucDeviceHostUpdate' in p.window, false, 'no entry point for pages');
    assert.ok(Object.getOwnPropertyNames(p.window.Navigator.prototype).includes('getBattery'));
    assert.ok(Object.getOwnPropertyNames(p.window.Document.prototype).includes('onfreeze'));
    const h = Object.getOwnPropertyDescriptor(p.window, 'deviceHost');
    assert.equal(h.enumerable, true);
    assert.equal(h.configurable, true);
    assert.equal(h.writable, false);
});

test('defaults without a state', async () => {
    const p = page();
    p.start();
    const w = p.window;
    assert.ok('deviceHost' in w);
    assert.equal(w.deviceHost.version, 1);
    assert.equal(w.deviceHost.screen, 'on');
    assert.equal(w.deviceHost.powerSource, 'ac');
    assert.equal(w.deviceHost.powerProfile, 'balanced');
    assert.equal(String(w.deviceHost), '[object DeviceHost]');
    const b = await w.navigator.getBattery();
    assert.equal(b, await w.navigator.getBattery(), 'the same BatteryManager');
    assert.equal(w.navigator.getBattery(), w.navigator.getBattery(), 'the same promise');
    assert.equal(b.charging, true);
    assert.equal(b.level, 1);
    assert.equal(b.chargingTime, 0);
    assert.equal(b.dischargingTime, Infinity);
    assert.ok(b instanceof EventTarget);
    assert.equal(String(b), '[object BatteryManager]');
    assert.equal('BatteryManager' in w, false, 'no constructor on window');
    assert.equal(w.document.documentElement.getAttribute('data-screen'), 'on');
    assert.equal(w.document.documentElement.getAttribute('data-power-profile'), 'balanced');
    assert.equal(p.pending.length, 0, 'no update queued');
});

test('the start state, without events', async () => {
    const p = page();
    p.start({ screen: 'dim', powerSource: 'battery', powerProfile: 'power-saver', level: 42 });
    const w = p.window;
    assert.equal(w.deviceHost.screen, 'dim');
    assert.equal(w.deviceHost.powerSource, 'battery');
    assert.equal(w.deviceHost.powerProfile, 'power-saver');
    const b = await w.navigator.getBattery();
    assert.equal(b.charging, false);
    assert.equal(b.level, 0.42);
    assert.equal(b.chargingTime, Infinity);
    assert.equal(b.dischargingTime, Infinity);
    assert.equal(w.document.documentElement.getAttribute('data-screen'), 'dim');
    assert.equal(w.document.documentElement.getAttribute('data-power-profile'), 'power-saver');
    assert.equal(p.pending.length, 0);
});

test('an invalid start value keeps its default', async () => {
    const p = page();
    p.start({ screen: 'bright', powerSource: 'solar', powerProfile: 'turbo', level: 0.5 });
    assert.equal(p.window.deviceHost.screen, 'on');
    assert.equal(p.window.deviceHost.powerSource, 'ac');
    assert.equal(p.window.deviceHost.powerProfile, 'balanced');
    assert.equal((await p.window.navigator.getBattery()).level, 1);
    const q = page();
    vm.runInContext(SHIM, q.window)(null);
    assert.equal(q.window.deviceHost.screen, 'on', 'null as the state');
});

test('the update function runs no page code: what a page replaces later is not called', () => {
    const p = page();
    p.start();
    // a page that replaces what the update function could call, after the shim started
    p.eval(`(function () {
        const called = window.called = [];
        Array.prototype.push = function () { called[called.length] = 'push'; return 0; };
        MessagePort.prototype.postMessage = function () { called[called.length] = 'postMessage'; };
        Function.prototype.call = function () { called[called.length] = 'call'; };
        Function.prototype.apply = function () { called[called.length] = 'apply'; };
        Function.prototype.bind = function () { called[called.length] = 'bind'; };
        Object.defineProperty(Object.prototype, 'screen', {
            configurable: true,
            set() { called[called.length] = 'setter'; },
        });
    })()`);
    // the extension's change: a plain object as JSON.parse() makes it
    u(p, vm.runInContext('JSON.parse(\'{"screen":"dim","level":40}\')', p.window));
    assert.deepEqual(Array.from(p.window.called), [], 'no page function ran inside the call');
    assert.equal(p.pending.length, 1, 'the change was posted');
    assert.deepEqual(p.pending[0][1], { screen: 'dim', level: 40 }, 'as a copy of its values');
});

test('read-only attributes and no constructors for pages', async () => {
    const p = page();
    p.start();
    assert.throws(() => p.eval('"use strict"; deviceHost.screen = "off";'), { name: 'TypeError' });
    assert.throws(() => p.eval('"use strict"; deviceHost.version = 2;'), { name: 'TypeError' });
    assert.equal(p.window.deviceHost.screen, 'on');
    assert.throws(() => p.eval('new (Object.getPrototypeOf(deviceHost).constructor)()'), { name: 'TypeError' });
    const b = await p.window.navigator.getBattery();
    assert.throws(() => new b.constructor(), { name: 'TypeError' });
    assert.equal(p.eval('"onscreenchange" in deviceHost'), false, 'deviceHost has no on... handlers');
});

test('a change: deferred to a task, every value first, then the events of the mock in its order', async () => {
    const p = page();
    p.start();
    const log = p.record();
    await tick();
    const seen = [];
    p.eval('(f => navigator.getBattery().then(b => b.addEventListener("chargingchange", f)))')(() => {
        seen.push([p.window.deviceHost.screen, p.window.deviceHost.powerProfile, p.window.deviceHost.powerSource,
                   p.window.document.documentElement.getAttribute('data-power-profile')]);
    });
    await tick();
    u(p, { screen: 'off', powerSource: 'battery', powerProfile: 'power-saver', level: 50 });
    assert.equal(log.length, 0, 'nothing inside the delivering script');
    assert.equal(p.window.deviceHost.screen, 'on');
    p.flush();
    assert.deepEqual(seen, [['off', 'power-saver', 'battery', 'power-saver']]);
    assert.deepEqual(log, [
        ['battery', 'chargingchange', undefined],
        ['host', 'on-battery', undefined],
        ['battery', 'levelchange', undefined],
        ['host', 'screenchange', { state: 'off' }],
        ['host', 'powerprofilechange', { profile: 'power-saver' }],
    ]);
    assert.deepEqual(log.classes, [undefined, 'Event', undefined, 'CustomEvent', 'CustomEvent']);
    assert.equal(p.window.document.documentElement.getAttribute('data-screen'), 'off');
    const b = await p.window.navigator.getBattery();
    assert.equal(b.charging, false);
    assert.equal(b.level, 0.5);
    assert.equal(b.chargingTime, Infinity);
});

test('events only for real changes; partial changes', async () => {
    const p = page();
    p.start();
    const log = p.record();
    await tick();
    u(p, { screen: 'on', powerSource: 'ac', powerProfile: 'balanced', level: 100 });
    p.flush();
    assert.deepEqual(log, []);
    u(p, { screen: 'dim' });
    u(p, { screen: 'dim' });
    u(p, { powerSource: 'battery' });
    u(p, { powerSource: 'ac' });
    p.flush();
    assert.deepEqual(log, [['host', 'screenchange', { state: 'dim' }],
                           ['battery', 'chargingchange', undefined], ['host', 'on-battery', undefined],
                           ['battery', 'chargingchange', undefined], ['host', 'on-ac', undefined]]);
    assert.equal(p.window.deviceHost.powerProfile, 'balanced', 'untouched');
});

test('invalid values and changes are ignored', async () => {
    const p = page();
    p.start();
    const log = p.record();
    await tick();
    const update = p.update;
    update({ screen: 'bright', powerSource: 'solar', powerProfile: 'turbo', level: 101 });
    update({ level: -1 });
    update({ level: 42.5 });
    update({ level: NaN });
    update({ level: '50' });
    update({ charging: false, thermal: 'critical' });
    update({ event: 'standby' });
    update(null);
    update('screen=off');
    update();
    p.flush();
    assert.deepEqual(log, []);
    assert.equal(p.window.deviceHost.screen, 'on');
    const b = await p.window.navigator.getBattery();
    assert.equal(b.level, 1);
    assert.equal(b.charging, true);
});

test('charging follows the power source; chargingTime 0 on power at full level; no time change events', async () => {
    const p = page();
    p.start({ screen: 'on', powerSource: 'ac', powerProfile: 'balanced', level: 99 });
    const log = p.record();
    await tick();
    const b = await p.window.navigator.getBattery();
    assert.equal(b.charging, true);
    assert.equal(b.chargingTime, Infinity);
    u(p, { level: 100 });
    p.flush();
    assert.equal(b.chargingTime, 0);
    assert.deepEqual(log, [['battery', 'levelchange', undefined]]);
    log.length = 0;
    u(p, { powerSource: 'battery' });
    p.flush();
    assert.equal(b.charging, false);
    assert.equal(b.chargingTime, Infinity);
    assert.equal(b.dischargingTime, Infinity);
    assert.deepEqual(log, [['battery', 'chargingchange', undefined], ['host', 'on-battery', undefined]]);
});

test('suspend: the state, then freeze and suspend; resume: resume on both, then the state', async () => {
    const p = page();
    p.start();
    const log = p.record();
    await tick();
    u(p, { event: 'suspend', screen: 'off', powerProfile: 'power-saver' });
    u(p, { event: 'resume', sleptMs: 61234, screen: 'on', powerProfile: 'balanced' });
    assert.deepEqual(log, []);
    p.flush();
    assert.deepEqual(log, [
        ['host', 'screenchange', { state: 'off' }],
        ['host', 'powerprofilechange', { profile: 'power-saver' }],
        ['document', 'freeze', undefined],
        ['host', 'suspend', undefined],
        ['document', 'resume', undefined],
        ['host', 'resume', { sleptMs: 61234 }],
        ['host', 'screenchange', { state: 'on' }],
        ['host', 'powerprofilechange', { profile: 'balanced' }],
    ]);
    assert.equal(log.classes[3], 'Event', 'suspend is a plain Event');
    assert.equal(log.classes[5], 'CustomEvent');
    log.length = 0;
    u(p, { event: 'resume', sleptMs: -5 });
    u(p, { event: 'resume' });
    u(p, { event: 'resume', sleptMs: 12 });
    p.flush();
    assert.deepEqual(log.filter(e => e[0] === 'host').map(e => e[2]), [{ sleptMs: 0 }, { sleptMs: 0 }, { sleptMs: 12 }]);
});

test('event handler attributes: battery, document.onfreeze/onresume', async () => {
    const p = page();
    p.start();
    const b = await p.window.navigator.getBattery();
    const calls = [];
    assert.equal(b.onlevelchange, null);
    assert.ok('onlevelchange' in b && 'onchargingchange' in b && 'onchargingtimechange' in b
              && 'ondischargingtimechange' in b);
    // a handler keeps its place among the listeners when it is replaced
    b.addEventListener('levelchange', () => calls.push('a'));
    b.onlevelchange = function (e) {
        calls.push(['h1', this === b, e.type]);
    };
    b.addEventListener('levelchange', () => calls.push('b'));
    const h2 = () => calls.push('h2');
    b.onlevelchange = h2;
    assert.equal(b.onlevelchange, h2);
    u(p, { level: 50 });
    p.flush();
    assert.deepEqual(calls, ['a', 'h2', 'b']);
    // null and non-functions remove it
    calls.length = 0;
    b.onlevelchange = 'calls.push(1)';
    assert.equal(b.onlevelchange, null);
    u(p, { level: 40 });
    p.flush();
    assert.deepEqual(calls, ['a', 'b']);
    // this and the event
    calls.length = 0;
    b.onchargingchange = function (e) {
        calls.push([this === b, e.type]);
    };
    u(p, { powerSource: 'battery' });
    p.flush();
    assert.deepEqual(calls, [[true, 'chargingchange']]);

    const d = p.window.document;
    const lifecycle = [];
    assert.ok('onfreeze' in d && 'onresume' in d);
    d.onfreeze = function (e) {
        lifecycle.push(['freeze', this === d, e.type]);
    };
    d.onresume = () => lifecycle.push(['resume']);
    u(p, { event: 'suspend' });
    u(p, { event: 'resume', sleptMs: 5 });
    p.flush();
    assert.deepEqual(lifecycle, [['freeze', true, 'freeze'], ['resume']]);
    d.onfreeze = null;
    assert.equal(d.onfreeze, null);
});

test('no document element yet: the attributes come at DOMContentLoaded', () => {
    const p = page({ withRoot: false });
    p.start({ screen: 'off', powerSource: 'ac', powerProfile: 'power-saver', level: 100 });
    const d = p.window.document;
    d.documentElement = new FakeElement();
    assert.equal(d.documentElement.getAttribute('data-screen'), null);
    d.dispatchEvent(new Event('DOMContentLoaded'));
    assert.equal(d.documentElement.getAttribute('data-screen'), 'off');
    assert.equal(d.documentElement.getAttribute('data-power-profile'), 'power-saver');
});

test('changes keep their order across tasks', () => {
    const p = page();
    p.start();
    const log = p.record();
    for (const screen of ['dim', 'off', 'on', 'dim']) {
        u(p, { screen });
    }
    assert.equal(p.pending.length, 4, 'one task per change');
    p.flush();
    assert.deepEqual(log.map(e => e[2].state), ['dim', 'off', 'on', 'dim']);
});

test('without MessageChannel a change applies at once', () => {
    const p = page({ withChannel: false });
    p.start();
    const log = p.record();
    u(p, { screen: 'dim' });
    assert.deepEqual(log, [['host', 'screenchange', { state: 'dim' }]]);
});
