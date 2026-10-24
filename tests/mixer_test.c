/* DirectAudio mixer regression and microbenchmark, 2026-10-24.
 * The reference include is extracted unchanged from directaudio-v1.4.0. */
#include "../directaudio.c"
#include "reference_mixer.inc"
#include <assert.h>
#include <math.h>

static unsigned rng = 0x12345678;

static unsigned random_word(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static float random_float(void)
{
    return ((int)(random_word() & 0xffff) - 32768) / 32768.0f;
}

static void initialize(struct directaudio_stream *v, WAVEFORMATEX *fmt, BYTE *buffer)
{
    memset(v, 0, sizeof(*v));
    pthread_mutex_init(&v->lock, NULL);
    v->playing = 1;
    v->fmt = fmt;
    v->local_buffer = buffer;
    v->aa_rate = MIX_OUT_RATE;
    v->is_float = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
}

__attribute__((noinline)) static void run_reference(struct directaudio_stream *v, float *out, int n)
{
    reference_mix_voice(v, out, n);
}

__attribute__((noinline)) static void run_optimized(struct directaudio_stream *v, float *out, int n)
{
    mix_voice(v, out, n);
}

static unsigned regression(void)
{
    const unsigned sizes[] = {1, 2, 3, 5, 17, 192, 257};
    const unsigned rates[] = {48000, 44100, 96000};
    unsigned test, i;
    for (test = 0; test < 12000; test++)
    {
        WAVEFORMATEX fmt = {0};
        struct directaudio_stream a, b;
        float out_a[2 * 512 + 8], out_b[2 * 512 + 8];
        _Alignas(float) BYTE buffer[257 * 8 * 4];
        unsigned capacity = sizes[random_word() % ARRAY_SIZE(sizes)];
        unsigned requested = random_word() % 513;
        unsigned format = test % 5;
        fmt.wFormatTag = format == 0 ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        fmt.wBitsPerSample = format == 0 ? 32 : (WORD[]){8, 16, 24, 32}[format - 1];
        fmt.nChannels = (WORD[]){1, 2, 3, 4, 6, 8}[random_word() % 6];
        /* Exercise the measured fast path frequently, including odd ring spans. */
        if (test % 2 == 0) fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT,
            fmt.wBitsPerSample = 32, fmt.nChannels = 2;
        fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
        for (i = 0; i < sizeof(buffer); i++) buffer[i] = random_word();
        if (fmt.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
            for (i = 0; i < capacity * fmt.nChannels; i++)
                ((float *)buffer)[i] = random_float();
        initialize(&a, &fmt, buffer);
        initialize(&b, &fmt, buffer);
        a.bufsize_frames = b.bufsize_frames = capacity;
        a.lcl_offs_frames = b.lcl_offs_frames = random_word() % capacity;
        a.held_frames = b.held_frames = random_word() % (capacity + 1);
        a.playing = b.playing = test % 19 != 0;
        a.aa_rate = b.aa_rate = rates[test % 3];
        a.vols_active = b.vols_active = test % 4 == 1;
        a.rs_pos = b.rs_pos = (test % 4) * 0.125;
        for (i = 0; i < 8; i++) a.vols[i] = b.vols[i] = random_float();
        for (i = 0; i < ARRAY_SIZE(out_a); i++) out_a[i] = out_b[i] = random_float();
        run_reference(&a, out_a, requested);
        run_optimized(&b, out_b, requested);
        if (memcmp(out_a, out_b, sizeof(out_a)) ||
            a.lcl_offs_frames != b.lcl_offs_frames || a.held_frames != b.held_frames ||
            a.rs_pos != b.rs_pos)
        {
            fprintf(stderr, "mismatch test=%u channels=%u bits=%u rate=%d size=%u requested=%u\n",
                    test, fmt.nChannels, fmt.wBitsPerSample, a.aa_rate, capacity, requested);
            abort();
        }
        {
            struct directaudio_mixer ma = { .lock = PTHREAD_MUTEX_INITIALIZER };
            struct directaudio_mixer mb = { .lock = PTHREAD_MUTEX_INITIALIZER };
            a.held_frames = b.held_frames = capacity;
            ma.voices[0] = &a;
            mb.voices[0] = &b;
            ma.nvoices = mb.nvoices = 1;
            reference_mix_block(&ma, out_a, requested);
            mix_block(&mb, out_b, requested);
            assert(!memcmp(out_a, out_b, sizeof(out_a)));
            assert(a.lcl_offs_frames == b.lcl_offs_frames && a.held_frames == b.held_frames);
            assert(a.rs_pos == b.rs_pos);
            pthread_mutex_destroy(&ma.lock);
            pthread_mutex_destroy(&mb.lock);
        }
        pthread_mutex_destroy(&a.lock);
        pthread_mutex_destroy(&b.lock);
    }
    /* Preserve IEEE edge values bit-for-bit on the new arithmetic path. */
    for (test = 0; test < 16; test++)
    {
        WAVEFORMATEX fmt = {.wFormatTag = WAVE_FORMAT_IEEE_FLOAT,
                            .nChannels = 2, .wBitsPerSample = 32, .nBlockAlign = 8};
        struct directaudio_stream a, b;
        float buffer[] = {0.0f, -0.0f, INFINITY, -INFINITY, NAN, 1.e-40f, -1.e-40f, 1.f};
        float out_a[8], out_b[8];
        initialize(&a, &fmt, (BYTE *)buffer);
        initialize(&b, &fmt, (BYTE *)buffer);
        a.bufsize_frames = b.bufsize_frames = 4;
        a.held_frames = b.held_frames = 4;
        a.lcl_offs_frames = b.lcl_offs_frames = test % 4;
        for (i = 0; i < 8; i++) out_a[i] = out_b[i] = buffer[(i + test) % 8];
        run_reference(&a, out_a, 4);
        run_optimized(&b, out_b, 4);
        assert(!memcmp(out_a, out_b, sizeof(out_a)));
        pthread_mutex_destroy(&a.lock);
        pthread_mutex_destroy(&b.lock);
    }
    return test + 12000;
}

static double cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1.e9 + ts.tv_nsec;
}

__attribute__((noinline)) static void run_reference_block(struct directaudio_mixer *mx, float *out, int n)
{
    reference_mix_block(mx, out, n);
}

__attribute__((noinline)) static void run_optimized_block(struct directaudio_mixer *mx, float *out, int n)
{
    mix_block(mx, out, n);
}

static void benchmark(int full_block)
{
    WAVEFORMATEX fmt = {.wFormatTag = WAVE_FORMAT_IEEE_FLOAT,
                        .nChannels = 2, .wBitsPerSample = 32, .nBlockAlign = 8};
    float buffer[8192 * 2], out[192 * 2];
    struct directaudio_stream v;
    struct directaudio_mixer mx = { .lock = PTHREAD_MUTEX_INITIALIZER };
    unsigned pass, mode, j, i;
    initialize(&v, &fmt, (BYTE *)buffer);
    v.bufsize_frames = 8192;
    mx.voices[0] = &v;
    mx.nvoices = 1;
    for (i = 0; i < ARRAY_SIZE(buffer); i++) buffer[i] = random_float();
    for (pass = 0; pass < 6; pass++)
        for (j = 0; j < 2; j++)
        {
            double start, elapsed;
            mode = j ^ (pass & 1);
            memset(out, 0, sizeof(out));
            v.lcl_offs_frames = 0;
            start = cpu_ns();
            for (i = 0; i < 100000; i++)
            {
                v.held_frames = 8192;
                if (full_block && mode) run_optimized_block(&mx, out, 192);
                else if (full_block) run_reference_block(&mx, out, 192);
                else if (mode) run_optimized(&v, out, 192);
                else run_reference(&v, out, 192);
            }
            elapsed = cpu_ns() - start;
            printf("%s,%u,%s,%.2f,%.6f\n", full_block ? "block" : "voice",
                   pass, mode ? "optimized" : "reference",
                   elapsed / 100000, out[0]);
        }
    pthread_mutex_destroy(&v.lock);
    pthread_mutex_destroy(&mx.lock);
}

int main(void)
{
    printf("regression: %u cases bit-identical\n", regression());
    benchmark(0);
    benchmark(1);
    return 0;
}
