/*
 * Super Boom — Chain UI
 *
 * 4-page parameter browser with "hover-to-preview" knob mapping:
 * scrolling the jog to highlight a page immediately maps knobs 1-8
 * to that page's parameters, without needing to click into it.
 *
 * Jog click enters the page for detailed param editing via jog.
 */

import {
    MoveMainKnob,
    MoveBack,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    LightGrey
} from '/data/UserData/move-anything/shared/constants.mjs';

import { decodeDelta, decodeAcceleratedDelta } from '/data/UserData/move-anything/shared/input_filter.mjs';

import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter
} from '/data/UserData/move-anything/shared/menu_layout.mjs';

const W = 128;
const H = 64;
const CC_JOG_CLICK = 3;
const KNOB_CC_BASE = 71;

/* ═══════════════════════════════════ Page Definitions ══ */

const PAGES = [
    {
        name: "Boom",
        params: [
            { key: "inputGain", name: "Input",  min: 0,   max: 4,   step: 0.02, fmt: v => v.toFixed(2) },
            { key: "compAmount",name: "Comp",   min: 0,   max: 1,   step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "drive",     name: "Drive",  min: 1,   max: 20,  step: 0.1,  fmt: v => v.toFixed(1) },
            { key: "distMode",  name: "Mode",   min: 0,   max: 3,   step: 1,    enum: ["Boost","Tube","Fuzz","Square"] },
            { key: "flavor",    name: "Flavor", min: 0,   max: 7,   step: 1,    enum: ["Bal","Form","Oct","Thump","Radio","Acid","Motwn","West"] },
            { key: "shift",     name: "Shift",  min: -2,  max: 2,   step: 0.01, fmt: v => `${v >= 0 ? "+" : ""}${v.toFixed(2)}` },
            { key: "mix",       name: "Mix",    min: 0,   max: 1,   step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "output",    name: "Level",  min: 0,   max: 2,   step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` }
        ]
    },
    {
        name: "Skulpt",
        params: [
            { key: "b1", name: "Low",  min: 0, max: 2, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "b2", name: "B2",   min: 0, max: 2, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "b3", name: "B3",   min: 0, max: 2, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "b4", name: "Mid",  min: 0, max: 2, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "b5", name: "B5",   min: 0, max: 2, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "b6", name: "B6",   min: 0, max: 2, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "b7", name: "B7",   min: 0, max: 2, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "b8", name: "High", min: 0, max: 2, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` }
        ]
    },
    {
        name: "Move",
        params: [
            { key: "atk",      name: "Atk",    min: 0, max: 5, step: 1, enum: ["50us","330us","1ms","3.3ms","10ms","33ms"] },
            { key: "rel",      name: "Rel",    min: 0, max: 5, step: 1, enum: ["50ms","100ms","250ms","350ms","500ms","1s"] },
            { key: "modShift", name: "M-Shft", min: -1, max: 1, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "modDrive", name: "M-Drv",  min: 0,  max: 1, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "preType",  name: "Pre",    min: 0,  max: 9, step: 1, enum: ["Cass1","Cass2","Mast","Slam","Thick","12bit","8bit","Brit","USA","Cln"] },
            { key: "grit",     name: "Grit",   min: 0,  max: 1, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "gate",     name: "Gate",   min: 0,  max: 6, step: 1, enum: ["Off","-50dB","-40dB","-30dB","-20dB","-10dB","0dB"] },
            { key: "link",     name: "Link",   min: 0,  max: 1, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` }
        ]
    },
    {
        name: "Seal",
        params: [
            { key: "loCut",   name: "LoCut",   min: 0, max: 3, step: 1, enum: ["Off","75","150","300"] },
            { key: "hiCut",   name: "HiCut",   min: 0, max: 100, step: 1, fmt: v => {
                const hz = 20 * Math.pow(1000, v / 100);
                return hz >= 1000 ? `${(hz/1000).toFixed(1)}k` : `${Math.round(hz)}Hz`;
            }},
            { key: "sat",     name: "Sat",     min: 0, max: 1, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "age",     name: "Age",     min: 0, max: 1, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "flutter", name: "Flutter", min: 0, max: 1, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "bump",    name: "Bump",    min: 0, max: 1, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "thresh",  name: "Thresh",  min: 0, max: 1, step: 0.01, fmt: v => `${(v*100).toFixed(0)}%` },
            { key: "bypass",  name: "Bypass",  min: 0, max: 1, step: 1, enum: ["On","Byp"] }
        ]
    }
];

/* ═══════════════════════════════════ State ══ */

let selectedPage = 0;       /* highlighted page in root view (0-3) */
let insidePage = false;      /* true = browsing params inside a page */
let selectedParam = 0;       /* highlighted param index within page */
let editMode = false;        /* true = jog edits value instead of navigating */
let needsRedraw = true;

/* Flat value store indexed by param key */
let values = {};

/* ═══════════════════════════════════ Helpers ══ */

function formatValue(p, v) {
    if (p.enum) return p.enum[Math.round(v)] || "?";
    return p.fmt(v);
}

function clampValue(p, v) {
    if (p.enum) {
        v = Math.round(v);
        if (v > p.max) v = p.max;
        if (v < p.min) v = p.min;
    } else {
        v = Math.max(p.min, Math.min(p.max, v));
    }
    return v;
}

function setParam(p, value) {
    value = clampValue(p, value);
    values[p.key] = value;
    let valStr;
    if (p.enum) {
        valStr = String(Math.round(value));
    } else if (p.step >= 1) {
        valStr = String(Math.round(value));
    } else {
        valStr = value.toFixed(4);
    }
    host_module_set_param(p.key, valStr);
    needsRedraw = true;
}

function fetchAllParams() {
    for (const page of PAGES) {
        for (const p of page.params) {
            const raw = host_module_get_param(p.key);
            if (raw !== null && raw !== undefined && raw !== "") {
                if (p.enum) {
                    /* DSP returns enum name string — find index */
                    const idx = p.enum.indexOf(raw);
                    if (idx >= 0) {
                        values[p.key] = idx;
                    } else {
                        values[p.key] = parseFloat(raw) || 0;
                    }
                } else {
                    const num = parseFloat(raw);
                    if (!isNaN(num)) values[p.key] = num;
                }
            }
        }
    }
}

/* ═══════════════════════════════════ Drawing ══ */

function drawRootView() {
    clear_screen();
    drawHeader("Super Boom");

    /* Show active page's knob summary below header */
    const activePage = PAGES[selectedPage];
    print(2, 12, activePage.name, 1);

    /* Page list */
    const lh = 11;
    const y0 = 24;

    for (let i = 0; i < PAGES.length; i++) {
        const y = y0 + i * lh;
        const sel = i === selectedPage;

        if (sel) fill_rect(0, y - 1, W, lh, 1);

        const color = sel ? 0 : 1;
        const prefix = sel ? "> " : "  ";
        print(2, y, `${prefix}${PAGES[i].name}`, color);

        /* Show first param value as preview */
        const firstP = PAGES[i].params[0];
        const v = values[firstP.key];
        if (v !== undefined) {
            const vs = formatValue(firstP, v);
            print(W - vs.length * 6 - 4, y, vs, color);
        }
    }

    drawFooter({ left: "Jog:page", right: "Click:enter" });
}

function drawPageView() {
    clear_screen();

    const page = PAGES[selectedPage];
    drawHeader(`Super Boom: ${page.name}`);

    /* Parameter list — show 4 visible params around selection */
    const lh = 11;
    const y0 = 16;
    const visible = 4;
    let startIdx = Math.max(0, Math.min(selectedParam - 1, page.params.length - visible));

    for (let vi = 0; vi < visible; vi++) {
        const i = startIdx + vi;
        if (i >= page.params.length) break;

        const y = y0 + vi * lh;
        const p = page.params[i];
        const sel = i === selectedParam;

        if (sel) fill_rect(0, y - 1, W, lh, 1);

        const color = sel ? 0 : 1;
        const prefix = sel ? (editMode ? "* " : "> ") : "  ";
        print(2, y, `${prefix}${p.name}`, color);

        const v = values[p.key];
        if (v !== undefined) {
            const vs = formatValue(p, v);
            print(W - vs.length * 6 - 4, y, vs, color);
        }
    }

    if (editMode) {
        drawFooter({ left: "Jog:value", right: "Click:done" });
    } else {
        drawFooter({ left: "Jog:param", right: "Back:pages" });
    }
}

function draw() {
    if (insidePage) {
        drawPageView();
    } else {
        drawRootView();
    }
    needsRedraw = false;
}

/* ═══════════════════════════════════ Knob Handling ══ */

function handleKnob(knobIdx, delta) {
    /* Knobs always map to the highlighted page's params */
    const page = PAGES[selectedPage];
    if (knobIdx >= page.params.length) return;

    const p = page.params[knobIdx];
    const v = values[p.key] !== undefined ? values[p.key] : p.min;
    setParam(p, v + delta * p.step);
}

/* ═══════════════════════════════════ Input ══ */

function init() {
    fetchAllParams();
    needsRedraw = true;
}

function tick() {
    fetchAllParams();
    if (needsRedraw) draw();
}

function onMidiMessageInternal(data) {
    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    if (status !== 0xB0) return;

    /* ── Jog wheel rotate ── */
    if (d1 === MoveMainKnob) {
        const delta = decodeDelta(d2);
        if (delta === 0) return;

        if (!insidePage) {
            /* Root view — scroll pages, knobs follow immediately */
            selectedPage = Math.max(0, Math.min(PAGES.length - 1, selectedPage + delta));
            needsRedraw = true;
        } else if (editMode) {
            /* Edit mode — adjust selected param value */
            const page = PAGES[selectedPage];
            const p = page.params[selectedParam];
            const v = values[p.key] !== undefined ? values[p.key] : p.min;
            setParam(p, v + delta * p.step);
        } else {
            /* Page view — navigate params */
            const page = PAGES[selectedPage];
            selectedParam = Math.max(0, Math.min(page.params.length - 1, selectedParam + delta));
            needsRedraw = true;
        }
        return;
    }

    /* ── Jog click ── */
    if (d1 === CC_JOG_CLICK && d2 >= 64) {
        if (!insidePage) {
            /* Enter page */
            insidePage = true;
            selectedParam = 0;
            editMode = false;
        } else if (editMode) {
            /* Exit edit mode */
            editMode = false;
        } else {
            /* Enter edit mode for selected param */
            editMode = true;
        }
        needsRedraw = true;
        return;
    }

    /* ── Back button — exit page or edit ── */
    if (d1 === MoveBack && d2 >= 64) {
        if (editMode) {
            editMode = false;
            needsRedraw = true;
        } else if (insidePage) {
            insidePage = false;
            needsRedraw = true;
        }
        /* If at root level, let chain handle the back (exit component UI) */
        return;
    }

    /* ── Knobs 1-8 → always map to highlighted page ── */
    const knobIdx = d1 - KNOB_CC_BASE;
    if (knobIdx >= 0 && knobIdx < 8) {
        const delta = decodeDelta(d2);
        if (delta !== 0) handleKnob(knobIdx, delta);
        return;
    }
}

/* Export for chain source UI (ui_chain.js path) */
globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal
};

/* Export for chain component UI (ui.js path) */
globalThis.init = init;
globalThis.tick = tick;
globalThis.onMidiMessageInternal = onMidiMessageInternal;
