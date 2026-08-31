/*
 * Cyber Acoustic Feedbacker & Polyphonic Sustainer - LV2 Plugin
 * Copyright (c) 2026 Cyber Audio
 *
 * True Musical Harmonic Feedback Engine (FreqOut / DF-2 style):
 *  - Low-pass pre-filtered autocorrelation pitch tracker locks onto the exact guitar note.
 *  - Phase-locked sinusoidal harmonic resonator synthesizes pure musical feedback (Unison, 5th, Octave, 2nd Octave, Morph).
 *  - Dynamic tube saturation and analog acoustic speaker resonance.
 *  - Full vibrato and string bending pitch tracking.
 *  - Zero random noise / zero comb flutter.
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
#define PITCH_BUF_SIZE 2048

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

class CyberAcousticFeedbacker {
private:
    double sample_rate;

    // Pitch Tracking State
    float pitch_buf[PITCH_BUF_SIZE];
    int pitch_idx;
    float prefilter_lp;
    float current_note_freq;
    float latched_note_freq;
    float target_osc_freq;
    float smoothed_osc_freq;
    float pitch_confidence;
    bool has_note_locked;

    // Harmonic Oscillator State
    double osc_phase;
    double osc_phase_poly[3];
    float morph_progress;

    // Envelope and Trigger
    float guitar_env;
    float feedback_gain_env;
    float auto_trigger_timer;

    // Tone and Cabinet Simulation
    float cab_lp1_l, cab_lp1_r;
    float cab_lp2_l, cab_lp2_r;
    float cab_hp_l, cab_hp_r;

    // Port Pointers
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

    // Fast Autocorrelation Pitch Detector
    void detect_guitar_pitch() {
        int half_w = PITCH_BUF_SIZE / 2;
        int min_tau = (int)(sample_rate / 1200.0); // Highest guitar note ~1200 Hz
        int max_tau = (int)(sample_rate / 70.0);   // Lowest guitar note ~70 Hz
        if (max_tau >= half_w) max_tau = half_w - 1;

        float best_corr = 0.0f;
        int best_tau = 0;

        // Energy of zero lag
        float e0 = 0.0f;
        for (int j = 0; j < half_w; ++j) {
            e0 += pitch_buf[j] * pitch_buf[j];
        }

        if (e0 < 0.0001f) {
            pitch_confidence = 0.0f;
            return;
        }

        // Normalized Autocorrelation
        for (int tau = min_tau; tau <= max_tau; ++tau) {
            float sum = 0.0f;
            float e_tau = 0.0f;
            for (int j = 0; j < half_w; ++j) {
                sum += pitch_buf[j] * pitch_buf[j + tau];
                e_tau += pitch_buf[j + tau] * pitch_buf[j + tau];
            }
            float norm = sqrtf(e0 * e_tau + 1e-9f);
            float corr = sum / norm;

            if (corr > best_corr) {
                best_corr = corr;
                best_tau = tau;
            }
        }

        pitch_confidence = best_corr;

        if (best_corr > 0.65f && best_tau > 0) {
            // Parabolic Interpolation for exact sub-sample frequency
            float s0 = 0.0f, s1 = best_corr, s2 = 0.0f;
            if (best_tau > min_tau) {
                float sum = 0.0f;
                for (int j = 0; j < half_w; ++j) sum += pitch_buf[j] * pitch_buf[j + best_tau - 1];
                s0 = sum / e0;
            }
            if (best_tau < max_tau) {
                float sum = 0.0f;
                for (int j = 0; j < half_w; ++j) sum += pitch_buf[j] * pitch_buf[j + best_tau + 1];
                s2 = sum / e0;
            }

            float delta = (s2 - s0) / (2.0f * (2.0f * s1 - s2 - s0) + 1e-6f);
            float interp_tau = (float)best_tau + delta;
            float raw_hz = (float)sample_rate / interp_tau;

            if (raw_hz >= 65.0f && raw_hz <= 1400.0f) {
                current_note_freq += 0.4f * (raw_hz - current_note_freq);
                has_note_locked = true;
            }
        }
    }

public:
    CyberAcousticFeedbacker(double sr) : sample_rate(sr) {
        memset(pitch_buf, 0, sizeof(pitch_buf));
        pitch_idx = 0;
        prefilter_lp = 0.0f;
        current_note_freq = 220.0f; // Default A3 (220 Hz)
        latched_note_freq = 220.0f;
        target_osc_freq = 220.0f;
        smoothed_osc_freq = 220.0f;
        pitch_confidence = 0.0f;
        has_note_locked = false;

        osc_phase = 0.0;
        osc_phase_poly[0] = osc_phase_poly[1] = osc_phase_poly[2] = 0.0;
        morph_progress = 0.0f;

        guitar_env = 0.0f;
        feedback_gain_env = 0.0f;
        auto_trigger_timer = 0.0f;

        cab_lp1_l = cab_lp1_r = 0.0f;
        cab_lp2_l = cab_lp2_r = 0.0f;
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
        int harmonic_mode = (int)std::round(p_harmonic ? *p_harmonic : 1.0f); // 0=Unison, 1=5th, 2=Octave, 3=2nd Octave, 4=Morph
        float warmth_knob = (p_warmth ? *p_warmth : 50.0f) * 0.01f;
        float vibrato_knob = (p_vibrato ? *p_vibrato : 75.0f) * 0.01f;
        float mix_knob = (p_mix ? *p_mix : 50.0f) * 0.01f;

        // Dynamic bloom time (0.05s snappy to 1.8s singing swell)
        float bloom_sec = 0.05f + bloom_knob * 1.5f;
        float attack_rate = 1.0f - expf(-1.0f / (bloom_sec * (float)sample_rate));
        float release_rate = 1.0f - expf(-1.0f / (0.12f * (float)sample_rate));

        // Pitch smoothing coefficient
        float pitch_smooth_rate = 1.0f - expf(-1.0f / (0.02f * (float)sample_rate));

        // Prefilter for pitch detector (gentle 800Hz lowpass to isolate string fundamental)
        float prefilter_coeff = 1.0f - expf(-2.0f * (float)M_PI * 700.0f / (float)sample_rate);

        // Cabinet Tone Filters (simulates 12" speaker body resonance)
        float hp_coeff = 1.0f - expf(-2.0f * (float)M_PI * 100.0f / (float)sample_rate);
        float lp_cutoff = 1800.0f + (1.0f - warmth_knob) * 4500.0f;
        float lp_coeff = 1.0f - expf(-2.0f * (float)M_PI * lp_cutoff / (float)sample_rate);

        for (uint32_t i = 0; i < sample_count; ++i) {
            float in_l = p_in_l[i];
            float in_r = (p_in_r ? p_in_r[i] : in_l);
            float in_mono = 0.5f * (in_l + in_r);
            float in_abs = fabsf(in_mono);

            // Envelope tracking
            guitar_env += (in_abs - guitar_env) * 0.02f;

            // Lowpass prefilter audio before pitch detection
            prefilter_lp += prefilter_coeff * (in_mono - prefilter_lp);

            // Store in pitch buffer
            pitch_buf[pitch_idx] = prefilter_lp;
            if (++pitch_idx >= PITCH_BUF_SIZE) {
                pitch_idx = 0;
                detect_guitar_pitch();
            }

            // Determine if feedback should trigger
            bool is_triggered = false;
            if (manual_trigger) {
                is_triggered = true;
                // If trigger just engaged, lock the active note
                if (feedback_gain_env < 0.05f && has_note_locked) {
                    latched_note_freq = current_note_freq;
                }
            } else {
                // Auto mode: Note held longer than 200ms with clear pitch confidence
                if (guitar_env > 0.012f && pitch_confidence > 0.65f) {
                    auto_trigger_timer += (1.0f / (float)sample_rate);
                    if (auto_trigger_timer > 0.22f) {
                        is_triggered = true;
                        if (feedback_gain_env < 0.05f && has_note_locked) {
                            latched_note_freq = current_note_freq;
                        }
                    }
                } else {
                    auto_trigger_timer = 0.0f;
                }
            }

            // Envelope ramp
            if (is_triggered) {
                feedback_gain_env += (1.0f - feedback_gain_env) * attack_rate;
                morph_progress += (1.0f - morph_progress) * (attack_rate * 0.5f);
            } else {
                feedback_gain_env += (0.0f - feedback_gain_env) * release_rate;
                morph_progress = 0.0f;
            }

            // Calculate precise note frequency with vibrato & bending tracking
            float active_base_freq = latched_note_freq * (1.0f - vibrato_knob) + current_note_freq * vibrato_knob;
            if (active_base_freq < 65.0f) active_base_freq = 65.0f;
            if (active_base_freq > 1400.0f) active_base_freq = 1400.0f;

            // Compute harmonic multiplier
            float harm_mult = 1.0f;
            if (harmonic_mode == 0) {
                harm_mult = 1.0f;         // Unison (Fundamental E-Bow sustain)
            } else if (harmonic_mode == 1) {
                harm_mult = 1.498307f;    // 5th Above (Jimi Hendrix singing feedback)
            } else if (harmonic_mode == 2) {
                harm_mult = 2.0f;         // Octave Above
            } else if (harmonic_mode == 3) {
                harm_mult = 4.0f;         // 2nd Octave
            } else if (harmonic_mode == 4) {
                // Dynamic Morph: Starts at fundamental and blooms up into singing 5th
                harm_mult = 1.0f + morph_progress * 0.498307f;
            }

            target_osc_freq = active_base_freq * harm_mult;
            smoothed_osc_freq += (target_osc_freq - smoothed_osc_freq) * pitch_smooth_rate;

            // Generate Pure Singing Musical Feedback Oscillator
            double phase_inc = (2.0 * M_PI * smoothed_osc_freq) / sample_rate;
            osc_phase += phase_inc;
            if (osc_phase >= 2.0 * M_PI) osc_phase -= 2.0 * M_PI;

            // Pure fundamental sine wave
            float pure_osc = (float)sin(osc_phase);

            // Phase injection: inject pickup signal to lock string vibrato & micro-dynamics
            float driven_osc = pure_osc + (in_mono * 0.35f);

            // Tube saturation / Asymmetric soft clipping (adds authentic guitar amp harmonics)
            float saturated_feedback;
            if (driven_osc > 0.0f) {
                saturated_feedback = tanhf(driven_osc * (1.2f + gain_knob * 0.8f));
            } else {
                saturated_feedback = tanhf(driven_osc * (1.0f + gain_knob * 0.6f));
            }

            // Multi-Mode Processing
            float fb_sig_l = 0.0f;
            float fb_sig_r = 0.0f;

            if (mode == 0) {
                // Mode 0: Root Dominant Single-Note Singing Feedback
                float out_sample = saturated_feedback * feedback_gain_env;
                fb_sig_l = out_sample;
                fb_sig_r = out_sample;
            } else if (mode == 1) {
                // Mode 1: Polyphonic Sustainer (Fundamental + 5th overtone layer)
                double p_inc2 = (2.0 * M_PI * smoothed_osc_freq * 1.5) / sample_rate;
                osc_phase_poly[0] += p_inc2;
                if (osc_phase_poly[0] >= 2.0 * M_PI) osc_phase_poly[0] -= 2.0 * M_PI;
                float poly_5th = (float)sin(osc_phase_poly[0]) * 0.4f;

                float poly_mix = (saturated_feedback * 0.7f + poly_5th) * feedback_gain_env;
                fb_sig_l = poly_mix;
                fb_sig_r = poly_mix;
            } else {
                // Mode 2: Shimmering Harmonic Bloom (Fundamental + Octave shimmer)
                double p_inc_oct = (2.0 * M_PI * smoothed_osc_freq * 2.0) / sample_rate;
                osc_phase_poly[1] += p_inc_oct;
                if (osc_phase_poly[1] >= 2.0 * M_PI) osc_phase_poly[1] -= 2.0 * M_PI;
                float oct_shimmer = (float)sin(osc_phase_poly[1]) * 0.5f;

                float bloom_mix = (saturated_feedback * 0.6f + oct_shimmer) * feedback_gain_env;
                fb_sig_l = bloom_mix;
                fb_sig_r = bloom_mix;
            }

            // Cabinet Emulation Filter (Highpass 100Hz + Dual Lowpass)
            cab_hp_l += hp_coeff * (fb_sig_l - cab_hp_l);
            cab_hp_r += hp_coeff * (fb_sig_r - cab_hp_r);
            float hp_l = fb_sig_l - cab_hp_l;
            float hp_r = fb_sig_r - cab_hp_r;

            cab_lp1_l += lp_coeff * (hp_l - cab_lp1_l);
            cab_lp1_r += lp_coeff * (hp_r - cab_lp1_r);
            cab_lp2_l += lp_coeff * (cab_lp1_l - cab_lp2_l);
            cab_lp2_r += lp_coeff * (cab_lp1_r - cab_lp2_r);

            // Final Output Mix (Clean dry guitar + singing musical feedback tone)
            float final_wet_l = cab_lp2_l * (0.8f + gain_knob * 0.5f);
            float final_wet_r = cab_lp2_r * (0.8f + gain_knob * 0.5f);

            p_out_l[i] = in_l + final_wet_l * mix_knob * 1.5f;
            if (p_out_r) {
                p_out_r[i] = in_r + final_wet_r * mix_knob * 1.5f;
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
