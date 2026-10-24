/*
 * Unixlib for the DirectAudio driver (native Wine -> Android AAudio).
 *
 * mmdevapi owns IAudioClient/IAudioRenderClient; this backend only implements
 * the mmdevapi driver unixlib vtable (see ../mmdevapi/unixlib.h) against AAudio,
 * with no PulseAudio daemon and no ALSA aserver in the path.
 *
 * Structure and the mmdevapi ring-buffer bookkeeping are modelled on
 * winecoreaudio.drv (single native backend, render callback ~ AAudio callback);
 * the CoreAudio device layer is replaced with AAudio.
 *
 * Copyright (C) 2026 The412Banner <205237651+The412Banner@users.noreply.github.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#define samp_to_float reference_samp_to_float
#define downmix_stereo reference_downmix_stereo
#define voice_frame_stereo reference_voice_frame_stereo
#define mix_voice reference_mix_voice
#define mix_block reference_mix_block
/* one source sample of channel c -> float in [-1,1] */
static inline float samp_to_float(const struct directaudio_stream *v, const BYTE *frame, int c)
{
    if (v->is_float)
        return ((const float *)frame)[c];

    switch (v->fmt->wBitsPerSample)
    {
    case 16: return ((const INT16 *)frame)[c] * (1.0f / 32768.0f);
    case 32: return ((const INT32 *)frame)[c] * (1.0f / 2147483648.0f);
    case 8:  return (((const BYTE *)frame)[c] - 128) * (1.0f / 128.0f);
    case 24:
    {
        const BYTE *p = frame + c * 3;
        INT32 s = p[0] | (p[1] << 8) | (p[2] << 16);
        if (s & 0x800000) s |= ~0xffffff;
        return s * (1.0f / 8388608.0f);
    }
    default: return 0.0f;
    }
}

/* down/up-mix an N-channel source frame to stereo. WAVEFORMATEX channel order:
 * mono; L R; 5.1 = FL FR C LFE BL BR; 7.1 = FL FR C LFE BL BR SL SR. */
static inline void downmix_stereo(int ch, const float *s, float *L, float *R)
{
    const float c = 0.7071f;
    switch (ch)
    {
    case 1:  *L = *R = s[0]; return;
    case 2:  *L = s[0]; *R = s[1]; return;
    case 3:  *L = s[0]; *R = s[1]; return;                    /* 2.1: drop LFE */
    case 4:  *L = s[0] + c*s[2]; *R = s[1] + c*s[3]; return;  /* quad */
    case 6:  *L = s[0] + c*s[2] + c*s[4];                     /* 5.1 */
             *R = s[1] + c*s[2] + c*s[5]; return;
    case 8:  *L = s[0] + c*s[2] + c*s[4] + c*s[6];            /* 7.1 */
             *R = s[1] + c*s[2] + c*s[5] + c*s[7]; return;
    default: *L = s[0]; *R = (ch > 1) ? s[1] : s[0]; return;
    }
}

/* read the voice frame at (lcl_offs+idx), down-mixed to stereo float */
static inline void voice_frame_stereo(struct directaudio_stream *v, UINT32 idx,
                                      float *L, float *R)
{
    UINT32 pos = (v->lcl_offs_frames + idx) % v->bufsize_frames;
    const BYTE *frame = v->local_buffer + (size_t)pos * v->fmt->nBlockAlign;
    int ch = v->fmt->nChannels, c;
    float s[8];

    if (ch > 8) ch = 8;
    for (c = 0; c < ch; c++)
    {
        float val = samp_to_float(v, frame, c);
        if (v->vols_active) val *= v->vols[c < 8 ? c : 7];
        s[c] = val;
    }
    downmix_stereo(ch, s, L, R);
}

