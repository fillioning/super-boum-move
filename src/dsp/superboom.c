/*
 * superboom.c — Super Boom for Ableton Move
 *
 * Master Bus Destructor & Harmonic Sculptor.
 * Sourced from OTO hardware specs and Airwindows-style DSP primitives.
 *
 * Signal flow:
 *   Input Gain → Preamp (10 Models) → Compressor (1:-1 Negative Ratio)
 *   → 2x OS Parallel Filterbank (8 Flavors) → Distortion (4 Modes)
 *   → Wet Filters (Lo-Cut / Hi-Cut) → Mix → Gate → Tape Stage → Flutter
 *   → Output Gain
 *
 * Build:
 *   aarch64-linux-gnu-gcc -std=gnu11 -O3 -shared -fPIC \
 *       -I/home/flou/lore-move/host superboom.c -o superboom.so -lm
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "audio_fx_api_v2.h"

static const host_api_v1_t *g_host = NULL;

#define SR      44100.0
#define TWOPI   (2.0 * M_PI)
#define FLUTTER_BUF_SIZE 1024  /* must be power of 2 */

/* ═══════════════════════════════════════════════════════ helpers ══ */

static inline float sb_clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline double sb_clampd(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline double sb_tanh(double x) {
    if (x >  3.0) return  1.0;
    if (x < -3.0) return -1.0;
    double x2 = x * x;
    return x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

/* ═══════════════════════════════════════ Biquad (Stereo TDF-II) ══ */

typedef struct {
    double b0, b1, b2, a1, a2;
    double z1L, z2L, z1R, z2R;
} Biquad;

static void bq_reset(Biquad *f) {
    f->z1L = f->z2L = f->z1R = f->z2R = 0.0;
    f->b0 = f->b1 = f->b2 = f->a1 = f->a2 = 0.0;
}

static void bq_set_bp(Biquad *f, double freq, double Q, double fs) {
    double w0 = TWOPI * freq / fs;
    double alpha = sin(w0) / (2.0 * Q);
    double a0 = 1.0 + alpha;
    f->b0 =  alpha / a0;
    f->b1 =  0.0;
    f->b2 = -alpha / a0;
    f->a1 = (-2.0 * cos(w0)) / a0;
    f->a2 = (1.0 - alpha) / a0;
}

static void bq_set_hp(Biquad *f, double freq, double Q, double fs) {
    double w0 = TWOPI * freq / fs;
    double cosW = cos(w0);
    double alpha = sin(w0) / (2.0 * Q);
    double a0 = 1.0 + alpha;
    f->b0 =  (1.0 + cosW) / 2.0 / a0;
    f->b1 = -(1.0 + cosW) / a0;
    f->b2 = f->b0;
    f->a1 = (-2.0 * cosW) / a0;
    f->a2 = (1.0 - alpha) / a0;
}

static void bq_set_lp(Biquad *f, double freq, double Q, double fs) {
    double w0 = TWOPI * freq / fs;
    double cosW = cos(w0);
    double alpha = sin(w0) / (2.0 * Q);
    double a0 = 1.0 + alpha;
    f->b0 = (1.0 - cosW) / 2.0 / a0;
    f->b1 = (1.0 - cosW) / a0;
    f->b2 = f->b0;
    f->a1 = (-2.0 * cosW) / a0;
    f->a2 = (1.0 - alpha) / a0;
}

static inline double bq_processL(Biquad *f, double x) {
    double out = f->b0 * x + f->z1L;
    f->z1L = f->b1 * x - f->a1 * out + f->z2L;
    f->z2L = f->b2 * x - f->a2 * out;
    return out;
}

static inline double bq_processR(Biquad *f, double x) {
    double out = f->b0 * x + f->z1R;
    f->z1R = f->b1 * x - f->a1 * out + f->z2R;
    f->z2R = f->b2 * x - f->a2 * out;
    return out;
}

/* ═══════════════════════════════════ Frequency Matrix (8×8) ══ */

static const double freqMatrix[8][8] = {
    {80, 160, 320, 640, 1200, 2500, 5000, 10000},   /* 0: Balanced */
    {150, 300, 500, 800, 1200, 1800, 2600, 5000},   /* 1: Formant */
    {63, 125, 250, 500, 1000, 2000, 4000, 8000},    /* 2: Octave */
    {40, 80, 120, 200, 400, 800, 1600, 6000},       /* 3: Thump */
    {300, 450, 700, 1000, 1400, 2000, 2800, 4000},  /* 4: Radio */
    {55, 110, 220, 440, 880, 1760, 3520, 7040},     /* 5: Acid */
    {50, 130, 320, 800, 2000, 5000, 12500, 16000},  /* 6: Hitsville */
    {29, 115, 411, 777, 1500, 2800, 5200, 11000}    /* 7: West Coast */
};

/* ═══════════════════════════════════ OTO Fixed-Value Tables ══ */

static const double atk_times[6] = {
    0.00005, 0.00033, 0.001, 0.0033, 0.01, 0.033
};  /* 50us, 330us, 1ms, 3.3ms, 10ms, 33ms */

static const double rel_times[6] = {
    0.05, 0.1, 0.25, 0.35, 0.5, 1.0
};  /* 50ms, 100ms, 250ms, 350ms, 500ms, 1s */

static const double gate_thresh[7] = {
    0.0, 0.00316, 0.01, 0.0316, 0.1, 0.316, 1.0
};  /* Off, -50dB, -40dB, -30dB, -20dB, -10dB, 0dB */

/* ═══════════════════════════════════════ Instance Struct ══ */

typedef struct {
    /* Page 1: BOOM */
    float inputGain;   /* 0.5 - 4.0 */
    float compAmount;  /* 0.0 - 1.0 */
    float drive;       /* 1.0 - 20.0 */
    float driveMix;    /* 0.0 - 1.0 (dry/driven blend) */
    float distMode;    /* enum 0-3 */
    float flavor;      /* enum 0-7 */
    float shift;       /* -2.0 - 2.0 */
    float mix;         /* 0.0 - 1.0 */
    float output;      /* 0.0 - 2.0 */

    /* Page 2: SKULPT */
    float bands[8];    /* 0.0 - 2.0 each */
    float micControl;  /* enum 0=Off, 1=On (Vocoder Mode) */
    float vocGain;     /* 0.5 - 2.0 (Vocoder Gain) */

    /* Page 3: MOVE */
    float atk;         /* enum 0-5: 50us,330us,1ms,3.3ms,10ms,33ms */
    float rel;         /* enum 0-5: 50ms,100ms,250ms,350ms,500ms,1s */
    float modShift;    /* -1.0 - 1.0 */
    float modDrive;    /* 0.0 - 1.0 */
    float preType;     /* enum 0-9 */
    float grit;        /* 0.0 - 1.0 */
    float gate;        /* enum 0-6: Off,-50,-40,-30,-20,-10,0dB */
    float link;        /* 0.0 - 1.0 */

    /* Page 4: SEAL */
    float loCut;       /* enum 0-3 */
    float hiCut;       /* 200-18000 Hz direct */
    float sat;         /* 0.0 - 1.0 */
    float age;         /* 0.0 - 1.0 */
    float flutter;     /* 0.0 - 1.0 */
    float bump;        /* 0.0 - 1.0 */
    float thresh;      /* 0.0 - 1.0 */
    float limiter;     /* enum 0=Off, 1=On */
    float bypass;      /* enum 0-1 */

    /* DSP state */
    Biquad bank[8];
    Biquad micBank[8];     /* Mic analysis filterbank (vocoder modulator) */
    double micEnv[8];      /* Per-band envelope followers for mic */
    Biquad loCutF;
    Biquad hiCutF;
    double envL, envR;
    double osL, osR;
    uint32_t rng;
    double gateGain;

    /* Preamp state */
    double preOsL, preOsR; /* Previous sample for preamp 2x oversampling */
    double casLpL, casLpR; /* Cassette one-pole LP filter state */

    /* Flutter delay line */
    double flutBufL[FLUTTER_BUF_SIZE];
    double flutBufR[FLUTTER_BUF_SIZE];
    int flutWr;
    double flutPhase;
    double flutPhase2;
    double flutSmooth;  /* Smoothed flutter amount to prevent clicks */
    double hiCutSmooth; /* Smoothed HiCut freq to prevent clicks */
} superboom_t;

/* ═══════════════════════════════════════════════════ PRNG ══ */

static inline double sb_rand(superboom_t *s) {
    s->rng = (214013u * s->rng + 2531011u);
    return ((double)((s->rng >> 16) & 0x7FFF) / 32768.0) * 2.0 - 1.0;
}

/* ═══════════════════════════════════════ Preamp Models ══ */

/* Mu-law encode/decode for musical bit reduction (DeRez2-style) */
static inline double mulaw_enc(double x) {
    return (x >= 0 ? 1.0 : -1.0) * log(1.0 + 255.0 * fabs(x)) / 5.5451774;
}
static inline double mulaw_dec(double x) {
    return (x >= 0 ? 1.0 : -1.0) * (pow(256.0, fabs(x)) - 1.0) / 255.0;
}

/* ToTape6-style perceptual saturation */
static inline double tape_sat(double x) {
    double ax = fabs(x);
    if (ax < 0.0001) return x;
    double mojo = pow(ax, 0.25);
    return sin(x * mojo * 1.5707963) / mojo;
}

/* Console5-style sin() saturation */
static inline double console_sin(double x) {
    return sin(sb_clampd(x, -1.5707963, 1.5707963));
}

/* Single-sample preamp core (called at 2x rate) */
static void apply_preamp_sample(double *l, double *r, int model,
                                double grit, superboom_t *s) {
    double noise = sb_rand(s) * grit * 0.04;
    /* TPDF dither for bit-crush models */
    double dither = (sb_rand(s) + sb_rand(s)) * 0.5;

    switch (model) {
        case 0: /* Cassette Grunge — tanh + one-pole LP for HF rolloff */
            *l = sb_tanh(*l * (1.5 + grit * 1.5)) + noise;
            *r = sb_tanh(*r * (1.5 + grit * 1.5)) + noise;
            /* Cassette HF rolloff: one-pole LP, cutoff drops with grit */
            { double k = 0.35 + grit * 0.35; /* 0.35-0.70 coeff */
              s->casLpL += k * (*l - s->casLpL);
              s->casLpR += k * (*r - s->casLpR);
              *l = s->casLpL; *r = s->casLpR; }
            break;
        case 1: /* Cassette Grit — atan + one-pole LP */
            *l = atan(*l * (1.2 + grit * 2.0)) * 0.6366 + noise;  /* /pi*2 normalize */
            *r = atan(*r * (1.2 + grit * 2.0)) * 0.6366 + noise;
            { double k = 0.30 + grit * 0.40;
              s->casLpL += k * (*l - s->casLpL);
              s->casLpR += k * (*r - s->casLpR);
              *l = s->casLpL; *r = s->casLpR; }
            break;
        case 2: /* Tape Master — ToTape6 perceptual saturation */
            *l = tape_sat(*l * (1.0 + grit * 0.8)) + noise * 0.5;
            *r = tape_sat(*r * (1.0 + grit * 0.8)) + noise * 0.5;
            break;
        case 3: { /* Tape Slam — dual-stage IronOxide sin(clamp) */
            double drv = 1.4 + grit * 1.6;
            /* Stage 1 */
            *l = sin(sb_clampd(*l * drv, -1.5, 1.5));
            *r = sin(sb_clampd(*r * drv, -1.5, 1.5));
            /* Stage 2 (lighter) */
            *l = sin(sb_clampd(*l * (1.0 + grit * 0.5), -1.5, 1.5)) + noise;
            *r = sin(sb_clampd(*r * (1.0 + grit * 0.5), -1.5, 1.5)) + noise;
            break;
        }
        case 4: /* Tape Thick — heavy tanh with volume duck */
            *l = sb_tanh(*l * (1.8 + grit * 2.0)) * (0.8 - grit * 0.15);
            *r = sb_tanh(*r * (1.8 + grit * 2.0)) * (0.8 - grit * 0.15);
            break;
        case 5: { /* 12-bit — TPDF dithered quantization */
            double res = 16.0 + (1.0 - grit) * 48.0;
            *l = floor(*l * res + dither) / res;
            *r = floor(*r * res + dither) / res;
            break;
        }
        case 6: { /* 8-bit — mu-law companded + TPDF dither */
            double bits = 32.0 - grit * 28.0;
            double el = mulaw_enc(*l);
            double er = mulaw_enc(*r);
            el = floor(el * bits + dither) / bits;
            er = floor(er * bits + dither) / bits;
            *l = mulaw_dec(el);
            *r = mulaw_dec(er);
            break;
        }
        case 7: { /* Brit Console — Console7-style compound curve */
            double h = 0.3 + grit * 1.2;  /* 1.5x internal grit */
            double al = fabs(*l), ar = fabs(*r);
            /* Blend: sin(x*|x|)/|x| (spiral) + sin(x) (density) */
            double spL = (al > 0.001) ? sin(*l * al) / al : *l;
            double spR = (ar > 0.001) ? sin(*r * ar) / ar : *r;
            *l = sb_tanh(spL * (0.6 + h * 0.4) + sin(*l) * h * 0.3);
            *r = sb_tanh(spR * (0.6 + h * 0.4) + sin(*r) * h * 0.3);
            break;
        }
        case 8: /* USA Console — Console5-style sin() saturation */
            *l = console_sin(*l * (1.0 + grit * 1.5));  /* 1.25x internal grit */
            *r = console_sin(*r * (1.0 + grit * 1.5));
            break;
        case 9: /* Clean */
            *l = *l * (1.0 + grit * 0.3) + noise * 0.3;
            *r = *r * (1.0 + grit * 0.3) + noise * 0.3;
            break;
        default:
            break;
    }
}

/* 2x oversampled preamp wrapper — gain-neutral */
static void apply_preamp(double *L, double *R, int model, double grit,
                         superboom_t *s) {
    /* Measure input level */
    double inL = *L, inR = *R;

    /* Phase 1: interpolated half-sample */
    double h1L = (*L + s->preOsL) * 0.5;
    double h1R = (*R + s->preOsR) * 0.5;
    apply_preamp_sample(&h1L, &h1R, model, grit, s);

    /* Phase 2: current sample */
    double h2L = *L, h2R = *R;
    apply_preamp_sample(&h2L, &h2R, model, grit, s);

    /* Store for next oversample interpolation */
    s->preOsL = *L;
    s->preOsR = *R;

    /* Average both phases (implicit 2x downsample) */
    double outL = (h1L + h2L) * 0.5;
    double outR = (h1R + h2R) * 0.5;

    /* Gain compensation: match output RMS to input RMS */
    double inPow = inL * inL + inR * inR;
    double outPow = outL * outL + outR * outR;
    if (outPow > 1e-12 && inPow > 1e-12) {
        double comp = sqrt(inPow / outPow);
        /* Limit compensation range to avoid pumping */
        comp = sb_clampd(comp, 0.5, 2.0);
        outL *= comp;
        outR *= comp;
    }

    *L = outL;
    *R = outR;
}

/* ═══════════════════════════════════════ Compressor ══ */

static double calc_comp_gain(double det, double amt, double tOff) {
    double thresh_db = -30.0 + (tOff * 25.0);
    double db = 20.0 * log10(det + 1e-9);
    if (db > thresh_db) {
        /* amt 0-0.7: standard compression, 0.7-1.0: negative ratio */
        double slope = (amt > 0.7) ? 2.0 : amt * 1.42;
        return pow(10.0, (-(db - thresh_db) * slope) / 20.0);
    }
    return 1.0;
}

/* ═══════════════════════════════════════ Distortion ══ */

static inline double apply_dist(double x, int mode) {
    switch (mode) {
        case 0: return sin(sb_clampd(x, -1.57, 1.57));                  /* Boost */
        case 1: return (x > 0) ? sb_tanh(x) : (x * 0.55);              /* Tube */
        case 2: return sb_clampd(x * 1.4, -0.85, 0.85);                /* Fuzz */
        case 3: return (x > 0.3) ? 0.9 : (x < -0.3 ? -0.9 : 0.0);    /* Square */
        default: return x;
    }
}

/* ═══════════════════════════════════════ Tape Stage ══ */

static inline double apply_tape(double x, double sat, double age,
                                double bump) {
    double out = sb_tanh(x * (1.0 + sat));
    double lowBump = sin(x * 0.5) * bump * 0.1;
    return (out + lowBump) * (1.0 - (age * 0.25));
}

/* ═══════════════════════════════════════ Flutter ══ */

static void apply_flutter(superboom_t *s, double *L, double *R,
                          double amt) {
    /* Smooth the flutter amount to prevent clicks on parameter changes */
    double smoothCoeff = 0.9995;  /* ~100ms smoothing */
    s->flutSmooth = smoothCoeff * s->flutSmooth + (1.0 - smoothCoeff) * amt;
    double smoothAmt = s->flutSmooth;

    /* Write current sample */
    s->flutBufL[s->flutWr] = *L;
    s->flutBufR[s->flutWr] = *R;

    if (smoothAmt < 0.0005) {
        s->flutWr = (s->flutWr + 1) & (FLUTTER_BUF_SIZE - 1);
        return;
    }

    /* Two-LFO modulation: slow wow + faster flutter */
    double lfo1 = sin(s->flutPhase);
    double lfo2 = sin(s->flutPhase2) * 0.3;
    double mod = (lfo1 + lfo2) * smoothAmt;

    /* Delay modulation: base + swing */
    double delaySamples = 64.0 + mod * 60.0;

    /* Read with linear interpolation */
    double readPos = (double)s->flutWr - delaySamples;
    if (readPos < 0) readPos += FLUTTER_BUF_SIZE;
    int idx0 = (int)readPos;
    int idx1 = (idx0 + 1) & (FLUTTER_BUF_SIZE - 1);
    idx0 &= (FLUTTER_BUF_SIZE - 1);
    double frac = readPos - floor(readPos);

    *L = s->flutBufL[idx0] * (1.0 - frac) + s->flutBufL[idx1] * frac;
    *R = s->flutBufR[idx0] * (1.0 - frac) + s->flutBufR[idx1] * frac;

    /* Advance LFO phases */
    s->flutPhase  += TWOPI * 3.7 / SR;   /* ~3.7 Hz wow */
    s->flutPhase2 += TWOPI * 8.3 / SR;   /* ~8.3 Hz flutter */
    if (s->flutPhase  > TWOPI) s->flutPhase  -= TWOPI;
    if (s->flutPhase2 > TWOPI) s->flutPhase2 -= TWOPI;

    s->flutWr = (s->flutWr + 1) & (FLUTTER_BUF_SIZE - 1);
}

/* ═══════════════════════════════════ Main Process Block ══ */

static void process_block(void *inst, int16_t *audio_inout, int frames) {
    superboom_t *s = (superboom_t *)inst;
    if (s->bypass > 0.5f) return;

    /* ── Mic input pointer (shared memory hardware input) ── */
    int16_t *micBuf = NULL;
    int useMic = (s->micControl > 0.5f) && g_host && g_host->mapped_memory;
    if (useMic) {
        micBuf = (int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);
    }

    /* ── Block-rate coefficient update ── */
    int flav = (int)sb_clampf(s->flavor, 0.0f, 7.0f);

    /* Envelope-modulated shift (uses previous block's envelope) */
    double envMax = fmax(s->envL, s->envR);
    double effShift = (double)s->shift + envMax * (double)s->modShift * 2.0;
    double shiftMult = pow(2.0, effShift);

    /* Mic envelope follower coefficients (~5ms attack, ~20ms release) */
    double micAtkCoeff = exp(-1.0 / (SR * 0.005));
    double micRelCoeff = exp(-1.0 / (SR * 0.020));

    for (int i = 0; i < 8; i++) {
        double fc = sb_clampd(freqMatrix[flav][i] * shiftMult, 20.0,
                              SR * 0.45);
        bq_set_bp(&s->bank[i], fc, 1.2, SR * 2.0);
        /* Mic analysis filters at same frequencies (1x rate, narrower Q) */
        if (useMic) bq_set_bp(&s->micBank[i], fc, 2.0, SR);
    }

    /* Wet-path filters */
    double loFreq = (s->loCut < 1.0f) ? 20.0 :
                    (s->loCut < 2.0f) ? 75.0 :
                    (s->loCut < 3.0f) ? 150.0 : 300.0;
    bq_set_hp(&s->loCutF, loFreq, 0.707, SR);
    /* HiCut: smoothed direct Hz value (~100ms ramp) */
    double hiCutCoeff = exp(-1.0 / (SR * 0.1 / (double)frames));
    s->hiCutSmooth = hiCutCoeff * s->hiCutSmooth
                   + (1.0 - hiCutCoeff) * (double)s->hiCut;
    bq_set_lp(&s->hiCutF,
              sb_clampd(s->hiCutSmooth, 20.0, SR * 0.49), 0.707, SR);

    /* Envelope ballistics (OTO fixed attack/release times) */
    int atkIdx = (int)sb_clampf(s->atk, 0.0f, 5.0f);
    int relIdx = (int)sb_clampf(s->rel, 0.0f, 5.0f);
    double aAtk = exp(-1.0 / (SR * (atk_times[atkIdx] + 0.00001)));
    double aRel = exp(-1.0 / (SR * (rel_times[relIdx] + 0.0001)));

    /* Gate threshold (OTO fixed levels) */
    int gateIdx = (int)sb_clampf(s->gate, 0.0f, 6.0f);
    double gateThreshold = gate_thresh[gateIdx];
    double lnk = (double)s->link;
    int dMode = (int)s->distMode;

    /* ── Sample loop ── */
    for (int n = 0; n < frames; n++) {
        double rawL = audio_inout[n * 2]     / 32767.0;
        double rawR = audio_inout[n * 2 + 1] / 32767.0;

        /* A. Input Gain + Preamp */
        double L = rawL * (double)s->inputGain;
        double R = rawR * (double)s->inputGain;
        apply_preamp(&L, &R, (int)s->preType, (double)s->grit, s);

        /* B. Stereo-Linked Envelope Followers */
        double detL = fabs(L);
        double detR = fabs(R);
        if (detL > s->envL) s->envL = aAtk * s->envL + (1.0 - aAtk) * detL;
        else                s->envL = aRel * s->envL + (1.0 - aRel) * detL;
        if (detR > s->envR) s->envR = aAtk * s->envR + (1.0 - aAtk) * detR;
        else                s->envR = aRel * s->envR + (1.0 - aRel) * detR;

        double envLinked = fmax(s->envL, s->envR);
        double envForL = lnk * envLinked + (1.0 - lnk) * s->envL;
        double envForR = lnk * envLinked + (1.0 - lnk) * s->envR;

        /* C. Compressor (negative ratio above 70%) */
        /* thresh 0-1 maps to 0.7-1.0 internal range */
        double effThresh = 0.7 + (double)s->thresh * 0.3;
        double cGainL = calc_comp_gain(envForL, (double)s->compAmount,
                                       effThresh);
        double cGainR = calc_comp_gain(envForR, (double)s->compAmount,
                                       effThresh);
        L *= cGainL;
        R *= cGainR;

        /* Make-up gain: compensate for compression level reduction */
        double makeup = 1.0 + (double)s->compAmount * 0.7;
        L *= makeup;
        R *= makeup;

        /* Capture dry for mix */
        double dryL = L, dryR = R;

        /* D. 2x Oversampled Filterbank + Distortion */
        double l1 = L, l2 = (L + s->osL) * 0.5;
        double r1 = R, r2 = (R + s->osR) * 0.5;
        s->osL = L;
        s->osR = R;

        double dynDrive = (double)s->drive +
                          envForL * (double)s->modDrive * 10.0;

        /* Mic vocoder modulation: analyse mic per-band energy */
        double micGain[8];
        if (useMic) {
            double micMono = (micBuf[n * 2] + micBuf[n * 2 + 1])
                             / (2.0 * 32767.0);
            for (int i = 0; i < 8; i++) {
                double bandSig = bq_processL(&s->micBank[i], micMono);
                double det = fabs(bandSig);
                if (det > s->micEnv[i])
                    s->micEnv[i] = micAtkCoeff * s->micEnv[i]
                                 + (1.0 - micAtkCoeff) * det;
                else
                    s->micEnv[i] = micRelCoeff * s->micEnv[i]
                                 + (1.0 - micRelCoeff) * det;
                /* Scale envelope to usable gain, apply vocoder gain */
                micGain[i] = sb_clampd(s->micEnv[i] * 8.0 * (double)s->vocGain,
                                       0.0, 1.0);
            }
        }

        double dmx = (double)s->driveMix;

        /* Phase 1 */
        {
            double fL = 0, fR = 0;
            for (int i = 0; i < 8; i++) {
                double bL = bq_processL(&s->bank[i], l1) * (double)s->bands[i];
                double bR = bq_processR(&s->bank[i], r1) * (double)s->bands[i];
                if (useMic) { bL *= micGain[i]; bR *= micGain[i]; }
                fL += bL;
                fR += bR;
            }
            /* Drive Mix: blend filtered-clean with filtered+distorted */
            double dL = apply_dist(fL * dynDrive, dMode);
            double dR = apply_dist(fR * dynDrive, dMode);
            l1 = dL * dmx + fL * (1.0 - dmx);
            r1 = dR * dmx + fR * (1.0 - dmx);
        }
        /* Phase 2 */
        {
            double fL = 0, fR = 0;
            for (int i = 0; i < 8; i++) {
                double bL = bq_processL(&s->bank[i], l2) * (double)s->bands[i];
                double bR = bq_processR(&s->bank[i], r2) * (double)s->bands[i];
                if (useMic) { bL *= micGain[i]; bR *= micGain[i]; }
                fL += bL;
                fR += bR;
            }
            double dL = apply_dist(fL * dynDrive, dMode);
            double dR = apply_dist(fR * dynDrive, dMode);
            l2 = dL * dmx + fL * (1.0 - dmx);
            r2 = dR * dmx + fR * (1.0 - dmx);
        }

        double wetL = (l1 + l2) * 0.5;
        double wetR = (r1 + r2) * 0.5;

        /* E. Wet Filters (before mix per OTO spec) */
        wetL = bq_processL(&s->hiCutF, bq_processL(&s->loCutF, wetL));
        wetR = bq_processR(&s->hiCutF, bq_processR(&s->loCutF, wetR));

        /* F. Mix */
        double mxAmt = (double)s->mix;
        double mixL = wetL * mxAmt + dryL * (1.0 - mxAmt);
        double mixR = wetR * mxAmt + dryR * (1.0 - mxAmt);

        /* G. Gate (silence noise from Square mode / preamp) */
        if (gateIdx > 0) {
            double gTarget = (envLinked > gateThreshold) ? 1.0 : 0.0;
            double gCoeff = (gTarget > s->gateGain) ? 0.99 : 0.9995;
            s->gateGain = gCoeff * s->gateGain + (1.0 - gCoeff) * gTarget;
            mixL *= s->gateGain;
            mixR *= s->gateGain;
        }

        /* H. Tape Stage */
        mixL = apply_tape(mixL, (double)s->sat, (double)s->age,
                          (double)s->bump);
        mixR = apply_tape(mixR, (double)s->sat, (double)s->age,
                          (double)s->bump);

        /* I. Flutter */
        apply_flutter(s, &mixL, &mixR, (double)s->flutter);

        /* J. Output */
        mixL *= (double)s->output;
        mixR *= (double)s->output;

        /* K. Limiter (soft-knee brickwall) */
        if (s->limiter > 0.5f) {
            mixL = sb_tanh(mixL);
            mixR = sb_tanh(mixR);
        }

        audio_inout[n * 2]     = (int16_t)(sb_clampd(mixL, -1.0, 1.0) *
                                           32767.0);
        audio_inout[n * 2 + 1] = (int16_t)(sb_clampd(mixR, -1.0, 1.0) *
                                           32767.0);
    }
}

/* ═══════════════════════════════════ Instance Lifecycle ══ */

static void *create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    superboom_t *s = (superboom_t *)calloc(1, sizeof(superboom_t));
    if (!s) return NULL;

    /* Defaults — Page 1: BOOM */
    s->inputGain = 1.0f;
    s->compAmount = 0.0f;
    s->drive = 1.0f;
    s->driveMix = 1.0f;
    s->distMode = 0.0f;
    s->flavor = 0.0f;
    s->shift = 0.0f;
    s->mix = 1.0f;
    s->output = 1.0f;

    /* Defaults — Page 2: SKULPT */
    for (int i = 0; i < 8; i++) s->bands[i] = 1.0f;

    /* Defaults — Page 3: MOVE */
    s->atk = 0.0f;      /* index 0 = 50us */
    s->rel = 0.0f;      /* index 0 = 50ms */
    s->micControl = 0.0f;  /* Off */
    s->vocGain = 1.0f;
    s->modShift = 0.0f;
    s->modDrive = 0.0f;
    s->preType = 9.0f;   /* Clean */
    s->grit = 0.0f;
    s->gate = 0.0f;      /* Off */
    s->link = 1.0f;

    /* Defaults — Page 4: SEAL */
    s->loCut = 0.0f;
    s->hiCut = 20000.0f;  /* Direct Hz */
    s->sat = 0.0f;
    s->age = 0.0f;
    s->flutter = 0.0f;
    s->bump = 0.0f;
    s->thresh = 0.9f;
    s->limiter = 0.0f;    /* Off */
    s->bypass = 0.0f;

    /* Init DSP state */
    for (int i = 0; i < 8; i++) {
        bq_reset(&s->bank[i]);
        bq_reset(&s->micBank[i]);
        s->micEnv[i] = 0.0;
    }
    bq_reset(&s->loCutF);
    bq_reset(&s->hiCutF);
    s->rng = 12345;
    s->gateGain = 1.0;
    s->hiCutSmooth = 20000.0;

    return s;
}

static void destroy_instance(void *inst) {
    free(inst);
}

/* ═══════════════════════════════════════ Enum Tables ══ */

static const char *distMode_opts[] = {"Boost","Tube","Fuzz","Square"};
static const char *flavor_opts[]   = {"Bal","Form","Oct","Thump",
                                      "Radio","Acid","Motwn","West"};
static const char *preType_opts[]  = {"Cass1","Cass2","Mast","Slam","Thick",
                                      "12bit","8bit","Brit","USA","Cln"};
static const char *atk_opts[]      = {"50us","330us","1ms","3.3ms","10ms","33ms"};
static const char *rel_opts[]      = {"50ms","100ms","250ms","350ms","500ms","1s"};
static const char *gate_opts[]     = {"Off","-50dB","-40dB","-30dB","-20dB","-10dB","0dB"};
static const char *loCut_opts[]    = {"Off","75","150","300"};
static const char *bypass_opts[]   = {"On","Byp"};
static const char *micControl_opts[] = {"Off","On"};
static const char *limiter_opts[] = {"Off","On"};

static int match_enum(const char *value, const char **opts, int count) {
    for (int i = 0; i < count; i++)
        if (strcmp(value, opts[i]) == 0) return i;
    return -1;
}

static void set_enum(float *dst, const char *value, const char **opts,
                     int count) {
    int idx = match_enum(value, opts, count);
    if (idx >= 0) { *dst = (float)idx; return; }
    *dst = sb_clampf((float)(int)round(atof(value)), 0.0f,
                     (float)(count - 1));
}

/* ═══════════════════════════════════════ set_param ══ */

#define SETFR(k,f,lo,hi) \
    if(strcmp(key,k)==0){ \
        s->f=sb_clampf((float)atof(value),(float)(lo),(float)(hi)); \
        return; \
    }

static void set_param(void *inst, const char *key, const char *value) {
    superboom_t *s = (superboom_t *)inst;
    if (!key || !value) return;

    /* Page 1: BOOM */
    SETFR("inputGain", inputGain, 0.5, 4.0)
    SETFR("compAmount", compAmount, 0.0, 1.0)
    SETFR("drive", drive, 1.0, 20.0)
    SETFR("driveMix", driveMix, 0.0, 1.0)
    if(strcmp(key,"distMode")==0){set_enum(&s->distMode,value,distMode_opts,4);return;}
    if(strcmp(key,"flavor")==0){set_enum(&s->flavor,value,flavor_opts,8);return;}
    SETFR("shift", shift, -2.0, 2.0)
    SETFR("mix", mix, 0.0, 1.0)
    SETFR("output", output, 0.0, 2.0)

    /* Page 2: SKULPT */
    SETFR("b1", bands[0], 0.0, 2.0)
    SETFR("b2", bands[1], 0.0, 2.0)
    SETFR("b3", bands[2], 0.0, 2.0)
    SETFR("b4", bands[3], 0.0, 2.0)
    SETFR("b5", bands[4], 0.0, 2.0)
    SETFR("b6", bands[5], 0.0, 2.0)
    SETFR("b7", bands[6], 0.0, 2.0)
    SETFR("b8", bands[7], 0.0, 2.0)
    if(strcmp(key,"micControl")==0){set_enum(&s->micControl,value,micControl_opts,2);return;}
    SETFR("vocGain", vocGain, 0.5, 2.0)

    /* Page 3: MOVE */
    if(strcmp(key,"atk")==0){set_enum(&s->atk,value,atk_opts,6);return;}
    if(strcmp(key,"rel")==0){set_enum(&s->rel,value,rel_opts,6);return;}
    SETFR("modShift", modShift, -1.0, 1.0)
    SETFR("modDrive", modDrive, 0.0, 1.0)
    if(strcmp(key,"preType")==0){set_enum(&s->preType,value,preType_opts,10);return;}
    SETFR("grit", grit, 0.0, 1.0)
    if(strcmp(key,"gate")==0){set_enum(&s->gate,value,gate_opts,7);return;}
    SETFR("link", link, 0.0, 1.0)

    /* Page 4: SEAL */
    if(strcmp(key,"loCut")==0){set_enum(&s->loCut,value,loCut_opts,4);return;}
    SETFR("hiCut", hiCut, 20.0, 20000.0)
    SETFR("sat", sat, 0.0, 1.0)
    SETFR("age", age, 0.0, 1.0)
    SETFR("flutter", flutter, 0.0, 1.0)
    SETFR("bump", bump, 0.0, 1.0)
    SETFR("thresh", thresh, 0.0, 1.0)
    if(strcmp(key,"limiter")==0){set_enum(&s->limiter,value,limiter_opts,2);return;}
    if(strcmp(key,"bypass")==0){set_enum(&s->bypass,value,bypass_opts,2);return;}
}

/* ═══════════════════════════════════════ get_param ══ */

#define GETP(k,f) \
    if(strcmp(key,k)==0) return snprintf(buf,buf_len,"%.4f",(double)s->f);

#define GETE(k,f,opts,cnt) do { \
    if(strcmp(key,k)==0){ \
        int _i=(int)roundf(s->f); \
        if(_i<0) _i=0; if(_i>=(cnt)) _i=(cnt)-1; \
        return snprintf(buf,buf_len,"%s",(opts)[_i]); \
    } } while(0)

