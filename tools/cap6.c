// Minimal 6-channel S24_3LE capture from a tinyalsa PCM device (Echo Show FPGA
// mic+loopback stream pcmC0D22c). Links against the device's own libtinyalsa.so.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum pcm_format {
    PCM_FORMAT_S16_LE = 0,
    PCM_FORMAT_S32_LE,
    PCM_FORMAT_S8,
    PCM_FORMAT_S24_LE,
    PCM_FORMAT_S24_3LE,
};
struct pcm_config {
    unsigned int channels;
    unsigned int rate;
    unsigned int period_size;
    unsigned int period_count;
    enum pcm_format format;
    unsigned int start_threshold;
    unsigned int stop_threshold;
    unsigned int silence_threshold;
    unsigned int silence_size;
    unsigned int _pad[4]; // ABI tail safety
};
#define PCM_IN 0x10000000

extern struct pcm *pcm_open(unsigned int, unsigned int, unsigned int, const struct pcm_config *);
extern int pcm_is_ready(struct pcm *);
extern const char *pcm_get_error(struct pcm *);
extern int pcm_read(struct pcm *, void *, unsigned int);
extern int pcm_close(struct pcm *);

int main(int argc, char **argv) {
    unsigned int device = (argc > 1) ? (unsigned)atoi(argv[1]) : 22;
    unsigned int seconds = (argc > 2) ? (unsigned)atoi(argv[2]) : 12;
    const char *out = (argc > 3) ? argv[3] : "/data/local/tmp/6ch.raw";

    struct pcm_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channels = 6;
    cfg.rate = 16000;
    cfg.period_size = 257;
    cfg.period_count = 10;
    cfg.format = PCM_FORMAT_S24_3LE;

    struct pcm *pcm = pcm_open(0, device, PCM_IN, &cfg);
    if (!pcm || !pcm_is_ready(pcm)) {
        fprintf(stderr, "open failed: %s\n", pcm ? pcm_get_error(pcm) : "null pcm");
        return 1;
    }
    const unsigned int frame_bytes = 6 * 3;      // S24_3LE = 3 bytes/sample, 6 ch
    const unsigned int chunk_frames = 257;
    const unsigned int chunk_bytes = chunk_frames * frame_bytes;
    char *buf = (char *)malloc(chunk_bytes);
    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "cannot open %s\n", out); return 1; }

    unsigned int total = 16000 * seconds, got = 0;
    while (got < total) {
        int r = pcm_read(pcm, buf, chunk_bytes);
        if (r != 0) { fprintf(stderr, "read err %d: %s\n", r, pcm_get_error(pcm)); break; }
        fwrite(buf, 1, chunk_bytes, f);
        got += chunk_frames;
    }
    fclose(f);
    pcm_close(pcm);
    fprintf(stderr, "captured %u frames -> %s\n", got, out);
    return 0;
}
