/*
 * Cyber Acoustic Feedbacker & Polyphonic Sustainer - LV2 Plugin
 * Copyright (c) 2026 Cyber Audio
 *
 * Core DSP Architecture:
 *  1. YIN Pitch Detector: Real-time fundamental pitch and vibrato tracking.
 *  2. Crest Factor & Sustain Envelope Analyzer:
 *     - Fast riffs & transients -> Feedback ducked/silent.
 *     - Held sustained notes/chords -> Feedback catches and blooms exponentially.
 *  3. Triple Mode Feedback Engine:
 *     - ROOT DOMINANT: Locks to strongest chord root note -> singing 5th or octave solo feedback.
 *     - POLY SUSTAIN: Multi-band regenerative resonator bank -> infinite full chord sustain (6-string E-Bow).
 *     - HARMONIC BLOOM: Full chord sustain with shimmering upper octave overtones.
 *  4. Nonlinear Asymmetric Saturation & String Energy Coupling.
 */

#include "lv2.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PLUGIN_URI "http://cyber-audio.co.uk/plugins/cyber-acoustic-feedbacker"
#define YIN_BUFFER_SIZE 2048

enum PortIndex {
    PORT_AUDIO_IN_L    = 0,
    PORT_AUDIO_IN_R    = 1,
    PORT_AUDIO_OUT_L   = 2,
    PORT_AUDIO_OUT_R   = 3,
    PORT_BYPASS        = 4,
    PORT_MODE          = 5,
    PORT_BLOOM         = 6,
    PORT_SENS          = 7,
    PORT_COUPLING      = 8,
    PORT_HARMONIC      = 9,
    PORT_WARMTH        = 10,
    PORT_VIBRATO       = 11,
    PORT_MIX           = 12
};

// Resonant Comb Resonator for String Feedback Emulation
struct StringResonator {
    float buffer[4096];
    int size;
    int write_pos;
    float damping_state;

    void init() {
        memset(buffer, 0, sizeof(buffer));
        size = 1000;
        write_pos = 0;
        damping_state = 0.0f;
    }

    inline float process(float in, float delay_samples, float feedback_gain, float damping_coeff) {
        int max_sz = 4090;
        float d_s = std::max(4.0f, std::min((float)max_sz, delay_samples));
        float r_pos = (float)write_pos - d_s;
        while (r_pos < 0) r_pos += 4096;
        while (r_pos >= 4096) r_pos -= 4096;

        int i0 = (int)r_pos;
        int i1 = (i0 + 1) % 4096;
        float frac = r_pos - (float)i0;
        float delayed = buffer[i0] + frac * (buffer[i1] - buffer[i0]);

        // One-pole lowpass damping inside string loop
        damping_state += damping_coeff * (delayed - damping_state);

        // Soft asymmetric saturation in string feedback loop
        float saturated = tanhf((in + damping_state * feedback_gain) * 1.5f);

        buffer[write_pos] = in + saturated * feedback_gain;
        if (++write_pos >= 4096) write_pos = 0;

        return saturated;
    }
};

class CyberAcousticFeedbacker {
private:
    double sample_rate;

    // Pitch Tracker Buffers
    float yin_buf[YIN_BUFFER_SIZE];
    int yin_idx;
    float detected_freq;
    float tracked_period_samples;

    // Envelope & Crest Factor
    float fast_env;
    float slow_env;
    float sustain_timer;
    float feedback_bloom_gain;

    // Resonator Engine (1 Monophonic + 6 Polyphonic Resonators)
    StringResonator mono_res;
    StringResonator poly_res[6];

    // Tone Filters
    float lp_l, lp_r;

    // Ports
    const float* p_in_l;
    const float* p_in_r;
    float* p_out_l;
    float* p_out_r;
    const float* p_bypass;
    const float* p_mode;
    const float* p_bloom;
    const float* p_sens;
    const float* p_coupling;
    const float* p_harmonic;
    const float* p_warmth;
    const float* p_vibrato;
    const float* p_mix;

