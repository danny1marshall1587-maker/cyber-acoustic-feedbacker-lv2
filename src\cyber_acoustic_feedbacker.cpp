/*
 * Cyber Acoustic Feedbacker & Polyphonic Sustainer - LV2 Plugin
 * Copyright (c) 2026 Cyber Audio
 *
 * Core DSP Architecture:
 *  1. Low-latency Pitch & Vibrato Tracker (YIN + Autocorrelation hybrid).
 *  2. Dual Trigger Engine:
 *     - Momentary/Latching MIDI Footswitch Trigger (instant latch & singing sustained oscillation).
 *     - Auto-Sustain Envelope Detector (picks up held notes seamlessly).
 *  3. Physical String & Acoustic Room Feedback Resonator:
 *     - Loop gain > 1.0 with dynamic hyperbolic tangent + soft cubic saturation.
 *     - Harmonic overtone selection (Unison, 5th, Octave, 2nd Octave, Dynamic Morph).
 *  4. Analog Cabinet Warmth & Dynamic String Coupling.
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
    PORT_TRIGGER       = 5,
    PORT_MODE          = 6,
    PORT_BLOOM         = 7,
    PORT_GAIN          = 8,
    PORT_HARMONIC      = 9,
    PORT_WARMTH        = 10,
    PORT_VIBRATO       = 11,
    PORT_MIX           = 12
};

// Physical Acoustic Feedback String Resonator
struct AcousticFeedbackLoop {
    float buffer[4096];
    int write_pos;
    float damping_lp;
    float dc_block_in;
    float dc_block_out;

    void init() {
        memset(buffer, 0, sizeof(buffer));
        write_pos = 0;
        damping_lp = 0.0f;
        dc_block_in = 0.0f;
        dc_block_out = 0.0f;
    }

    inline float process(float in_pickup, float target_delay_samples, float loop_gain, float damping_coeff, float warmth_factor) {
        // Safe bounds for delay line (guitar range 50 Hz to 2500 Hz)
        float d_s = std::max(8.0f, std::min(4090.0f, target_delay_samples));
        float r_pos = (float)write_pos - d_s;
        while (r_pos < 0.0f) r_pos += 4096.0f;
        while (r_pos >= 4096.0f) r_pos -= 4096.0f;

        int i0 = (int)r_pos;
        int i1 = (i0 + 1) % 4096;
        float frac = r_pos - (float)i0;
        float delayed_signal = buffer[i0] + frac * (buffer[i1] - buffer[i0]);

        // DC Blocker (removes sub-bass offset build up in feedback loop)
        dc_block_out = delayed_signal - dc_block_in + 0.995f * dc_block_out;
        dc_block_in = delayed_signal;

        // Damping lowpass inside feedback path (emulates air/wood absorption)
        damping_lp += damping_coeff * (dc_block_out - damping_lp);

        // Pickup coupling: Inject fresh guitar pickup audio to lock phase with guitar vibrato/bends
        float loop_input = (in_pickup * 1.2f) + (damping_lp * loop_gain);

        // Soft asymmetric saturation (simulates speaker/pickup magnet saturation when feeding back)
        float saturated;
        if (loop_input > 0.0f) {
            saturated = tanhf(loop_input * 1.35f);
        } else {
            saturated = tanhf(loop_input * 1.15f);
        }

        // Store back in delay line
        buffer[write_pos] = saturated;
        if (++write_pos >= 4096) write_pos = 0;

        return saturated;
    }
};

class CyberAcousticFeedbacker {
private:
    double sample_rate;

    // Pitch Tracker
    float yin_buf[YIN_BUFFER_SIZE];
    int yin_idx;
    float current_pitch_freq;
    float latched_pitch_freq;
    float tracked_delay_samples;
    bool has_valid_pitch;

    // Trigger & Envelope State
    float envelope_follower;
    float trigger_env;
    float auto_sustain_timer;
    float dynamic_morph_progress;

    // Resonator Engines
    AcousticFeedbackLoop mono_resonator;
    AcousticFeedbackLoop poly_resonators[6];

    // Master Tone / Cabinet Filters
    float cab_lp_l, cab_lp_r;
    float cab_hp_l, cab_hp_r;

    // LV2 Port Pointers
    const float* p_in_l;
    const float* p_in_r;
    float* p_out_l;
    float* p_out_r;
    const float* p_bypass;
    const float* p_trigger;
    const float* p_mode;
    const float* p_bloom;
    const float* p_gain;
    const float* p_harmonic;
    const float* p_warmth;
    const float* p_vibrato;
    const float* p_mix;

    // Pitch Detection Engine
    void run_pitch_detection() {
        int half_w = YIN_BUFFER_SIZE / 2;
        float d[half_w];
        d[0] = 1.0f;

        // Autocorrelation / difference function
        for (int tau = 1; tau < half_w; ++tau) {
            float sum = 0.0f;
            for (int j = 0; j < half_w; ++j) {
                float diff = yin_buf[j] - yin_buf[j + tau];
                sum += diff * diff;
            }
            d[tau] = sum;
        }

        // Cumulative mean normalization
        float running_sum = 0.0f;
        d[0] = 1.0f;
        int best_tau = -1;
        float threshold = 0.18f;

        for (int tau = 1; tau < half_w; ++tau) {
            running_sum += d[tau];
            d[tau] *= (float)tau / (running_sum + 1e-6f);
            if (d[tau] < threshold && best_tau == -1) {
                best_tau = tau;
            }
        }

        if (best_tau > 0) {
            // Parabolic interpolation for fine sub-sample precision
            float s0 = d[best_tau - 1];
            float s1 = d[best_tau];
            float s2 = (best_tau + 1 < half_w) ? d[best_tau + 1] : s1;
            float delta = (s2 - s0) / (2.0f * (2.0f * s1 - s2 - s0) + 1e-6f);
            float interp_tau = (float)best_tau + delta;

            float detected_hz = (float)sample_rate / interp_tau;
            if (detected_hz >= 65.0f && detected_hz <= 1400.0f) {
                current_pitch_freq += 0.35f * (detected_hz - current_pitch_freq);
                has_valid_pitch = true;
            }
        }
    }

public:
    CyberAcousticFeedbacker(double sr) : sample_rate(sr) {
        memset(yin_buf, 0, sizeof(yin_buf));
        yin_idx = 0;
        current_pitch_freq = 220.0f; // A3 default
        latched_pitch_freq = 220.0f;
        tracked_delay_samples = (float)sample_rate / 220.0f;
        has_valid_pitch = false;

        envelope_follower = 0.0f;
        trigger_env = 0.0f;
        auto_sustain_timer = 0.0f;
        dynamic_morph_progress = 0.0f;

        mono_resonator.init();
        for (int i = 0; i < 6; ++i) {
            poly_resonators[i].init();
        }

        cab_lp_l = cab_lp_r = 0.0f;
        cab_hp_l = cab_hp_r = 0.0f;
    }

    void connect_port(uint32_t port, void* data) {
        switch ((PortIndex)port) {
            case PORT_AUDIO_IN_L:  p_in_l = (const float*)data; break;
            case PORT_AUDIO_IN_R:  p_in_r = (const float*)data; break;
            case PORT_AUDIO_OUT_L: p_out_l = (float*)data; break;
            case PORT_AUDIO_OUT_R: p_out_r = (float*)data; break;
            case PORT_BYPASS:      p_bypass = (const float*)data; break;
            case PORT_TRIGGER:     p_trigger = (const float*)data; break;
            case PORT_MODE:        p_mode = (const float*)data; break;
            case PORT_BLOOM:       p_bloom = (const float*)data; break;
            case PORT_GAIN:        p_gain = (const float*)data; break;
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
            if (p_out_r && p_in_r && p_out_r != p_in_r) memcpy(p_out_r, p_in_r, sample_count * sizeof(float));
            return;
        }

        bool manual_trigger = (p_trigger && *p_trigger > 0.5f);
        int mode = (int)std::round(p_mode ? *p_mode : 0.0f); // 0=Root, 1=Poly, 2=Bloom
        float bloom_knob = (p_bloom ? *p_bloom : 40.0f) * 0.01f;
        float gain_knob = (p_gain ? *p_gain : 75.0f) * 0.01f;
        int harmonic_mode = (int)std::round(p_harmonic ? *p_harmonic : 1.0f); // 0=Unison, 1=5th, 2=Oct, 3=2ndOct, 4=Morph
        float warmth_knob = (p_warmth ? *p_warmth : 50.0f) * 0.01f;
        float vibrato_knob = (p_vibrato ? *p_vibrato : 75.0f) * 0.01f;
        float mix_knob = (p_mix ? *p_mix : 50.0f) * 0.01f;

        // Attack & Release Coeffs for Bloom
        float bloom_sec = 0.08f + bloom_knob * 1.5f;
        float attack_coeff = 1.0f - expf(-1.0f / (bloom_sec * (float)sample_rate));
        float release_coeff = 1.0f - expf(-1.0f / (0.12f * (float)sample_rate));

        // Loop resonance gain: 0.96 at min to 1.15 at max (gives rich singing self-oscillation)
        float feedback_loop_gain = 0.96f + gain_knob * 0.18f;
        float damping_coeff = 0.12f + (1.0f - warmth_knob) * 0.65f;

        // Open string frequencies for polyphonic acoustic sustain mode (E2, A2, D3, G3, B3, E4)
        const float poly_freqs[6] = { 82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f };
        float poly_delays[6];
        for (int s = 0; s < 6; ++s) {
            poly_delays[s] = (float)sample_rate / poly_freqs[s];
        }

        for (uint32_t i = 0; i < sample_count; ++i) {
            float in_l = p_in_l[i];
            float in_r = (p_in_r ? p_in_r[i] : in_l);
            float in_mono = 0.5f * (in_l + in_r);
            float in_abs = fabsf(in_mono);

            // Buffer input for pitch detection
            yin_buf[yin_idx] = in_mono;
            if (++yin_idx >= YIN_BUFFER_SIZE) {
                yin_idx = 0;
                run_pitch_detection();
            }

            // Envelope tracking
            envelope_follower += (in_abs - envelope_follower) * 0.015f;

            // Trigger detection (Manual MIDI Footswitch OR Auto-Sustain when note is held)
            bool is_active_trigger = false;
            if (manual_trigger) {
                is_active_trigger = true;
                // If just triggered, latch the currently ringing pitch
                if (trigger_env < 0.05f && has_valid_pitch) {
                    latched_pitch_freq = current_pitch_freq;
                }
            } else {
                // Auto-Sustain mode: Detect when a note is held longer than 150ms
                if (envelope_follower > 0.015f) {
                    auto_sustain_timer += (1.0f / (float)sample_rate);
                    if (auto_sustain_timer > 0.18f) {
                        is_active_trigger = true;
                        if (trigger_env < 0.05f && has_valid_pitch) {
                            latched_pitch_freq = current_pitch_freq;
                        }
                    }
                } else {
                    auto_sustain_timer = 0.0f;
                }
            }

            // Smooth feedback engagement envelope
            if (is_active_trigger) {
                trigger_env += (1.0f - trigger_env) * attack_coeff;
                dynamic_morph_progress += (1.0f - dynamic_morph_progress) * (attack_coeff * 0.4f);
            } else {
                trigger_env += (0.0f - trigger_env) * release_coeff;
                dynamic_morph_progress = 0.0f;
            }

            // Select pitch target (blend live tracked vibrato with latched note)
            float effective_freq = latched_pitch_freq * (1.0f - vibrato_knob) + current_pitch_freq * vibrato_knob;
            if (effective_freq < 60.0f) effective_freq = 60.0f;

            // Harmonic multiplier calculation
            float harm_mult = 1.0f;
            if (harmonic_mode == 0) {
                harm_mult = 1.0f; // Unison Fundamental
            } else if (harmonic_mode == 1) {
                harm_mult = 1.498307f; // 5th Harmonic (+7 semitones)
            } else if (harmonic_mode == 2) {
                harm_mult = 2.0f; // Octave (+12 semitones)
            } else if (harmonic_mode == 3) {
                harm_mult = 4.0f; // 2nd Octave (+24 semitones)
            } else if (harmonic_mode == 4) {
                // Dynamic Morph: Starts on fundamental and blooms into screaming 5th/octave
                harm_mult = 1.0f + dynamic_morph_progress * 0.5f; // Unison to 5th
            }

            float target_freq = effective_freq * harm_mult;
            float root_delay_samples = (float)sample_rate / target_freq;

            float fb_out_l = 0.0f;
            float fb_out_r = 0.0f;

            if (mode == 0) {
                // MODE 0: ROOT DOMINANT FEEDBACK (Singing Lead / Soloist Feedbacker)
                float res_out = mono_resonator.process(in_mono, root_delay_samples, feedback_loop_gain, damping_coeff, warmth_knob);
                fb_out_l = res_out * trigger_env;
                fb_out_r = res_out * trigger_env;
            } else if (mode == 1) {
                // MODE 1: POLY SUSTAINER (Multi-String Resonator Bank)
                float poly_sum_l = 0.0f;
                float poly_sum_r = 0.0f;
                for (int s = 0; s < 6; ++s) {
                    float r_out = poly_resonators[s].process(in_mono * 0.4f, poly_delays[s] / harm_mult, feedback_loop_gain * 0.98f, damping_coeff, warmth_knob);
                    if (s % 2 == 0) poly_sum_l += r_out;
                    else poly_sum_r += r_out;
                }
                fb_out_l = poly_sum_l * trigger_env;
                fb_out_r = poly_sum_r * trigger_env;
            } else {
                // MODE 2: HARMONIC BLOOM (Shimmer Poly Sustainer with Upper Octaves)
                float bloom_sum = 0.0f;
                for (int s = 0; s < 6; ++s) {
                    float r_out = poly_resonators[s].process(in_mono * 0.35f, (poly_delays[s] * 0.5f) / harm_mult, feedback_loop_gain * 0.98f, damping_coeff * 0.8f, warmth_knob);
                    bloom_sum += r_out;
                }
                fb_out_l = bloom_sum * trigger_env * 0.75f;
                fb_out_r = bloom_sum * trigger_env * 0.75f;
            }

            // Cabinet / Room Warmth Filter (gentle 120Hz highpass to remove rumble + tone lowpass)
            float hp_coeff = 1.0f - expf(-2.0f * (float)M_PI * 120.0f / (float)sample_rate);
            cab_hp_l += hp_coeff * (fb_out_l - cab_hp_l);
            cab_hp_r += hp_coeff * (fb_out_r - cab_hp_r);
            float hp_filtered_l = fb_out_l - cab_hp_l;
            float hp_filtered_r = fb_out_r - cab_hp_r;

            float lp_cutoff = 1500.0f + (1.0f - warmth_knob) * 7500.0f;
            float lp_coeff = 1.0f - expf(-2.0f * (float)M_PI * lp_cutoff / (float)sample_rate);
            cab_lp_l += lp_coeff * (hp_filtered_l - cab_lp_l);
            cab_lp_r += lp_coeff * (hp_filtered_r - cab_lp_r);

            // Output Mix
            p_out_l[i] = in_l + cab_lp_l * mix_knob * 1.6f;
            if (p_out_r) {
                p_out_r[i] = in_r + cab_lp_r * mix_knob * 1.6f;
            }
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

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
  #define LV2_EXPORT __declspec(dllexport)
#else
  #define LV2_EXPORT __attribute__((visibility("default")))
#endif

LV2_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return (index == 0) ? &descriptor : NULL;
}

#ifdef __cplusplus
}
#endif
