/* DirectAudio format and memory-boundary regressions, 2026-10-24. */
#include "../directaudio.c"
#include "reference_mixer.inc"
#include <assert.h>
#include <math.h>
#include <sys/mman.h>

static unsigned checks;
static unsigned seed = 0x98765432;

static unsigned next_word(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static void setup(struct directaudio_stream *v, WAVEFORMATEXTENSIBLE *fmt,
                  BYTE *buffer, unsigned format, unsigned channels, unsigned rate,
                  unsigned capacity, unsigned offset, unsigned held, unsigned gain)
{
    unsigned i;
    memset(fmt, 0, sizeof(*fmt));
    fmt->Format.wFormatTag = format >= 5 ? WAVE_FORMAT_EXTENSIBLE :
        (format == 0 ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM);
    fmt->Format.nChannels = channels;
    fmt->Format.nSamplesPerSec = rate;
    fmt->Format.wBitsPerSample = (WORD[]){32, 8, 16, 24, 32}[format % 5];
    fmt->Format.nBlockAlign = channels * fmt->Format.wBitsPerSample / 8;
    fmt->Format.nAvgBytesPerSec = rate * fmt->Format.nBlockAlign;
    fmt->Format.cbSize = format >= 5 ? 22 : 0;
    fmt->Samples.wValidBitsPerSample = fmt->Format.wBitsPerSample;
    fmt->SubFormat = format % 5 == 0 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    memset(v, 0, sizeof(*v));
    pthread_mutex_init(&v->lock, NULL);
    v->fmt = &fmt->Format;
    v->local_buffer = buffer;
    v->is_float = format % 5 == 0;
    v->playing = 1;
    v->aa_rate = rate;
    v->bufsize_frames = capacity;
    v->lcl_offs_frames = offset;
    v->held_frames = held;
    v->vols_active = gain != 0;
    for (i = 0; i < 8; i++)
        v->vols[i] = gain == 1 ? 0.f : gain == 2 ? (i & 1 ? -0.5f : 1.25f) : 1.f;
}

static void equal_outputs(const float *a, const float *b, unsigned count)
{
    unsigned i;
    for (i = 0; i < count; i++)
        if (memcmp(a + i, b + i, sizeof(float)))
        {
            unsigned x, y;
            memcpy(&x, a + i, sizeof(x));
            memcpy(&y, b + i, sizeof(y));
            fprintf(stderr, "mismatch check=%u sample=%u reference=%08x optimized=%08x\n",
                    checks, i, x, y);
            abort();
        }
}

static void equal_state(const struct directaudio_stream *a, const struct directaudio_stream *b)
{
    assert(a->lcl_offs_frames == b->lcl_offs_frames);
    assert(a->held_frames == b->held_frames);
    assert(!memcmp(&a->rs_pos, &b->rs_pos, sizeof(a->rs_pos)));
    assert(a->playing == b->playing && a->vols_active == b->vols_active);
}

static void format_matrix(void)
{
    const unsigned rates[] = {8000, 22050, 44100, 48000, 96000, 192000};
    unsigned format, channels, rate, gain, variant, i;
    for (format = 0; format < 10; format++)
        for (channels = 1; channels <= 8; channels++)
            for (rate = 0; rate < ARRAY_SIZE(rates); rate++)
                for (gain = 0; gain < 4; gain++)
                    for (variant = 0; variant < 12; variant++)
                    {
                        _Alignas(float) BYTE storage[32 * 8 * 4 + 16];
                        BYTE *buffer = storage + (variant % 4) * sizeof(float);
                        float a_out[2 * 40 + 4], b_out[2 * 40 + 4];
                        unsigned capacity = (unsigned[]){1, 2, 3, 5, 17, 31}[variant % 6];
                        unsigned held = variant == 0 ? 0 : variant == 1 ? 1 : capacity;
                        unsigned requested = (unsigned[]){0, 1, 2, 3, 4, 7, 16, 17, 30, 31, 32, 40}[variant];
                        WAVEFORMATEXTENSIBLE af, bf;
                        struct directaudio_stream a, b;
                        setup(&a, &af, buffer, format, channels, rates[rate], capacity,
                              variant % 2 ? capacity - 1 : 0, held, gain);
                        setup(&b, &bf, buffer, format, channels, rates[rate], capacity,
                              a.lcl_offs_frames, held, gain);
                        if (variant == 2) a.playing = b.playing = 0;
                        /* A padded block must use the original dispatcher. */
                        if (variant == 3) af.Format.nBlockAlign += 4, bf.Format.nBlockAlign += 4;
                        a.rs_pos = b.rs_pos = (variant % 4) * 0.25;
                        for (i = 0; i < capacity * af.Format.nBlockAlign; i++) buffer[i] = next_word();
                        if (a.is_float)
                            for (i = 0; i < capacity; i++)
                            {
                                unsigned c;
                                float *p = (float *)(buffer + i * af.Format.nBlockAlign);
                                for (c = 0; c < channels; c++)
                                    p[c] = ((int)(next_word() & 0xffff) - 32768) / 8192.f;
                            }
                        for (i = 0; i < ARRAY_SIZE(a_out); i++) a_out[i] = b_out[i] = i / 256.f;
                        reference_mix_voice(&a, a_out, requested);
                        mix_voice(&b, b_out, requested);
                        equal_outputs(a_out, b_out, ARRAY_SIZE(a_out));
                        equal_state(&a, &b);
                        pthread_mutex_destroy(&a.lock);
                        pthread_mutex_destroy(&b.lock);
                        checks++;
                    }
    printf("format matrix: %u cases passed\n", checks);
}

static void float_edges(void)
{
    const unsigned bits[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x00800000,
        0x7f7fffff, 0xff7fffff, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000,
        0x7fc12345, 0xffc54321, 0x7fa12345, 0xffa54321};
    unsigned s, d, offset, i;
    for (s = 0; s < ARRAY_SIZE(bits); s++)
        for (d = 0; d < ARRAY_SIZE(bits); d++)
            for (offset = 0; offset < 5; offset++)
            {
                WAVEFORMATEXTENSIBLE af, bf;
                struct directaudio_stream a, b;
                float buffer[10], a_out[14], b_out[14];
                for (i = 0; i < 10; i++) memcpy(buffer + i, bits + s, sizeof(float));
                for (i = 0; i < 14; i++) memcpy(a_out + i, bits + d, sizeof(float));
                memcpy(b_out, a_out, sizeof(a_out));
                setup(&a, &af, (BYTE *)buffer, 0, 2, 48000, 5, offset, 5, 0);
                setup(&b, &bf, (BYTE *)buffer, 0, 2, 48000, 5, offset, 5, 0);
                reference_mix_voice(&a, a_out, 5);
                mix_voice(&b, b_out, 5);
                equal_outputs(a_out, b_out, ARRAY_SIZE(a_out));
                equal_state(&a, &b);
                pthread_mutex_destroy(&a.lock);
                pthread_mutex_destroy(&b.lock);
                checks++;
            }
}

static void guard_pages(void)
{
    size_t page = sysconf(_SC_PAGESIZE);
    BYTE *src = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    BYTE *dst = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned capacity, offset, requested, edge, i;
    assert(src != MAP_FAILED && dst != MAP_FAILED);
    assert(!mprotect(src + page, page, PROT_READ | PROT_WRITE));
    assert(!mprotect(dst + page, page, PROT_READ | PROT_WRITE));
    for (capacity = 1; capacity <= 33; capacity++)
        for (offset = 0; offset < capacity; offset++)
            for (requested = 0; requested <= capacity + 2; requested++)
                for (edge = 0; edge < 2; edge++)
                {
                    WAVEFORMATEXTENSIBLE fmt;
                    struct directaudio_stream v;
                    float *input = (float *)(src + page + (edge ? page - capacity * 8 : 0));
                    float *output = (float *)(dst + page + (edge ? page - requested * 8 : 0));
                    for (i = 0; i < capacity * 2; i++) input[i] = (float)i;
                    for (i = 0; i < requested * 2; i++) output[i] = 0.5f;
                    setup(&v, &fmt, (BYTE *)input, 0, 2, 48000, capacity, offset, capacity, 0);
                    mix_voice(&v, output, requested);
                    /* Independent direct-index oracle, not the reference implementation. */
                    for (i = 0; i < requested; i++)
                    {
                        float l = i < capacity ? input[((offset + i) % capacity) * 2] + 0.5f : 0.5f;
                        float r = i < capacity ? input[((offset + i) % capacity) * 2 + 1] + 0.5f : 0.5f;
                        assert(output[i * 2] == l && output[i * 2 + 1] == r);
                    }
                    assert(v.held_frames == capacity - min(capacity, requested));
                    assert(v.lcl_offs_frames == (offset + min(capacity, requested)) % capacity);
                    pthread_mutex_destroy(&v.lock);
                    checks++;
                }
    munmap(src, page * 3);
    munmap(dst, page * 3);
}

static void multiple_voices(void)
{
    const unsigned counts[] = {0, 1, 2, 8, 64};
    unsigned c, i, block;
    for (c = 0; c < ARRAY_SIZE(counts); c++)
    {
        struct directaudio_mixer ma = { .lock = PTHREAD_MUTEX_INITIALIZER };
        struct directaudio_mixer mb = { .lock = PTHREAD_MUTEX_INITIALIZER };
        struct directaudio_stream a[64], b[64];
        WAVEFORMATEXTENSIBLE af[64], bf[64];
        _Alignas(float) BYTE buffers[64][31 * 8 * 4];
        float a_out[2 * 40 + 4], b_out[2 * 40 + 4];
        ma.nvoices = mb.nvoices = counts[c];
        for (i = 0; i < counts[c]; i++)
        {
            unsigned j;
            unsigned format = i % 4 ? i % 10 : 0;
            unsigned channels = i % 4 ? i % 8 + 1 : 2;
            unsigned rate = i % 4 && i % 3 == 0 ? 44100 : 48000;
            setup(a + i, af + i, buffers[i], format, channels,
                  rate, 31, i % 31, 31, i % 4);
            setup(b + i, bf + i, buffers[i], format, channels,
                  rate, 31, i % 31, 31, i % 4);
            for (j = 0; j < sizeof(buffers[i]); j++) buffers[i][j] = next_word();
            if (a[i].is_float)
                for (j = 0; j < 31 * af[i].Format.nChannels; j++)
                    ((float *)buffers[i])[j] = (j % 5 - 2.f) * 0.8f;
            ma.voices[i] = a + i;
            mb.voices[i] = b + i;
        }
        for (block = 0; block < 64; block++)
        {
            unsigned requested = block % 41;
            for (i = 0; i < counts[c]; i++)
            {
                a[i].held_frames = b[i].held_frames = block % 31 + 1;
                a[i].playing = b[i].playing = (block + i) % 7 != 0;
            }
            for (i = 0; i < ARRAY_SIZE(a_out); i++) a_out[i] = b_out[i] = 100.f;
            reference_mix_block(&ma, a_out, requested);
            mix_block(&mb, b_out, requested);
            equal_outputs(a_out, b_out, ARRAY_SIZE(a_out));
            for (i = 0; i < counts[c]; i++) equal_state(a + i, b + i);
            checks++;
        }
        for (i = 0; i < counts[c]; i++)
        {
            pthread_mutex_destroy(&a[i].lock);
            pthread_mutex_destroy(&b[i].lock);
        }
        pthread_mutex_destroy(&ma.lock);
        pthread_mutex_destroy(&mb.lock);
    }
}

int main(void)
{
    format_matrix();
#ifdef __aarch64__
    {
        unsigned long saved, mode;
        __asm__ volatile("mrs %0, fpcr" : "=r"(saved));
        for (mode = 0; mode < 4; mode++)
        {
            unsigned long fpcr = (saved & ~((1ul << 24) | (1ul << 25))) |
                                 ((mode & 1) << 24) | ((mode >> 1) << 25);
            __asm__ volatile("msr fpcr, %0; isb" : : "r"(fpcr) : "memory");
            float_edges();
        }
        __asm__ volatile("msr fpcr, %0; isb" : : "r"(saved) : "memory");
    }
#else
    float_edges();
#endif
    guard_pages();
    multiple_voices();
    printf("extended regression: %u cases passed bit-for-bit, including guard-page oracle\n", checks);
    return 0;
}