    // YIN Pitch Detection
    void detect_pitch() {
        int half_w = YIN_BUFFER_SIZE / 2;
        float d[half_w];
        d[0] = 1.0f;

        // Difference function
        for (int tau = 1; tau < half_w; ++tau) {
            float sum = 0.0f;
            for (int j = 0; j < half_w; ++j) {
                float diff = yin_buf[j] - yin_buf[j + tau];
                sum += diff * diff;
            }
            d[tau] = sum;
        }

        // Cumulative mean normalized difference
        float running_sum = 0.0f;
        d[0] = 1.0f;
        int best_tau = -1;
        float threshold = 0.15f;

        for (int tau = 1; tau < half_w; ++tau) {
            running_sum += d[tau];
            d[tau] *= (float)tau / (running_sum + 1e-6f);
            if (d[tau] < threshold && best_tau == -1) {
                best_tau = tau;
            }
        }

        if (best_tau > 0) {
            // Parabolic interpolation
            float s0 = d[best_tau - 1];
            float s1 = d[best_tau];
            float s2 = (best_tau + 1 < half_w) ? d[best_tau + 1] : s1;
            float delta = (s2 - s0) / (2.0f * (2.0f * s1 - s2 - s0) + 1e-6f);
            float interp_tau = (float)best_tau + delta;

            float new_freq = (float)sample_rate / interp_tau;
            if (new_freq >= 65.0f && new_freq <= 1400.0f) {
                detected_freq += 0.25f * (new_freq - detected_freq);
                tracked_period_samples = (float)sample_rate / detected_freq;
            }
        }
    }

public:
    CyberAcousticFeedbacker(double sr) : sample_rate(sr) {
        memset(yin_buf, 0, sizeof(yin_buf));
        yin_idx = 0;
        detected_freq = 220.0f; // Default A3
        tracked_period_samples = (float)sample_rate / 220.0f;

        fast_env = 0.0f;
        slow_env = 0.0f;
        sustain_timer = 0.0f;
        feedback_bloom_gain = 0.0f;

        mono_res.init();
        for (int i = 0; i < 6; ++i) poly_res[i].init();

        lp_l = lp_r = 0.0f;
    }

    void connect_port(uint32_t port, void* data) {
        switch ((PortIndex)port) {
            case PORT_AUDIO_IN_L:  p_in_l = (const float*)data; break;
            case PORT_AUDIO_IN_R:  p_in_r = (const float*)data; break;
            case PORT_AUDIO_OUT_L: p_out_l = (float*)data; break;
            case PORT_AUDIO_OUT_R: p_out_r = (float*)data; break;
            case PORT_BYPASS:      p_bypass = (const float*)data; break;
            case PORT_MODE:        p_mode = (const float*)data; break;
            case PORT_BLOOM:       p_bloom = (const float*)data; break;
            case PORT_SENS:        p_sens = (const float*)data; break;
            case PORT_COUPLING:    p_coupling = (const float*)data; break;
            case PORT_HARMONIC:    p_harmonic = (const float*)data; break;
            case PORT_WARMTH:      p_warmth = (const float*)data; break;
            case PORT_VIBRATO:     p_vibrato = (const float*)data; break;
            case PORT_MIX:         p_mix = (const float*)data; break;
        }
    }

