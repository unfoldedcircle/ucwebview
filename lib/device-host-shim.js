/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The device state API of a web page (libucwebview 0.9.0, package README "Device state for web pages"; the page side
 * is the Core-API's web view entity documentation, "Device state for web pages"). The library's web process extension
 * (uc_ext_device.c) evaluates this file in the page's own script world at the start of every document of the top
 * frame of a web view whose host switched the device API on (uc_webview_set_device_api()): the file is one function
 * expression, which the extension calls once with the view's state and which returns the document's update function.
 * The extension keeps that function in the library's own script world (uc_ext_protocol.h) and calls it with every
 * change; the page has no way to reach it. It behaves like the shim of the Core-API repository's device API simulator
 * (tools/device-api-simulator), which a desktop mock drives through an entry point on window instead; the differences
 * are the deferred delivery and the validation below, and the event handler attributes and getBattery(), which follow
 * the specifications more closely.
 *
 * What a page gets:
 *  - navigator.getBattery(): the same promise of a BatteryManager as in the W3C Battery Status API (charging, level,
 *    chargingTime, dischargingTime; chargingchange, levelchange, chargingtimechange, dischargingtimechange and their
 *    on... handlers). charging is true while the Remote is on power (also at full level), level has steps of 0.01.
 *    The Remote has no time estimates: chargingTime is 0 on power at full level, else Infinity; dischargingTime is
 *    always Infinity; their change events never fire.
 *  - document freeze and resume events (Page Lifecycle names) and document.onfreeze / document.onresume, around a
 *    standby of the Remote.
 *  - window.deviceHost, an EventTarget: version 1 and the read-only attributes screen ('on' | 'dim' | 'off'),
 *    powerSource ('ac' | 'battery') and powerProfile ('power-saver' | 'balanced' | 'performance'); the events
 *    suspend (an Event), resume (detail.sleptMs), on-ac, on-battery, screenchange (detail.state) and
 *    powerprofilechange (detail.profile), through addEventListener only.
 *  - <html data-screen="..." data-power-profile="..."> for CSS.
 * Events fire only when a value really changes; every attribute has its new value before the first event of an
 * update fires. Updates arrive in a task of their own (a MessageChannel): the update function only posts the change
 * to that channel, with functions it took before the page's first script, so that no page code runs inside the
 * extension's call; values that are not valid are ignored.
 *
 * The update function takes one change: { screen?, powerSource?, powerProfile?, level? (percent, an integer
 * 0..100) }, optionally with event: 'suspend' (the state first, then freeze on document and suspend on deviceHost) or
 * 'resume' with sleptMs (resume on document and on deviceHost first, then the state).
 *
 * Rules for this file: ASCII only and no tab characters (the build compiles it into the extension as bytes); one
 * function expression and nothing after it but comments.
 */
(function (init) {
    'use strict';

    const defineProperty = Object.defineProperty;
    const addListener = EventTarget.prototype.addEventListener;
    const removeListener = EventTarget.prototype.removeEventListener;
    const dispatch = EventTarget.prototype.dispatchEvent;
    const call = Function.prototype.call;
    const SCREENS = ['on', 'dim', 'off'];
    const SOURCES = ['ac', 'battery'];
    const PROFILES = ['power-saver', 'balanced', 'performance'];
    const token = {};

    // ---- the state and its validation (the library sends valid values; the shim does not rely on it) ----

    // level: the battery level in percent
    const state = { screen: 'on', powerSource: 'ac', powerProfile: 'balanced', level: 100 };

    // the valid values of a change; anything else is left out
    function validated(change) {
        const out = {};
        if (SCREENS.indexOf(change.screen) >= 0) {
            out.screen = change.screen;
        }
        if (SOURCES.indexOf(change.powerSource) >= 0) {
            out.powerSource = change.powerSource;
        }
        if (PROFILES.indexOf(change.powerProfile) >= 0) {
            out.powerProfile = change.powerProfile;
        }
        if (Number.isInteger(change.level) && change.level >= 0 && change.level <= 100) {
            out.level = change.level;
        }
        return out;
    }

    function charging() {
        return state.powerSource === 'ac';
    }

    // ---- events ----

    function fire(target, type, detail) {
        dispatch.call(target, detail === undefined ? new Event(type) : new CustomEvent(type, { detail: detail }));
    }

    // on<type> event handler attributes, as accessors on a prototype: a handler is a listener registered when it is
    // set, keeps its place when it is replaced and goes when it is set to null (a non-function counts as null)
    const handlers = new WeakMap();
    function defineHandlers(proto, types) {
        types.forEach(function (type) {
            defineProperty(proto, 'on' + type, {
                configurable: true,
                enumerable: true,
                get: function () {
                    const slots = handlers.get(this);
                    return slots && slots[type] ? slots[type].handler : null;
                },
                set: function (value) {
                    const handler = typeof value === 'function' ? value : null;
                    let slots = handlers.get(this);
                    if (!slots) {
                        slots = {};
                        handlers.set(this, slots);
                    }
                    const slot = slots[type];
                    if (slot && handler) {
                        slot.handler = handler;
                    } else if (slot) {
                        removeListener.call(this, type, slot.listener);
                        delete slots[type];
                    } else if (handler) {
                        const created = {
                            handler: handler,
                            listener: function (event) {
                                return created.handler.call(this, event);
                            },
                        };
                        slots[type] = created;
                        addListener.call(this, type, created.listener);
                    }
                },
            });
        });
    }

    // ---- navigator.getBattery() ----

    class BatteryManager extends EventTarget {
        constructor(key) {
            if (key !== token) {
                throw new TypeError('Illegal constructor');
            }
            super();
        }
        get charging() {
            return charging();
        }
        get chargingTime() {
            return charging() && state.level >= 100 ? 0 : Infinity;
        }
        get dischargingTime() {
            return Infinity;
        }
        get level() {
            return state.level / 100;
        }
    }
    defineHandlers(BatteryManager.prototype, ['chargingchange', 'chargingtimechange', 'dischargingtimechange',
                                              'levelchange']);
    defineProperty(BatteryManager.prototype, Symbol.toStringTag, { configurable: true, value: 'BatteryManager' });

    // on Navigator.prototype like a browser's, with the same promise for every call (the spec's battery promise)
    const battery = new BatteryManager(token);
    const batteryPromise = Promise.resolve(battery);
    defineProperty(Navigator.prototype, 'getBattery', {
        configurable: true,
        enumerable: true,
        writable: true,
        value: function getBattery() {
            return batteryPromise;
        },
    });

    // ---- document freeze and resume ----

    defineHandlers(Document.prototype, ['freeze', 'resume']);

    // ---- window.deviceHost ----

    class DeviceHost extends EventTarget {
        constructor(key) {
            if (key !== token) {
                throw new TypeError('Illegal constructor');
            }
            super();
        }
        get version() {
            return 1;
        }
        get screen() {
            return state.screen;
        }
        get powerSource() {
            return state.powerSource;
        }
        get powerProfile() {
            return state.powerProfile;
        }
    }
    defineProperty(DeviceHost.prototype, Symbol.toStringTag, { configurable: true, value: 'DeviceHost' });

    const host = new DeviceHost(token);
    defineProperty(window, 'deviceHost', { configurable: true, enumerable: true, value: host });

    // ---- <html data-screen data-power-profile> ----

    const doc = document;
    function setAttributes() {
        const root = doc.documentElement;
        if (root) {
            root.setAttribute('data-screen', state.screen);
            root.setAttribute('data-power-profile', state.powerProfile);
        }
        return !!root;
    }

    // ---- updates ----

    // applies the valid values of a change: every value first, then the events of what changed
    function applyState(change) {
        const next = validated(change);
        const before = {
            screen: state.screen,
            powerSource: state.powerSource,
            powerProfile: state.powerProfile,
            level: state.level,
        };
        Object.keys(next).forEach(function (key) {
            state[key] = next[key];
        });
        setAttributes();
        if (state.powerSource !== before.powerSource) {
            fire(battery, 'chargingchange');
            fire(host, state.powerSource === 'ac' ? 'on-ac' : 'on-battery');
        }
        if (state.level !== before.level) {
            fire(battery, 'levelchange');
        }
        if (state.screen !== before.screen) {
            fire(host, 'screenchange', { state: state.screen });
        }
        if (state.powerProfile !== before.powerProfile) {
            fire(host, 'powerprofilechange', { profile: state.powerProfile });
        }
    }

    function apply(change) {
        if (change.event === 'resume') {
            fire(doc, 'resume');
            fire(host, 'resume', { sleptMs: Math.max(0, Number(change.sleptMs) || 0) });
        }
        applyState(change);
        if (change.event === 'suspend') {
            fire(doc, 'freeze');
            fire(host, 'suspend');
        }
    }

    // One channel for every update: its messages run in order, each in a task of its own, outside the extension's call.
    // The change travels as the message (a structured clone of the extension's plain object), and the update function
    // posts it with the port and postMessage of this moment, through a bound call: nothing a page script replaces
    // later (Array.prototype.push, MessagePort.prototype.postMessage, Function.prototype.call) runs inside it.
    let send = null;
    if (typeof MessageChannel === 'function') {
        const channel = new MessageChannel();
        channel.port1.onmessage = function (event) {
            const change = event.data;
            if (change !== null && typeof change === 'object') {
                apply(change);
            }
        };
        send = call.bind(channel.port2.postMessage, channel.port2);
    }

    function update(change) {
        if (change === null || typeof change !== 'object') {
            return;
        }
        if (send === null) {
            apply(change);
            return;
        }
        send(change);
    }

    // the state this document starts with: no events
    const initial = validated(init !== null && typeof init === 'object' ? init : {});
    Object.keys(initial).forEach(function (key) {
        state[key] = initial[key];
    });
    if (!setAttributes()) {
        doc.addEventListener('DOMContentLoaded', setAttributes, { once: true });
    }
    return update;
})
//# sourceURL=ucwebview-device-host-shim.js
