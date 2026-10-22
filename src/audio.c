#include "audio.h"
#include <opus/opus.h>
#include <pulse/pulseaudio.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PCM_CAP (XP_FRAME * 2 * 16)
typedef struct {
    int16_t samples[PCM_CAP];
    size_t begin, count;
} pcm_ring;
typedef struct {
    uint8_t data[XP_MEDIA_MAX];
    size_t len;
    uint32_t sequence;
    uint64_t arrival;
    bool occupied;
} packet;
struct xp_audio {
    pthread_t thread;
    pthread_mutex_t lock;
    atomic_bool stop, muted;
    atomic_int volume;
    bool synthetic;
    int tx, rx, complexity;
    xp_encoded_cb encoded;
    xp_audio_event_cb event;
    void *arg;
    packet jitter[32];
    bool receiving;
    uint32_t expected;
    uint64_t playout_at;
    xp_audio_stats stats;
    pa_mainloop *loop;
    pa_context *context;
    pa_stream *capture, *playback;
    pcm_ring input, output;
    int16_t references[32][XP_FRAME * 2];
    int16_t reference_partial[XP_FRAME * 2];
    size_t ref_count, ref_partial, ref_head;
    uint32_t capture_device, playback_device;
    bool device_reported;
};
static void ring_write(pcm_ring *r, const int16_t *samples, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (r->count == PCM_CAP) {
            r->begin = (r->begin + 1) % PCM_CAP;
            --r->count;
        }
        r->samples[(r->begin + r->count++) % PCM_CAP] = samples ? samples[i] : 0;
    }
}
static size_t ring_read(pcm_ring *r, int16_t *out, size_t count) {
    size_t n = r->count < count ? r->count : count;
    for (size_t i = 0; i < n; ++i) {
        out[i] = r->samples[r->begin];
        r->begin = (r->begin + 1) % PCM_CAP;
    }
    r->count -= n;
    memset(out + n, 0, (count - n) * sizeof(*out));
    return n;
}
typedef struct {
    bool done;
    int channels;
} probe_result;
static void source_info(pa_context *ctx, const pa_source_info *info, int eol, void *arg) {
    (void)ctx;
    probe_result *r = arg;
    if (info)
        r->channels = info->channel_map.channels >= 2 ? 2 : 1;
    if (eol)
        r->done = true;
}
static void sink_info(pa_context *ctx, const pa_sink_info *info, int eol, void *arg) {
    (void)ctx;
    probe_result *r = arg;
    if (info)
        r->channels = info->channel_map.channels >= 2 ? 2 : 1;
    if (eol)
        r->done = true;
}
static bool context_ready(pa_mainloop *loop, pa_context *ctx, uint64_t deadline) {
    while (xp_now_ms() < deadline) {
        pa_context_state_t state = pa_context_get_state(ctx);
        if (state == PA_CONTEXT_READY)
            return true;
        if (!PA_CONTEXT_IS_GOOD(state))
            return false;
        pa_mainloop_iterate(loop, 0, NULL);
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    return false;
}
void xp_audio_probe(int *capture, int *playback) {
    *capture = *playback = 1;
    pa_mainloop *loop = pa_mainloop_new();
    if (!loop)
        return;
    pa_context *ctx = pa_context_new(pa_mainloop_get_api(loop), "XPhone device discovery");
    if (!ctx) {
        pa_mainloop_free(loop);
        return;
    }
    if (!pa_context_connect(ctx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) &&
        context_ready(loop, ctx, xp_now_ms() + 1500)) {
        probe_result source = {.channels = 1}, sink = {.channels = 1};
        pa_operation *a =
            pa_context_get_source_info_by_name(ctx, "@DEFAULT_SOURCE@", source_info, &source);
        pa_operation *b = pa_context_get_sink_info_by_name(ctx, "@DEFAULT_SINK@", sink_info, &sink);
        uint64_t deadline = xp_now_ms() + 1000;
        while (a && b && (!source.done || !sink.done) && xp_now_ms() < deadline) {
            pa_mainloop_iterate(loop, 0, NULL);
            struct timespec delay = {.tv_nsec = 1000000};
            nanosleep(&delay, NULL);
        }
        *capture = source.channels;
        *playback = sink.channels;
        if (a) {
            pa_operation_cancel(a);
            pa_operation_unref(a);
        }
        if (b) {
            pa_operation_cancel(b);
            pa_operation_unref(b);
        }
    }
    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(loop);
}
static void capture_ready(pa_stream *stream, size_t bytes, void *arg) {
    (void)bytes;
    xp_audio *a = arg;
    const void *data;
    size_t len;
    while (pa_stream_readable_size(stream) > 0) {
        if (pa_stream_peek(stream, &data, &len) < 0 || !len)
            break;
        ring_write(&a->input, data, len / sizeof(int16_t));
        pa_stream_drop(stream);
    }
}
static void playback_ready(pa_stream *stream, size_t bytes, void *arg) {
    xp_audio *a = arg;
    int16_t samples[XP_FRAME * 2];
    while (bytes) {
        size_t count = bytes / sizeof(int16_t);
        if (count > XP_FRAME * (size_t)a->rx)
            count = XP_FRAME * (size_t)a->rx;
        count -= count % (size_t)a->rx;
        if (!count)
            break;
        if (ring_read(&a->output, samples, count) < count)
            ++a->stats.underruns;
        if (pa_stream_write(stream, samples, count * sizeof(int16_t), NULL, 0, PA_SEEK_RELATIVE) <
            0)
            break;
        for (size_t i = 0; i < count; ++i) {
            a->reference_partial[a->ref_partial++] = samples[i];
            if (a->ref_partial == XP_FRAME * (size_t)a->rx) {
                memcpy(a->references[a->ref_head], a->reference_partial,
                       a->ref_partial * sizeof(int16_t));
                a->ref_head = (a->ref_head + 1) % 32;
                if (a->ref_count < 32)
                    ++a->ref_count;
                a->ref_partial = 0;
            }
        }
        bytes -= count * sizeof(int16_t);
    }
}
static void moved(pa_stream *stream, void *arg) {
    xp_audio *a = arg;
    uint32_t index = pa_stream_get_device_index(stream);
    uint32_t *old = stream == a->capture ? &a->capture_device : &a->playback_device;
    if (*old != PA_INVALID_INDEX && *old != index && !a->device_reported) {
        a->device_reported = true;
        a->event("devices_changed", a->arg);
    }
    *old = index;
}
static bool pulse_start(xp_audio *a) {
    a->loop = pa_mainloop_new();
    if (!a->loop)
        return false;
    a->context = pa_context_new(pa_mainloop_get_api(a->loop), "XPhone");
    if (!a->context || pa_context_connect(a->context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) ||
        !context_ready(a->loop, a->context, xp_now_ms() + 1500))
        return false;
    pa_sample_spec capture_spec = {PA_SAMPLE_S16NE, XP_RATE, (uint8_t)a->tx};
    pa_sample_spec playback_spec = {PA_SAMPLE_S16NE, XP_RATE, (uint8_t)a->rx};
    pa_channel_map capture_map, playback_map;
    if (a->tx == 2)
        pa_channel_map_init_stereo(&capture_map);
    else
        pa_channel_map_init_mono(&capture_map);
    if (a->rx == 2)
        pa_channel_map_init_stereo(&playback_map);
    else
        pa_channel_map_init_mono(&playback_map);
    a->capture = pa_stream_new(a->context, "Call microphone", &capture_spec, &capture_map);
    a->playback = pa_stream_new(a->context, "Call speakers", &playback_spec, &playback_map);
    if (!a->capture || !a->playback)
        return false;
    pa_stream_set_read_callback(a->capture, capture_ready, a);
    pa_stream_set_write_callback(a->playback, playback_ready, a);
    pa_stream_set_moved_callback(a->capture, moved, a);
    pa_stream_set_moved_callback(a->playback, moved, a);
    pa_buffer_attr record_attr = {.maxlength = XP_FRAME * (uint32_t)a->tx * 2 * 8,
                                  .tlength = (uint32_t)-1,
                                  .prebuf = (uint32_t)-1,
                                  .minreq = (uint32_t)-1,
                                  .fragsize = XP_FRAME * (uint32_t)a->tx * 2};
    pa_buffer_attr play_attr = {.maxlength = XP_FRAME * (uint32_t)a->rx * 2 * 8,
                                .tlength = XP_FRAME * (uint32_t)a->rx * 2 * 3,
                                .prebuf = XP_FRAME * (uint32_t)a->rx * 2,
                                .minreq = XP_FRAME * (uint32_t)a->rx * 2,
                                .fragsize = (uint32_t)-1};
    pa_stream_flags_t flags =
        PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_INTERPOLATE_TIMING;
    if (pa_stream_connect_record(a->capture, NULL, &record_attr, flags) ||
        pa_stream_connect_playback(a->playback, NULL, &play_attr, flags, NULL, NULL))
        return false;
    uint64_t deadline = xp_now_ms() + 1500;
    while (xp_now_ms() < deadline && !atomic_load(&a->stop)) {
        pa_mainloop_iterate(a->loop, 0, NULL);
        pa_stream_state_t cs = pa_stream_get_state(a->capture),
                          ps = pa_stream_get_state(a->playback);
        if (cs == PA_STREAM_READY && ps == PA_STREAM_READY) {
            a->capture_device = pa_stream_get_device_index(a->capture);
            a->playback_device = pa_stream_get_device_index(a->playback);
            return true;
        }
        if (!PA_STREAM_IS_GOOD(cs) || !PA_STREAM_IS_GOOD(ps))
            return false;
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    return false;
}
static void pulse_stop(xp_audio *a) {
    if (a->capture) {
        pa_stream_disconnect(a->capture);
        pa_stream_unref(a->capture);
    }
    if (a->playback) {
        pa_stream_disconnect(a->playback);
        pa_stream_unref(a->playback);
    }
    if (a->context) {
        pa_context_disconnect(a->context);
        pa_context_unref(a->context);
    }
    if (a->loop)
        pa_mainloop_free(a->loop);
}
static int receive_frame(xp_audio *a, OpusDecoder *decoder, int16_t *pcm) {
    uint8_t data[XP_MEDIA_MAX];
    size_t len = 0;
    bool playing = false;
    pthread_mutex_lock(&a->lock);
    if (a->receiving && xp_now_ms() >= a->playout_at) {
        playing = true;
        packet *p = &a->jitter[a->expected % 32];
        if (p->occupied && p->sequence == a->expected && xp_now_ms() - p->arrival <= 200) {
            memcpy(data, p->data, p->len);
            len = p->len;
            p->occupied = false;
        }
        ++a->expected;
    }
    pthread_mutex_unlock(&a->lock);
    if (!playing) {
        memset(pcm, 0, XP_FRAME * (size_t)a->rx * sizeof(*pcm));
        return 0;
    }
    int n = opus_decode(decoder, len ? data : NULL, (opus_int32)len, pcm, XP_FRAME, 0);
    if (n != XP_FRAME)
        memset(pcm, 0, XP_FRAME * (size_t)a->rx * sizeof(*pcm));
    if (len && n == XP_FRAME)
        ++a->stats.received;
    else
        ++a->stats.concealed;
    return len ? 1 : 0;
}
static void *audio_thread(void *arg) {
    xp_audio *a = arg;
    if (a->tx < 1 || a->tx > 2 || a->rx < 1 || a->rx > 2) {
        a->event("audio_error", a->arg);
        return NULL;
    }
    int error;
    OpusEncoder *encoder = opus_encoder_create(XP_RATE, a->tx, OPUS_APPLICATION_VOIP, &error);
    OpusDecoder *decoder = opus_decoder_create(XP_RATE, a->rx, &error);
    SpeexEchoState *echo = speex_echo_state_init_mc(XP_FRAME, XP_RATE / 5, a->tx, a->rx);
    SpeexPreprocessState *pre[2] = {NULL, NULL};
    int rate = XP_RATE;
    if (!encoder || !decoder || !echo)
        goto failed;
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(a->tx == 2 ? 64000 : 32000));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(a->complexity));
    opus_encoder_ctl(encoder, OPUS_SET_FORCE_CHANNELS(a->tx));
    speex_echo_ctl(echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    for (int ch = 0; ch < a->tx; ++ch) {
        pre[ch] = speex_preprocess_state_init(XP_FRAME, XP_RATE);
        if (!pre[ch])
            goto failed;
        int enabled = 1, noise = -15, disabled = 0;
        speex_preprocess_ctl(pre[ch], SPEEX_PREPROCESS_SET_DENOISE, &enabled);
        speex_preprocess_ctl(pre[ch], SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise);
        speex_preprocess_ctl(pre[ch], SPEEX_PREPROCESS_SET_AGC, &disabled);
    }
    if (!a->synthetic && !pulse_start(a))
        goto failed;
    a->event("audio_ready", a->arg);
    uint64_t samples = 0, next = xp_now_ns();
    uint64_t sleep_offset = xp_sleep_offset_ms();
    while (!atomic_load(&a->stop)) {
        if (xp_sleep_offset_ms() > sleep_offset + 500) {
            a->event("suspended", a->arg);
            goto done;
        }
        if (!a->synthetic) {
            for (int i = 0; i < 4; ++i)
                pa_mainloop_iterate(a->loop, 0, NULL);
            if (pa_context_get_state(a->context) != PA_CONTEXT_READY ||
                pa_stream_get_state(a->capture) != PA_STREAM_READY ||
                pa_stream_get_state(a->playback) != PA_STREAM_READY)
                goto failed;
        }
        int16_t capture[XP_FRAME * 2], cleaned[XP_FRAME * 2], playback[XP_FRAME * 2],
            reference[XP_FRAME * 2] = {0};
        receive_frame(a, decoder, playback);
        double gain = atomic_load(&a->volume) / 100.0;
        for (int i = 0; i < XP_FRAME; ++i) {
            for (int ch = 0; ch < a->rx; ++ch) {
                size_t k = (size_t)i * (size_t)a->rx + (size_t)ch;
                playback[k] = (int16_t)(playback[k] * gain);
                double energy = (double)playback[k] * playback[k];
                if (ch == 0)
                    a->stats.left_energy += energy;
                else
                    a->stats.right_energy += energy;
            }
        }
        if (a->synthetic) {
            for (int i = 0; i < XP_FRAME; ++i)
                for (int ch = 0; ch < a->tx; ++ch)
                    capture[i * a->tx + ch] =
                        (int16_t)(8000 * sin(2 * M_PI * (ch ? 880 : 440) *
                                             (double)(samples + (uint64_t)i) / XP_RATE));
            memcpy(cleaned, capture, XP_FRAME * (size_t)a->tx * sizeof(*capture));
        } else {
            ring_write(&a->output, playback, XP_FRAME * (size_t)a->rx);
            ring_read(&a->input, capture, XP_FRAME * (size_t)a->tx);
            pa_usec_t input_latency = 0, output_latency = 0;
            int negative;
            pa_stream_get_latency(a->capture, &input_latency, &negative);
            pa_stream_get_latency(a->playback, &output_latency, &negative);
            size_t delay = (size_t)((input_latency + output_latency + 19999) / 20000);
            if (delay > 30)
                delay = 30;
            if (a->ref_count > delay)
                memcpy(reference, a->references[(a->ref_head + 31 - delay) % 32],
                       XP_FRAME * (size_t)a->rx * sizeof(*reference));
            speex_echo_cancellation(echo, capture, reference, cleaned);
            for (int ch = 0; ch < a->tx; ++ch) {
                int16_t mono[XP_FRAME];
                for (int i = 0; i < XP_FRAME; ++i)
                    mono[i] = cleaned[i * a->tx + ch];
                speex_preprocess_run(pre[ch], mono);
                for (int i = 0; i < XP_FRAME; ++i)
                    cleaned[i * a->tx + ch] = mono[i];
            }
        }
        if (atomic_load(&a->muted))
            memset(cleaned, 0, XP_FRAME * (size_t)a->tx * sizeof(*cleaned));
        uint8_t encoded[XP_MEDIA_MAX - 24];
        int len = opus_encode(encoder, cleaned, XP_FRAME, encoded, sizeof(encoded));
        if (len > 0) {
            a->encoded(encoded, (size_t)len, a->arg);
            ++a->stats.sent;
        }
        samples += XP_FRAME;
        next += 20000000;
        if (next + 100000000 < xp_now_ns())
            next = xp_now_ns();
        struct timespec deadline = {(time_t)(next / 1000000000), (long)(next % 1000000000)};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    }
    goto done;
failed:
    a->event("audio_error", a->arg);
done:
    pulse_stop(a);
    if (encoder)
        opus_encoder_destroy(encoder);
    if (decoder)
        opus_decoder_destroy(decoder);
    if (echo)
        speex_echo_state_destroy(echo);
    for (size_t i = 0; i < 2; ++i)
        if (pre[i])
            speex_preprocess_state_destroy(pre[i]);
    return NULL;
}
xp_audio *xp_audio_start(bool synthetic, int tx_channels, int rx_channels, int complexity,
                         xp_encoded_cb encoded, xp_audio_event_cb event, void *arg) {
    if ((tx_channels != 1 && tx_channels != 2) || (rx_channels != 1 && rx_channels != 2) ||
        complexity < 0 || complexity > 10)
        return NULL;
    xp_audio *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    pthread_mutex_init(&a->lock, NULL);
    atomic_init(&a->stop, false);
    atomic_init(&a->muted, false);
    atomic_init(&a->volume, 80);
    a->synthetic = synthetic;
    a->tx = tx_channels;
    a->rx = rx_channels;
    a->complexity = complexity;
    a->encoded = encoded;
    a->event = event;
    a->arg = arg;
    a->capture_device = a->playback_device = PA_INVALID_INDEX;
    if (pthread_create(&a->thread, NULL, audio_thread, a)) {
        pthread_mutex_destroy(&a->lock);
        free(a);
        return NULL;
    }
    return a;
}
void xp_audio_stop(xp_audio *a, xp_audio_stats *stats) {
    if (!a)
        return;
    atomic_store(&a->stop, true);
    pthread_join(a->thread, NULL);
    if (stats)
        *stats = a->stats;
    pthread_mutex_destroy(&a->lock);
    free(a);
}
void xp_audio_controls(xp_audio *a, bool muted, int volume) {
    if (!a)
        return;
    atomic_store(&a->muted, muted);
    atomic_store(&a->volume, volume < 0 ? 0 : volume > 100 ? 100 : volume);
}
void xp_audio_receive(xp_audio *a, uint32_t sequence, const uint8_t *data, size_t len) {
    if (!a || !len || len > XP_MEDIA_MAX)
        return;
    pthread_mutex_lock(&a->lock);
    if (!a->receiving) {
        a->receiving = true;
        a->expected = sequence;
        a->playout_at = xp_now_ms() + 60;
    }
    uint32_t diff = sequence - a->expected;
    if (diff >= 0x80000000U) {
        pthread_mutex_unlock(&a->lock);
        return;
    }
    if (diff >= 10) {
        memset(a->jitter, 0, sizeof(a->jitter));
        a->expected = sequence;
        a->playout_at = xp_now_ms() + 60;
    }
    packet *p = &a->jitter[sequence % 32];
    if (!p->occupied || p->sequence != sequence) {
        memcpy(p->data, data, len);
        p->len = len;
        p->sequence = sequence;
        p->arrival = xp_now_ms();
        p->occupied = true;
    }
    pthread_mutex_unlock(&a->lock);
}

struct xp_ringer {
    pthread_t thread;
    atomic_bool stop;
    uint64_t samples;
};
static void ring_write_ready(pa_stream *stream, size_t bytes, void *arg) {
    xp_ringer *r = arg;
    int16_t pcm[XP_FRAME];
    while (bytes >= sizeof(int16_t)) {
        size_t count = bytes / sizeof(int16_t);
        if (count > XP_FRAME)
            count = XP_FRAME;
        for (size_t i = 0; i < count; ++i) {
            uint64_t n = r->samples++;
            bool sounding = (n % (XP_RATE * 3)) < XP_RATE;
            double envelope = fmin(1.0, (double)(n % XP_RATE) / 480) *
                              fmin(1.0, (double)(XP_RATE - n % XP_RATE) / 480);
            pcm[i] = sounding ? (int16_t)(3500 * envelope *
                                          (sin(2 * M_PI * 440 * (double)n / XP_RATE) +
                                           sin(2 * M_PI * 480 * (double)n / XP_RATE)))
                              : 0;
        }
        if (pa_stream_write(stream, pcm, count * sizeof(int16_t), NULL, 0, PA_SEEK_RELATIVE) < 0)
            return;
        bytes -= count * sizeof(int16_t);
    }
}
static void *ringer_thread(void *arg) {
    xp_ringer *r = arg;
    pa_mainloop *loop = pa_mainloop_new();
    if (!loop)
        return NULL;
    pa_context *ctx = pa_context_new(pa_mainloop_get_api(loop), "XPhone ringing");
    pa_stream *stream = NULL;
    if (!ctx || pa_context_connect(ctx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) ||
        !context_ready(loop, ctx, xp_now_ms() + 1500) || atomic_load(&r->stop))
        goto done;
    pa_sample_spec spec = {PA_SAMPLE_S16NE, XP_RATE, 1};
    stream = pa_stream_new(ctx, "Incoming call", &spec, NULL);
    if (!stream)
        goto done;
    pa_stream_set_write_callback(stream, ring_write_ready, r);
    pa_buffer_attr attr = {.maxlength = XP_FRAME * 2 * 8,
                           .tlength = XP_FRAME * 2 * 3,
                           .prebuf = XP_FRAME * 2,
                           .minreq = XP_FRAME * 2,
                           .fragsize = (uint32_t)-1};
    if (pa_stream_connect_playback(stream, NULL, &attr, PA_STREAM_ADJUST_LATENCY, NULL, NULL))
        goto done;
    while (!atomic_load(&r->stop) && PA_CONTEXT_IS_GOOD(pa_context_get_state(ctx)) &&
           PA_STREAM_IS_GOOD(pa_stream_get_state(stream))) {
        pa_mainloop_iterate(loop, 0, NULL);
        struct timespec wait = {.tv_nsec = 10000000};
        nanosleep(&wait, NULL);
    }
done:
    if (stream) {
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
    }
    if (ctx) {
        pa_context_disconnect(ctx);
        pa_context_unref(ctx);
    }
    pa_mainloop_free(loop);
    return NULL;
}
xp_ringer *xp_ringer_start(void) {
    xp_ringer *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    atomic_init(&r->stop, false);
    if (pthread_create(&r->thread, NULL, ringer_thread, r)) {
        free(r);
        return NULL;
    }
    return r;
}
void xp_ringer_stop(xp_ringer *r) {
    if (!r)
        return;
    atomic_store(&r->stop, true);
    pthread_join(r->thread, NULL);
    free(r);
}