    void run(uint32_t sample_count) {
        bool bypass = (*p_bypass < 0.5f);
        if (bypass) {
            if (p_out_l != p_in_l) memcpy(p_out_l, p_in_l, sample_count * sizeof(float));
            if (p_out_r != p_in_r) memcpy(p_out_r, p_in_r, sample_count * sizeof(float));
            return;
        }

        int mode = (int)std::round(*p_mode); // 0 = Root, 1 = Poly, 2 = Bloom
        float bloom_sec = std::max(0.1f, *p_bloom * 0.01f * 2.5f);
        float sens = *p_sens * 0.01f;
        float coupling = *p_coupling * 0.01f;
        int harmonic_mode = (int)std::round(*p_harmonic); // 0=Unison, 1=5th, 2=Octave, 3=Morph
        float warmth = *p_warmth * 0.01f;
        float vibrato_track = *p_vibrato * 0.01f;
        float mix = *p_mix * 0.01f;

        float bloom_rise = 1.0f - expf(-1.0f / (bloom_sec * (float)sample_rate));
        float duck_decay = 1.0f - expf(-1.0f / (0.05f * (float)sample_rate));
        float damping_coeff = 0.15f + (1.0f - warmth) * 0.7f;

        // Base guitar open string frequencies for poly sustainer (E2, A2, D3, G3, B3, E4)
        float poly_periods[6] = {
            (float)sample_rate / 82.41f,
            (float)sample_rate / 110.0f,
            (float)sample_rate / 146.83f,
            (float)sample_rate / 196.0f,
            (float)sample_rate / 246.94f,
            (float)sample_rate / 329.63f
        };

        for (uint32_t i = 0; i < sample_count; ++i) {
            float in_l = p_in_l[i];
            float in_r = p_in_r ? p_in_r[i] : in_l;
            float in_mono = 0.5f * (in_l + in_r);
            float in_abs = fabsf(in_mono);

            // Feed YIN pitch buffer
            yin_buf[yin_idx] = in_mono;
            if (++yin_idx >= YIN_BUFFER_SIZE) {
                yin_idx = 0;
                detect_pitch();
            }

            // Envelope and Crest Factor Tracking
            fast_env += (in_abs - fast_env) * 0.05f;
            slow_env += (in_abs - slow_env) * 0.0005f;

            // Transient detection (picking attack vs sustained decay)
            bool is_picking = (fast_env > slow_env * 1.8f && fast_env > 0.02f);
            bool is_sustained = (fast_env > 0.005f * (1.0f - sens) && !is_picking);

            if (is_sustained) {
                sustain_timer += (1.0f / (float)sample_rate);
                if (sustain_timer > (0.15f * (1.0f - sens))) {
                    feedback_bloom_gain += (1.0f - feedback_bloom_gain) * bloom_rise;
                }
            } else {
                sustain_timer = 0.0f;
                feedback_bloom_gain += (0.0f - feedback_bloom_gain) * duck_decay;
            }

            // Harmonic Multiplier
            float harm_mult = 1.0f;
            if (harmonic_mode == 1) harm_mult = 1.5f;       // 5th Above (+7st)
            else if (harmonic_mode == 2) harm_mult = 2.0f;  // Octave (+12st)
            else if (harmonic_mode == 3) {
                // Natural dynamic morph: fundamental -> climbs to octave as note holds
                harm_mult = 1.0f + std::min(1.0f, feedback_bloom_gain * 1.2f);
            }

            float target_period = (tracked_period_samples / harm_mult);

            float feedback_sig_l = 0.0f;
            float feedback_sig_r = 0.0f;

            if (mode == 0) {
                // MODE 0: ROOT DOMINANT (Classic Amp Feedback)
                float res_out = mono_res.process(in_mono * 0.8f, target_period, 0.96f * coupling, damping_coeff);
                feedback_sig_l = res_out * feedback_bloom_gain;
                feedback_sig_r = res_out * feedback_bloom_gain;
            } else if (mode == 1) {
                // MODE 1: POLY SUSTAIN (Full 6-String Chord Sustain)
                float poly_sum_l = 0.0f;
                float poly_sum_r = 0.0f;
                for (int s = 0; s < 6; ++s) {
                    float r_s = poly_res[s].process(in_mono * 0.35f, poly_periods[s], 0.94f * coupling, damping_coeff);
                    if (s % 2 == 0) poly_sum_l += r_s;
                    else poly_sum_r += r_s;
                }
                feedback_sig_l = poly_sum_l * feedback_bloom_gain;
                feedback_sig_r = poly_sum_r * feedback_bloom_gain;
            } else {
                // MODE 2: HARMONIC BLOOM (Shimmer Poly Sustainer)
                float bloom_sum = 0.0f;
                for (int s = 0; s < 6; ++s) {
                    float r_s = poly_res[s].process(in_mono * 0.35f, poly_periods[s] * 0.5f, 0.95f * coupling, damping_coeff);
                    bloom_sum += r_s;
                }
                feedback_sig_l = bloom_sum * feedback_bloom_gain * 0.7f;
                feedback_sig_r = bloom_sum * feedback_bloom_gain * 0.7f;
            }

            // Output Lowpass Warmth Filter
            float lp_cutoff = 1200.0f + (1.0f - warmth) * 9000.0f;
            float lp_coeff = 1.0f - expf(-2.0f * (float)M_PI * lp_cutoff / (float)sample_rate);
            lp_l += lp_coeff * (feedback_sig_l - lp_l);
            lp_r += lp_coeff * (feedback_sig_r - lp_r);

            // Master Mix
            p_out_l[i] = in_l * (1.0f - mix * 0.5f) + lp_l * mix * 1.4f;
            if (p_out_r) p_out_r[i] = in_r * (1.0f - mix * 0.5f) + lp_r * mix * 1.4f;
        }
    }
};

static LV2_Handle instantiate(const LV2_Descriptor* descriptor,
                             double rate,
                             const char* path,
                             const LV2_Feature* const* features) {
    return new CyberAcousticFeedbacker(rate);
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    ((CyberAcousticFeedbacker*)instance)->connect_port(port, data);
}

static void activate(LV2_Handle instance) {}

static void run(LV2_Handle instance, uint32_t sample_count) {
    ((CyberAcousticFeedbacker*)instance)->run(sample_count);
}

static void deactivate(LV2_Handle instance) {}

static void cleanup(LV2_Handle instance) {
    delete (CyberAcousticFeedbacker*)instance;
}

static const void* extension_data(const char* uri) {
    return NULL;
}

static const LV2_Descriptor descriptor = {
    PLUGIN_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return (index == 0) ? &descriptor : NULL;
}
