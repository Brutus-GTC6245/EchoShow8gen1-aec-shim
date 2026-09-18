// Speex-only acoustic echo cancellation shim for the Echo Show (crown) audio HAL.
//
// LD_PRELOADed into android.hardware.audio.service. Interposes tinyalsa pcm_open/
// pcm_read/pcm_close; when the blob opens the FPGA mic stream (6ch S24_3LE 16kHz,
// pcmC0D22c) it runs SpeexDSP's linear echo canceller on mic channel 0 using the
// average of the DAC-loopback channels 4 and 5 as the far end, and writes the
// cleaned samples back before the blob sees them. Linear-only (no nonlinear
// suppressor by default) so the talker is preserved during double-talk — ideal for
// wake-word / barge-in. Self-contained: WebRTC-free, links only the bundled
// speexdsp + NDK libc/liblog. Modeled on jxlarrea's amznaec_shim.cpp (speex path).
//
// Tunables (persist.vendor.amznaec.*, read at PCM open):
//   enable(1) gain_db(20) hpf(1) log(0) spx_filter_ms(64) spx_stereo(1)
//   spx_headroom_db(12) spx_echo_suppress(0=linear only) spx_echo_suppress_active(0)
//   spx_denoise(0) spx_noise_suppress(-15)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

#include <android/log.h>
#include <sys/system_properties.h>

#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"

#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, "amznaec", __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, "amznaec", __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, "amznaec", __VA_ARGS__)

// Minimal tinyalsa ABI (matches the device's libtinyalsa, Android 11).
enum pcm_format { PCM_FORMAT_S16_LE = 0, PCM_FORMAT_S32_LE, PCM_FORMAT_S8, PCM_FORMAT_S24_LE, PCM_FORMAT_S24_3LE };
struct pcm_config {
    unsigned int channels, rate, period_size, period_count;
    enum pcm_format format;
    unsigned int start_threshold, stop_threshold, silence_threshold, silence_size;
};
#define PCM_IN 0x10000000

