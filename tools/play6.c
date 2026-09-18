// Minimal tinyalsa tone player (armv7) to drive the speaker DAC directly, so the
// FPGA loopback (pcmC0D22c ch4-5) can be captured while the audio HAL is stopped.
// Plays a 1 kHz sine for N seconds. Links the device's libtinyalsa.so.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum pcm_format { PCM_FORMAT_S16_LE = 0, PCM_FORMAT_S32_LE, PCM_FORMAT_S8, PCM_FORMAT_S24_LE, PCM_FORMAT_S24_3LE };
struct pcm_config {
    unsigned int channels, rate, period_size, period_count;
    enum pcm_format format;
    unsigned int start_threshold, stop_threshold, silence_threshold, silence_size;
    unsigned int _pad[4];
};
#define PCM_OUT 0x00000000

extern struct pcm *pcm_open(unsigned int, unsigned int, unsigned int, const struct pcm_config *);
extern int pcm_is_ready(struct pcm *);
extern const char *pcm_get_error(struct pcm *);
extern int pcm_write(struct pcm *, const void *, unsigned int);
extern int pcm_close(struct pcm *);

int main(int argc, char **argv) {
    unsigned int device = (argc > 1) ? (unsigned)atoi(argv[1]) : 0;
    unsigned int rate   = (argc > 2) ? (unsigned)atoi(argv[2]) : 44100;
    unsigned int chans  = (argc > 3) ? (unsigned)atoi(argv[3]) : 2;
    unsigned int secs   = (argc > 4) ? (unsigned)atoi(argv[4]) : 10;
    double freq = 1000.0, amp = 0.6 * 32767.0;

    struct pcm_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.channels = chans; cfg.rate = rate; cfg.period_size = 1024; cfg.period_count = 4;
    cfg.format = PCM_FORMAT_S16_LE;

    struct pcm *pcm = pcm_open(0, device, PCM_OUT, &cfg);
    if (!pcm || !pcm_is_ready(pcm)) { fprintf(stderr, "play open failed dev=%u: %s\n", device, pcm ? pcm_get_error(pcm) : "null"); return 1; }

    unsigned int frames = 1024;
    short *buf = (short *)malloc(frames * chans * sizeof(short));
    unsigned long total = (unsigned long)rate * secs, done = 0; double ph = 0.0, step = 2.0 * M_PI * freq / rate;
    while (done < total) {
        for (unsigned int i = 0; i < frames; i++) {
            short s = (short)(amp * sin(ph)); ph += step; if (ph > 2*M_PI) ph -= 2*M_PI;
            for (unsigned int c = 0; c < chans; c++) buf[i*chans + c] = s;
        }
        if (pcm_write(pcm, buf, frames * chans * sizeof(short)) != 0) { fprintf(stderr, "write err: %s\n", pcm_get_error(pcm)); break; }
        done += frames;
    }
    pcm_close(pcm); fprintf(stderr, "played %lu frames on dev %u\n", done, device); return 0;
}
