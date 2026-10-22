#include "audio.h"
#include <opus/opus.h>
#include <speex/speex_echo.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
typedef struct {
    pthread_mutex_t lock;
    xp_audio *audio;
    OpusDecoder *decoder;
    uint32_t sequence;
    double spectrum[2][2], muted_energy;
    size_t samples, muted_samples;
    bool muted, error;
} loopback;
static void encoded(const uint8_t *data, size_t len, void *arg) {
    loopback *l = arg;
    int16_t pcm[XP_FRAME * 2];
    assert(opus_decode(l->decoder, data, (opus_int32)len, pcm, XP_FRAME, 0) == XP_FRAME);
    pthread_mutex_lock(&l->lock);
    if (l->audio)
        xp_audio_receive(l->audio, l->sequence++, data, len);
    for (int i = 0; i < XP_FRAME; ++i) {
        if (l->samples > XP_RATE / 4 && !l->muted)
            for (int ch = 0; ch < 2; ++ch)
                for (int tone = 0; tone < 2; ++tone)
                    l->spectrum[ch][tone] += pcm[i * 2 + ch] * sin(2 * M_PI * (tone ? 880 : 440) *
                                                                   (double)l->samples / XP_RATE);
        if (l->muted) {
            if (l->muted_samples > XP_RATE / 4)
                l->muted_energy += (double)pcm[i * 2] * pcm[i * 2];
            ++l->muted_samples;
        }
        ++l->samples;
    }
    pthread_mutex_unlock(&l->lock);
}
static void event(const char *name, void *arg) {
    loopback *l = arg;
    if (!strcmp(name, "audio_error"))
        l->error = true;
}
static void delay_ms(int ms) {
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
static void echo_test(void) {
    SpeexEchoState *echo = speex_echo_state_init_mc(XP_FRAME, XP_RATE / 5, 2, 2);
    int rate = XP_RATE;
    speex_echo_ctl(echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    int16_t history[XP_FRAME * 2 * 2] = {0};
    double before = 0, after = 0;
    uint32_t random = 1234567;
    for (int frame = 0; frame < 200; ++frame) {
        int16_t playback[XP_FRAME * 2], capture[XP_FRAME * 2], out[XP_FRAME * 2];
        for (int i = 0; i < XP_FRAME * 2; ++i) {
            random = random * 1664525U + 1013904223U;
            playback[i] = (int16_t)((int)(random >> 16) - 32768) / 5;
            capture[i] = (int16_t)(history[i] * 0.5);
        }
        speex_echo_cancellation(echo, capture, playback, out);
        memcpy(history, playback, sizeof(playback));
        if (frame > 150)
            for (int i = 0; i < XP_FRAME * 2; ++i) {
                before += (double)capture[i] * capture[i];
                after += (double)out[i] * out[i];
            }
    }
    double reduction = 10 * log10(before / (after + 1));
    printf("multichannel synthetic echo reduction: %.1f dB\n", reduction);
    assert(reduction > 10);
    speex_echo_state_destroy(echo);
}
int main(void) {
    int error;
    loopback l = {.lock = PTHREAD_MUTEX_INITIALIZER};
    l.decoder = opus_decoder_create(XP_RATE, 2, &error);
    assert(l.decoder);
    xp_audio *audio = xp_audio_start(true, 2, 2, 5, encoded, event, &l);
    assert(audio);
    pthread_mutex_lock(&l.lock);
    l.audio = audio;
    pthread_mutex_unlock(&l.lock);
    delay_ms(800);
    pthread_mutex_lock(&l.lock);
    l.muted = true;
    pthread_mutex_unlock(&l.lock);
    xp_audio_controls(audio, true, 80);
    delay_ms(800);
    xp_audio_stats stats;
    xp_audio_stop(audio, &stats);
    assert(!l.error && stats.sent > 50 && stats.received > 20);
    assert(stats.left_energy > 0 && stats.right_energy > 0);
    assert(fabs(l.spectrum[0][0]) > fabs(l.spectrum[0][1]) * 8);
    assert(fabs(l.spectrum[1][1]) > fabs(l.spectrum[1][0]) * 8);
    double rms = sqrt(l.muted_energy / (double)(l.muted_samples - XP_RATE / 4));
    printf("stereo pipeline: %llu frames, muted RMS %.2f\n", (unsigned long long)stats.received,
           rms);
    assert(rms < 100);
    opus_decoder_destroy(l.decoder);
    pthread_mutex_destroy(&l.lock);
    echo_test();
    return 0;
}