namespace {

constexpr int kRate = 16000;
constexpr unsigned kChannels = 6;
constexpr int kBlock = 160;                 // 10 ms
constexpr unsigned kFrameBytes = kChannels * 3;   // S24_3LE, 6ch
constexpr unsigned kBlockBytes = kBlock * kFrameBytes;
constexpr int kRefFirst = 4;                // loopback channels 4,5

typedef struct pcm* (*pcm_open_t)(unsigned, unsigned, unsigned, struct pcm_config*);
typedef int (*pcm_read_t)(struct pcm*, void*, unsigned);
typedef int (*pcm_close_t)(struct pcm*);
pcm_open_t real_open;
pcm_read_t real_read;
pcm_close_t real_close;

bool prop_bool(const char* k, bool d) {
    char v[PROP_VALUE_MAX];
    if (__system_property_get(k, v) <= 0) return d;
    return v[0] == '1' || !strcmp(v, "true");
}
int prop_int(const char* k, int d) {
    char v[PROP_VALUE_MAX];
    if (__system_property_get(k, v) <= 0) return d;
    return atoi(v);
}

struct Settings {
    bool enable, hpf, log;
    int gain_db, filter_ms, echo_suppress, echo_suppress_active, denoise, noise_suppress, stereo, headroom_db;
};
Settings read_settings() {
    Settings s;
    s.enable = prop_bool("persist.vendor.amznaec.enable", true);
    s.hpf = prop_bool("persist.vendor.amznaec.hpf", true);
    s.log = prop_bool("persist.vendor.amznaec.log", false);
    s.gain_db = prop_int("persist.vendor.amznaec.gain_db", 20);
    s.filter_ms = prop_int("persist.vendor.amznaec.spx_filter_ms", 64);
    s.echo_suppress = prop_int("persist.vendor.amznaec.spx_echo_suppress", 0);
    s.echo_suppress_active = prop_int("persist.vendor.amznaec.spx_echo_suppress_active", 0);
    s.denoise = prop_int("persist.vendor.amznaec.spx_denoise", 0);
    s.noise_suppress = prop_int("persist.vendor.amznaec.spx_noise_suppress", -15);
    s.stereo = prop_int("persist.vendor.amznaec.spx_stereo", 1);
    s.headroom_db = prop_int("persist.vendor.amznaec.spx_headroom_db", 12);
    return s;
}

struct State {
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    struct pcm* pcm = nullptr;
    Settings s{};
    SpeexEchoState* spx = nullptr;
    SpeexPreprocessState* pp = nullptr;
    float hpf_x1 = 0, hpf_y1 = 0;
    std::vector<uint8_t> in, out;
    float near0[kBlock], refl[kBlock], refr[kBlock], ref[kBlock], outbuf[kBlock];
    double e_ref = 0, e_in = 0, e_out = 0;
    unsigned blocks = 0;
};
State g;

inline int32_t s24(const uint8_t* p) { int32_t v = p[0] | (p[1] << 8) | (p[2] << 16); if (v & 0x800000) v -= 0x1000000; return v; }
inline float s24f(const uint8_t* p) { return (float)s24(p) / 8388608.f; }
inline void put24(uint8_t* p, int32_t v) { if (v > 0x7fffff) v = 0x7fffff; if (v < -0x800000) v = -0x800000; p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; }
inline void putf(uint8_t* p, float v) { if (v > 0.9999999f) v = 0.9999999f; if (v < -1.f) v = -1.f; put24(p, (int32_t)(v * 8388608.f)); }

__attribute__((constructor)) static void on_load() {
    ALOGI("libamznaec_shim loaded into pid %d", (int)getpid());
}

void resolve() {
    if (real_open) return;
    void* h = dlopen("libtinyalsa.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("libtinyalsa.so", RTLD_NOW);
    if (h) {
        real_open = (pcm_open_t)dlsym(h, "pcm_open");
        real_read = (pcm_read_t)dlsym(h, "pcm_read");
        real_close = (pcm_close_t)dlsym(h, "pcm_close");
    }
    if (!real_open) real_open = (pcm_open_t)dlsym(RTLD_NEXT, "pcm_open");
    if (!real_read) real_read = (pcm_read_t)dlsym(RTLD_NEXT, "pcm_read");
    if (!real_close) real_close = (pcm_close_t)dlsym(RTLD_NEXT, "pcm_close");
    ALOGI("resolve: tinyalsa handle=%p open=%p read=%p close=%p", h, real_open, real_read, real_close);
    if (!real_open || !real_read || !real_close)
        ALOGE("failed to resolve tinyalsa symbols; audio will pass through");
}

void teardown_l() {
    if (g.pp) { speex_preprocess_state_destroy(g.pp); g.pp = nullptr; }
    if (g.spx) { speex_echo_state_destroy(g.spx); g.spx = nullptr; }
    g.pcm = nullptr;
    g.in.clear(); g.out.clear();
    g.hpf_x1 = g.hpf_y1 = 0;
}

bool make_speex(const Settings& s) {
    int taps = s.filter_ms * kRate / 1000;
    if (taps < kBlock) taps = kBlock;
    g.spx = s.stereo ? speex_echo_state_init_mc(kBlock, taps, 1, 2) : speex_echo_state_init(kBlock, taps);
    if (!g.spx) return false;
    int rate = kRate;
    speex_echo_ctl(g.spx, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    if (s.echo_suppress != 0 || s.denoise) {
        g.pp = speex_preprocess_state_init(kBlock, kRate);
        if (!g.pp) return false;
        int on = s.denoise ? 1 : 0, off = 0;
        speex_preprocess_ctl(g.pp, SPEEX_PREPROCESS_SET_DENOISE, &on);
        speex_preprocess_ctl(g.pp, SPEEX_PREPROCESS_SET_AGC, &off);
        speex_preprocess_ctl(g.pp, SPEEX_PREPROCESS_SET_DEREVERB, &off);
        int ns = s.noise_suppress;
        speex_preprocess_ctl(g.pp, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &ns);
        if (s.echo_suppress != 0) {
            speex_preprocess_ctl(g.pp, SPEEX_PREPROCESS_SET_ECHO_STATE, g.spx);
            int es = s.echo_suppress, esa = s.echo_suppress_active;
            speex_preprocess_ctl(g.pp, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &es);
            speex_preprocess_ctl(g.pp, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &esa);
        }
    }
    return true;
}

void run_speex(const float* src, float* dst) {
    int16_t near16[kBlock], ref16[kBlock * 2], out16[kBlock];
    const float a = 0.969f;  // ~80 Hz first-order high pass
    const float sc = 32768.f * powf(10.f, -(float)g.s.headroom_db / 20.f);
    for (int i = 0; i < kBlock; i++) {
        float x = src[i];
        float y = a * (g.hpf_y1 + x - g.hpf_x1);
        g.hpf_x1 = x; g.hpf_y1 = y;
        float v = g.s.hpf ? y : x;
        near16[i] = (int16_t)fmaxf(-32768.f, fminf(32767.f, v * sc));
        if (g.s.stereo) {
            ref16[2 * i] = (int16_t)fmaxf(-32768.f, fminf(32767.f, g.refl[i] * sc));
            ref16[2 * i + 1] = (int16_t)fmaxf(-32768.f, fminf(32767.f, g.refr[i] * sc));
        } else {
            ref16[i] = (int16_t)fmaxf(-32768.f, fminf(32767.f, g.ref[i] * sc));
        }
    }
    speex_echo_cancellation(g.spx, near16, ref16, out16);
    if (g.pp) speex_preprocess_run(g.pp, out16);
    for (int i = 0; i < kBlock; i++) dst[i] = out16[i] / sc;
}

void process_block_l(uint8_t* frames) {
    double e_ref = 0, e_in = 0, e_out = 0;
    for (int f = 0; f < kBlock; f++) {
        const uint8_t* fr = frames + f * kFrameBytes;
        g.near0[f] = s24f(fr + 0 * 3);
        g.refl[f] = s24f(fr + kRefFirst * 3);
        g.refr[f] = s24f(fr + (kRefFirst + 1) * 3);
        g.ref[f] = (g.refl[f] + g.refr[f]) * 0.5f;
        e_ref += (double)g.ref[f] * g.ref[f];
        e_in += (double)g.near0[f] * g.near0[f];
    }
    run_speex(g.near0, g.outbuf);
    const float gain = powf(10.f, (float)g.s.gain_db / 20.f);
    for (int f = 0; f < kBlock; f++) {
        uint8_t* fr = frames + f * kFrameBytes;
        putf(fr + 0 * 3, g.outbuf[f] * gain);   // write cleaned ch0 (the channel the blob keeps)
        e_out += (double)g.outbuf[f] * g.outbuf[f];
    }
    g.e_ref += e_ref; g.e_in += e_in; g.e_out += e_out;
    if (++g.blocks % 500 == 0 && g.s.log) {
        double n = 500.0 * kBlock;
        auto dbf = [n](double e) { return e > 0 ? 10.0 * log10(e / n) : -120.0; };
        ALOGI("5s: ref %.1f dBFS, mic in %.1f dBFS, out %.1f dBFS (speex linear)", dbf(g.e_ref), dbf(g.e_in), dbf(g.e_out));
        g.e_ref = g.e_in = g.e_out = 0;
    }
}

bool is_mic_pcm(unsigned device, unsigned flags, const struct pcm_config* c) {
    if (!(flags & PCM_IN) || !c) return false;
    return c->channels == kChannels && c->rate == (unsigned)kRate && c->format == PCM_FORMAT_S24_3LE && device < 64;
}

}  // namespace

extern "C" struct pcm* pcm_open(unsigned int card, unsigned int device, unsigned int flags, struct pcm_config* config) {
    resolve();
    struct pcm* pcm = real_open(card, device, flags, config);
    if (!pcm || !is_mic_pcm(device, flags, config)) return pcm;
    pthread_mutex_lock(&g.lock);
    teardown_l();
    g.s = read_settings();
    if (!g.s.enable) {
        ALOGI("mic PCM %u:%u opened, processing disabled by property", card, device);
        pthread_mutex_unlock(&g.lock);
        return pcm;
    }
    if (!make_speex(g.s)) {
        ALOGE("speex init failed; passing audio through");
        teardown_l();
        pthread_mutex_unlock(&g.lock);
        return pcm;
    }
    g.pcm = pcm;
    g.in.reserve(4 * kBlockBytes);
    g.out.assign(kBlockBytes, 0);  // one block of priming silence
    ALOGI("mic PCM %u:%u opened: %u ch %u Hz period %u; SPEEX linear filter=%dms suppress=%d/%d "
          "denoise=%d/%ddB stereo=%d hpf=%d gain=%ddB",
          card, device, config->channels, config->rate, config->period_size, g.s.filter_ms,
          g.s.echo_suppress, g.s.echo_suppress_active, g.s.denoise, g.s.noise_suppress, g.s.stereo,
          g.s.hpf, g.s.gain_db);
    pthread_mutex_unlock(&g.lock);
    return pcm;
}

extern "C" int pcm_read(struct pcm* pcm, void* data, unsigned int count) {
    resolve();
    int rc = real_read(pcm, data, count);
    if (rc != 0 || pcm != g.pcm || count == 0 || (count % kFrameBytes) != 0) return rc;
    pthread_mutex_lock(&g.lock);
    if (pcm == g.pcm) {
        uint8_t* buf = (uint8_t*)data;
        g.in.insert(g.in.end(), buf, buf + count);
        size_t off = 0;
        while (g.in.size() - off >= kBlockBytes) {
            process_block_l(g.in.data() + off);
            g.out.insert(g.out.end(), g.in.begin() + off, g.in.begin() + off + kBlockBytes);
            off += kBlockBytes;
        }
        if (off) g.in.erase(g.in.begin(), g.in.begin() + off);
        if (g.out.size() >= count) {
            memcpy(buf, g.out.data(), count);
            g.out.erase(g.out.begin(), g.out.begin() + count);
        } else {
            ALOGW("output underrun (%zu < %u), returning unprocessed audio", g.out.size(), count);
        }
    }
    pthread_mutex_unlock(&g.lock);
    return rc;
}

extern "C" int pcm_close(struct pcm* pcm) {
    resolve();
    pthread_mutex_lock(&g.lock);
    if (pcm == g.pcm) {
        ALOGI("mic PCM closed after %u blocks", g.blocks);
        teardown_l();
        g.blocks = 0;
    }
    pthread_mutex_unlock(&g.lock);
    return real_close(pcm);
}