static int get_param(void *inst, const char *key, char *buf, int buf_len) {
    superboom_t *s = (superboom_t *)inst;
    if (!key) return -1;

    /* ── chain_params ── */
    if (strcmp(key, "chain_params") == 0) {
        static const char *cp =
        "["
        /* Page 1: BOOM */
        "{\"key\":\"inputGain\",\"name\":\"Input\",\"type\":\"float\","
          "\"min\":0.5,\"max\":4,\"default\":1,\"step\":0.01},"
        "{\"key\":\"compAmount\",\"name\":\"Comp\",\"type\":\"float\","
          "\"min\":0,\"max\":1,\"default\":0,\"step\":0.01},"
        "{\"key\":\"drive\",\"name\":\"Drive\",\"type\":\"float\","
          "\"min\":1,\"max\":20,\"default\":1,\"step\":0.01},"
        "{\"key\":\"driveMix\",\"name\":\"DrvMx\",\"type\":\"float\","
          "\"min\":0,\"max\":1,\"default\":1,\"step\":0.01},"
        "{\"key\":\"distMode\",\"name\":\"Mode\",\"type\":\"enum\","
          "\"options\":[\"Boost\",\"Tube\",\"Fuzz\",\"Square\"],\"default\":0},"
        "{\"key\":\"flavor\",\"name\":\"Flavor\",\"type\":\"enum\","
          "\"options\":[\"Bal\",\"Form\",\"Oct\",\"Thump\","
          "\"Radio\",\"Acid\",\"Motwn\",\"West\"],\"default\":0},"
        "{\"key\":\"shift\",\"name\":\"Shift\",\"type\":\"float\","
          "\"min\":-2,\"max\":2,\"default\":0,\"step\":0.01},"
        "{\"key\":\"mix\",\"name\":\"Mix\",\"type\":\"float\","
          "\"min\":0,\"max\":1,\"default\":1,\"step\":0.01},"
        "{\"key\":\"output\",\"name\":\"Level\",\"type\":\"float\","
          "\"min\":0,\"max\":2,\"default\":1,\"step\":0.01},"
        /* Page 2: SKULPT */
        "{\"key\":\"b1\",\"name\":\"Low\",\"type\":\"float\","
          "\"min\":0,\"max\":2,\"default\":1,\"step\":0.01},"
        "{\"key\":\"b2\",\"name\":\"B2\",\"type\":\"float\","
          "\"min\":0,\"max\":2,\"default\":1,\"step\":0.01},"
        "{\"key\":\"b3\",\"name\":\"B3\",\"type\":\"float\","
          "\"min\":0,\"max\":2,\"default\":1,\"step\":0.01},"
        "{\"key\":\"b4\",\"name\":\"Mid\",\"type\":\"float\","
          "\"min\":0,\"max\":2,\"default\":1,\"step\":0.01},"
        "{\"key\":\"b5\",\"name\":\"B5\",\"type\":\"float\","
          "\"min\":0,\"max\":2,\"default\":1,\"step\":0.01},"
        "{\"key\":\"b6\",\"name\":\"B6\",\"type\":\"float\","
          "\"min\":0,\"max\":2,\"default\":1,\"step\":0.01},"
        "{\"key\":\"b7\",\"name\":\"B7\",\"type\":\"float\","
          "\"min\":0,\"max\":2,\"default\":1,\"step\":0.01},"
        "{\"key\":\"b8\",\"name\":\"High\",\"type\":\"float\","
          "\"min\":0,\"max\":2,\"default\":1,\"step\":0.01},"
        "{\"key\":\"micControl\",\"name\":\"Voc\",\"type\":\"enum\","
          "\"options\":[\"Off\",\"On\"],\"default\":0},"
        "{\"key\":\"vocGain\",\"name\":\"VocGn\",\"type\":\"float\","
          "\"min\":0.5,\"max\":2,\"default\":1,\"step\":0.01},"
        "{\"key\":\"modShift\",\"name\":\"M-Shft\",\"type\":\"float\","
          "\"min\":-1,\"max\":1,\"default\":0,\"step\":0.01},"
        /* Page 3: Pre&Comp */
        "{\"key\":\"atk\",\"name\":\"Atk\",\"type\":\"enum\","
          "\"options\":[\"50us\",\"330us\",\"1ms\",\"3.3ms\",\"10ms\",\"33ms\"],\"default\":0},"
        "{\"key\":\"rel\",\"name\":\"Rel\",\"type\":\"enum\","
          "\"options\":[\"50ms\",\"100ms\",\"250ms\",\"350ms\",\"500ms\",\"1s\"],\"default\":0},"
        "{\"key\":\"thresh\",\"name\":\"Thresh\",\"type\":\"float\","
          "\"min\":0,\"max\":1,\"default\":0.9,\"step\":0.01},"
        "{\"key\":\"modDrive\",\"name\":\"M-Drv\",\"type\":\"float\","
          "\"min\":0,\"max\":1,\"default\":0,\"step\":0.01},"
        "{\"key\":\"preType\",\"name\":\"Pre\",\"type\":\"enum\","
          "\"options\":[\"Cass1\",\"Cass2\",\"Mast\",\"Slam\",\"Thick\","
          "\"12bit\",\"8bit\",\"Brit\",\"USA\",\"Cln\"],\"default\":9},"
        "{\"key\":\"grit\",\"name\":\"Grit\",\"type\":\"float\","
          "\"min\":0,\"max\":1,\"default\":0,\"step\":0.01},"
        "{\"key\":\"gate\",\"name\":\"Gate\",\"type\":\"enum\","
          "\"options\":[\"Off\",\"-50dB\",\"-40dB\",\"-30dB\",\"-20dB\",\"-10dB\",\"0dB\"],\"default\":0},"
        "{\"key\":\"link\",\"name\":\"Link\",\"type\":\"float\","
          "\"min\":0,\"max\":1,\"default\":1,\"step\":0.01},"
        /* Page 4: SEAL */
        "{\"key\":\"loCut\",\"name\":\"LoCut\",\"type\":\"enum\","
          "\"options\":[\"Off\",\"75\",\"150\",\"300\"],\"default\":0},"
        "{\"key\":\"hiCut\",\"name\":\"HiCut\",\"type\":\"int\","
          "\"min\":20,\"max\":20000,\"default\":20000,\"step\":10},"
        "{\"key\":\"sat\",\"name\":\"Sat\",\"type\":\"float\","
          "\"min\":0,\"max\":1,\"default\":0,\"step\":0.01},"
        "{\"key\":\"age\",\"name\":\"Age\",\"type\":\"float\","
          "\"min\":0,\"max\":1,\"default\":0,\"step\":0.01},"
        "{\"key\":\"flutter\",\"name\":\"Flutter\",\"type\":\"float\","
          "\"min\":0,\"max\":1,\"default\":0,\"step\":0.01},"
        "{\"key\":\"bump\",\"name\":\"Bump\",\"type\":\"float\","
          "\"min\":0,\"max\":1,\"default\":0,\"step\":0.01},"
        "{\"key\":\"limiter\",\"name\":\"Limit\",\"type\":\"enum\","
          "\"options\":[\"Off\",\"On\"],\"default\":0},"
        "{\"key\":\"bypass\",\"name\":\"Bypass\",\"type\":\"enum\","
          "\"options\":[\"On\",\"Byp\"],\"default\":0}"
        "]";
        int len = (int)strlen(cp);
        if (len >= buf_len) return -1;
        memcpy(buf, cp, len + 1);
        return len;
    }

    /* ── ui_hierarchy ── */
    if (strcmp(key, "ui_hierarchy") == 0) {
        static const char *hier =
        "{\"modes\":null,"
        "\"levels\":{"
          "\"root\":{\"name\":\"Super Boom\","
            "\"knobs\":[\"inputGain\",\"compAmount\",\"drive\",\"driveMix\","
              "\"distMode\",\"shift\",\"mix\",\"output\"],"
            "\"params\":["
              "{\"level\":\"BOOM\",\"label\":\"Boom\"},"
              "{\"level\":\"SKULPT\",\"label\":\"Skulpt\"},"
              "{\"level\":\"PRECOMP\",\"label\":\"Pre&Comp\"},"
              "{\"level\":\"SEAL\",\"label\":\"Seal\"}"
            "]},"
          "\"BOOM\":{\"label\":\"Boom\","
            "\"knobs\":[\"inputGain\",\"compAmount\",\"drive\",\"driveMix\","
              "\"distMode\",\"shift\",\"mix\",\"output\"],"
            "\"params\":[\"inputGain\",\"compAmount\",\"drive\",\"driveMix\","
              "\"distMode\",\"shift\",\"mix\",\"output\"]},"
          "\"SKULPT\":{\"label\":\"Skulpt\","
            "\"knobs\":[\"b1\",\"b2\",\"b3\",\"b4\","
              "\"b5\",\"b6\",\"b7\",\"b8\"],"
            "\"params\":[\"b1\",\"b2\",\"b3\",\"b4\","
              "\"b5\",\"b6\",\"b7\",\"b8\",\"modShift\",\"flavor\",\"micControl\",\"vocGain\"]},"
          "\"PRECOMP\":{\"label\":\"Pre&Comp\","
            "\"knobs\":[\"atk\",\"rel\",\"thresh\",\"modDrive\","
              "\"preType\",\"grit\",\"gate\",\"link\"],"
            "\"params\":[\"atk\",\"rel\",\"thresh\",\"modDrive\","
              "\"preType\",\"grit\",\"gate\",\"link\"]},"
          "\"SEAL\":{\"label\":\"Seal\","
            "\"knobs\":[\"loCut\",\"hiCut\",\"sat\",\"age\","
              "\"flutter\",\"bump\",\"limiter\",\"bypass\"],"
            "\"params\":[\"loCut\",\"hiCut\",\"sat\",\"age\","
              "\"flutter\",\"bump\",\"limiter\",\"bypass\"]}"
        "}}";
        int len = (int)strlen(hier);
        if (len >= buf_len) return -1;
        memcpy(buf, hier, len + 1);
        return len;
    }

    /* ── Individual params ── */

    /* Page 1: BOOM */
    GETP("inputGain", inputGain)
    GETP("compAmount", compAmount)
    GETP("drive", drive)
    GETP("driveMix", driveMix)
    GETE("distMode", distMode, distMode_opts, 4);
    GETE("flavor", flavor, flavor_opts, 8);
    GETP("shift", shift)
    GETP("mix", mix)
    GETP("output", output)

    /* Page 2: SKULPT */
    GETP("b1", bands[0])
    GETP("b2", bands[1])
    GETP("b3", bands[2])
    GETP("b4", bands[3])
    GETP("b5", bands[4])
    GETP("b6", bands[5])
    GETP("b7", bands[6])
    GETP("b8", bands[7])
    GETE("micControl", micControl, micControl_opts, 2);
    GETP("vocGain", vocGain)

    /* Page 3: MOVE */
    GETE("atk", atk, atk_opts, 6);
    GETE("rel", rel, rel_opts, 6);
    GETP("modShift", modShift)
    GETP("modDrive", modDrive)
    GETE("preType", preType, preType_opts, 10);
    GETP("grit", grit)
    GETE("gate", gate, gate_opts, 7);
    GETP("link", link)

    /* Page 4: SEAL */
    GETE("loCut", loCut, loCut_opts, 4);
    if(strcmp(key,"hiCut")==0) return snprintf(buf,buf_len,"%d",(int)s->hiCut);
    GETP("sat", sat)
    GETP("age", age)
    GETP("flutter", flutter)
    GETP("bump", bump)
    GETP("thresh", thresh)
    GETE("limiter", limiter, limiter_opts, 2);
    GETE("bypass", bypass, bypass_opts, 2);

    return -1;
}

/* ═══════════════════════════════════════ API Export ══ */

static audio_fx_api_v2_t api = {
    .api_version      = AUDIO_FX_API_VERSION_2,
    .create_instance  = create_instance,
    .destroy_instance = destroy_instance,
    .process_block    = process_block,
    .set_param        = set_param,
    .get_param        = get_param,
    .on_midi          = NULL
};

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &api;
}
