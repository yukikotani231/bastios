/* Plays a tone through SDL2's audio API (its ALSA backend), as a game or a
 * media player would: opens the default device with a callback, lets it run
 * for a while and prints what the device really gave. The SDL types are
 * declared here so the program builds without SDL's headers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef void (*audio_cb)(void *user, uint8_t *stream, int len);
typedef struct {
    int freq;
    uint16_t format;
    uint8_t channels, silence;
    uint16_t samples, padding;
    uint32_t size;
    audio_cb callback;
    void *userdata;
} spec_t;

int SDL_Init(uint32_t flags);
const char *SDL_GetError(void);
const char *SDL_GetCurrentAudioDriver(void);
uint32_t SDL_OpenAudioDevice(const char *dev, int capture, const spec_t *want, spec_t *have, int allowed);
void SDL_PauseAudioDevice(uint32_t dev, int pause_on);
void SDL_CloseAudioDevice(uint32_t dev);
void SDL_Delay(uint32_t ms);
void SDL_Quit(void);

#define SDL_INIT_AUDIO 0x10
#define AUDIO_S16LSB 0x8010

static volatile long frames;

static void fill(void *user, uint8_t *stream, int len)
{
    int16_t *o = (int16_t *)stream;
    int n = len / 4;   /* stereo S16 */
    for (int i = 0; i < n; i++) {
        int16_t v = ((frames + i) / 24) % 2 ? 8000 : -8000;   /* 1 kHz at 48 kHz */
        o[2 * i] = o[2 * i + 1] = v;
    }
    frames += n;
}

int main(void)
{
    if (SDL_Init(SDL_INIT_AUDIO) != 0) { printf("sdlbeep: init: %s\n", SDL_GetError()); return 1; }
    spec_t want, have;
    memset(&want, 0, sizeof want);
    want.freq = 48000; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 1024; want.callback = fill;
    uint32_t dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { printf("sdlbeep: open: %s\n", SDL_GetError()); return 1; }
    printf("sdlbeep: driver %s, %d Hz, %d channels\n", SDL_GetCurrentAudioDriver(), have.freq, have.channels);
    SDL_PauseAudioDevice(dev, 0);
    SDL_Delay(1500);
    SDL_CloseAudioDevice(dev);
    SDL_Quit();
    printf("sdlbeep: %s\n", frames > 24000 ? "ok" : "too few frames");
    return frames > 24000 ? 0 : 1;
}
