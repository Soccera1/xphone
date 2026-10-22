#ifndef XPHONE_AUDIO_H
#define XPHONE_AUDIO_H
#include "common.h"
typedef struct xp_audio xp_audio;
typedef struct xp_ringer xp_ringer;
xp_ringer *xp_ringer_start(void);
void xp_ringer_stop(xp_ringer *ringer);
typedef void (*xp_encoded_cb)(const uint8_t *data, size_t len, void *arg);
typedef void (*xp_audio_event_cb)(const char *event, void *arg);
typedef struct {
    uint64_t sent, received, concealed, underruns;
    double left_energy, right_energy;
} xp_audio_stats;
void xp_audio_probe(int *capture, int *playback);
xp_audio *xp_audio_start(bool synthetic, int tx_channels, int rx_channels, int complexity,
                         xp_encoded_cb encoded, xp_audio_event_cb event, void *arg);
void xp_audio_stop(xp_audio *audio, xp_audio_stats *stats);
void xp_audio_receive(xp_audio *audio, uint32_t sequence, const uint8_t *data, size_t len);
void xp_audio_controls(xp_audio *audio, bool muted, int volume);
#endif