/* sum one voice into the stereo float mix buffer (out holds numFrames*2 floats) */
static void mix_voice(struct directaudio_stream *v, float *out, int32_t numFrames)
{
    pthread_mutex_lock(&v->lock);

    if (!v->playing || v->held_frames == 0)
    {
        pthread_mutex_unlock(&v->lock);
        return;
    }

    if (v->aa_rate == MIX_OUT_RATE)
    {
        UINT32 n = min((UINT32)numFrames, v->held_frames), f;
        for (f = 0; f < n; f++)
        {
            float L, R;
            voice_frame_stereo(v, f, &L, &R);
            out[2*f]     += L;
            out[2*f + 1] += R;
        }
        v->lcl_offs_frames = (v->lcl_offs_frames + n) % v->bufsize_frames;
        v->held_frames -= n;
    }
    else /* linear resample voice rate -> 48 kHz */
    {
        double ratio = (double)v->aa_rate / MIX_OUT_RATE;
        int32_t f, consumed;
        for (f = 0; f < numFrames; f++)
        {
            UINT32 i0 = (UINT32)v->rs_pos;
            float L0, R0, L1, R1;
            double frac;

            if (i0 + 1 >= v->held_frames) break;   /* ran dry - remaining stays silent */
            voice_frame_stereo(v, i0, &L0, &R0);
            voice_frame_stereo(v, i0 + 1, &L1, &R1);
            frac = v->rs_pos - i0;
            out[2*f]     += (float)(L0 + (L1 - L0) * frac);
            out[2*f + 1] += (float)(R0 + (R1 - R0) * frac);
            v->rs_pos += ratio;
        }
        consumed = (int32_t)v->rs_pos;
        if (consumed > (int32_t)v->held_frames) consumed = v->held_frames;
        if (consumed > 0)
        {
            v->lcl_offs_frames = (v->lcl_offs_frames + consumed) % v->bufsize_frames;
            v->held_frames -= consumed;
            v->rs_pos -= consumed;
        }
        if (v->held_frames == 0) v->rs_pos = 0.0;  /* resync fraction on underrun */
    }

    pthread_mutex_unlock(&v->lock);
}

/* Produce numFrames of the mixed output: sum every voice, then the limiter.
 * Called from the AAudio data callback in the in-process build and from the
 * relay pump thread in the relay build; the samples are identical either way. */
static void mix_block(struct directaudio_mixer *mx, float *out, int32_t numFrames)
{
    int32_t i, n2 = numFrames * MIX_OUT_CHANNELS;

    memset(out, 0, (size_t)n2 * sizeof(float));

    pthread_mutex_lock(&mx->lock);
    for (i = 0; i < mx->nvoices; i++)
        mix_voice(mx->voices[i], out, numFrames);
    pthread_mutex_unlock(&mx->lock);

    /* Soft knee instead of a hard clip. Two things push the mix past full scale:
     * several voices summing, and the multichannel fold in downmix_stereo(), which
     * is a correct ITU-style fold (centre and surrounds at -3 dB) but carries NO
     * headroom - a 5.1 source peaks at 1 + 0.7071 + 0.7071 = 2.41x. Hard clipping
     * that is the audible failure: flat tops, and the harmonics they generate.
     *
     * Below DA_KNEE the signal is untouched, so ordinary material is bit-identical
     * and nothing gets quieter - the alternative, scaling every fold down by 2.41x,
     * would cost 7.6 dB on all surround content to protect a peak it rarely hits.
     * Above the knee it compresses along u/(1+u), which is C1-continuous at the
     * knee and asymptotic to 1.0, so it can never wrap or overshoot. Two multiplies
     * and a divide on the samples that need it, none on the ones that do not. */
    for (i = 0; i < n2; i++)
    {
        float x = out[i];
        float a = x < 0.0f ? -x : x;

        if (a > DA_KNEE)
        {
            float u = (a - DA_KNEE) / (1.0f - DA_KNEE);
            float y = DA_KNEE + (1.0f - DA_KNEE) * (u / (1.0f + u));
            out[i] = x < 0.0f ? -y : y;
        }
    }
}

#undef samp_to_float
#undef downmix_stereo
#undef voice_frame_stereo
#undef mix_voice
#undef mix_block
