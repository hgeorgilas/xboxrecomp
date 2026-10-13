/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      3

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_underruns;
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;

/* Recovery. XAudio2 stops for good when its device goes away -- a headset
 * switched off, the default device changed, a monitor's speakers asleep --
 * and reports it once, through OnCriticalError. A voice whose position
 * stops moving with its queue full is treated the same way. Either has the
 * submitting thread rebuild the engine on the current default device. */
static volatile LONG           g_xa2_lost;
static UINT64                  g_xa2_last_played;
static ULONGLONG               g_xa2_stuck_since;

static void STDMETHODCALLTYPE xa2_cb_pass_start(IXAudio2EngineCallback *This, UINT32 bytes)
{ (void)This; (void)bytes; }
static void STDMETHODCALLTYPE xa2_cb_pass_end(IXAudio2EngineCallback *This)
{ (void)This; }
static void STDMETHODCALLTYPE xa2_cb_error(IXAudio2EngineCallback *This, HRESULT err)
{
    (void)This;
    fprintf(stderr, "[XA2] critical error 0x%08lX (device lost?) -- will restart audio\n", err);
    InterlockedExchange(&g_xa2_lost, 1);
}
static IXAudio2EngineCallbackVtbl g_xa2_cb_vtbl = { xa2_cb_pass_start, xa2_cb_pass_end, xa2_cb_error };
static IXAudio2EngineCallback g_xa2_cb = { &g_xa2_cb_vtbl };

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    IXAudio2_RegisterForCallbacks(g_xa2, &g_xa2_cb);

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    g_xa2_last_played = 0;
    g_xa2_stuck_since = 0;
    InterlockedExchange(&g_xa2_lost, 0);

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (g_xa2_lost) {
        static ULONGLONG last_try;
        ULONGLONG now = GetTickCount64();
        if (now - last_try < 1000)
            return 0;                  /* once a second until it comes back */
        last_try = now;
        xa2_shutdown();
        if (xa2_init())
            fprintf(stderr, "[XA2] audio restarted\n");
        else
            InterlockedExchange(&g_xa2_lost, 1);
        return 0;
    }
    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, 0);
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) {
        ULONGLONG now = GetTickCount64();
        if (state.SamplesPlayed != g_xa2_last_played) {
            g_xa2_last_played = state.SamplesPlayed;
            g_xa2_stuck_since = now;
        } else if (!g_xa2_stuck_since) {
            g_xa2_stuck_since = now;
        } else if (now - g_xa2_stuck_since > 1500) {
            fprintf(stderr, "[XA2] playback stalled for 1.5 s -- restarting audio\n");
            InterlockedExchange(&g_xa2_lost, 1);
        }
        return 0;
    }
    g_xa2_last_played = state.SamplesPlayed;
    g_xa2_stuck_since = 0;
    if (state.BuffersQueued == 0 && g_xa2_frames_written)
        g_xa2_underruns++;             /* the queue had run dry: a gap */

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) return 0;

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    return 1;
}

/* Times a buffer arrived to an empty queue since the last call. */
int xa2_take_underruns(void)
{
    int n = g_xa2_underruns;
    g_xa2_underruns = 0;
    return n;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- POSIX stubs (no audio output yet) */

int  xa2_init(void)                                   { return 0; }
void xa2_shutdown(void)                               {}
int  xa2_is_active(void)                              { return 0; }
int  xa2_submit_samples(const int16_t *s, int n)      { (void)s; (void)n; return 0; }
int  xa2_get_buffer_size(void)                        { return 0; }
int  xa2_take_underruns(void)                         { return 0; }

#endif /* _WIN32 */
