#!/usr/bin/env bash
# Retain the exact LGPL baseline used by the mixer regression.
set -euo pipefail
root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
baseline=8cc1227bf1b929521e6b2df2509d8b309c4c4f25
source_file=$(mktemp)
trap 'rm -f "$source_file"' EXIT
git -C "$root_dir" show "$baseline:directaudio.c" > "$source_file"
{
    sed -n '1,/^#if 0/p' "$source_file" | sed '$d'
    for name in samp_to_float downmix_stereo voice_frame_stereo mix_voice mix_block; do
        printf '#define %s reference_%s\n' "$name" "$name"
    done
    sed -n '/^\/\* one source sample of channel/,/^#ifndef DA_RELAY/p' "$source_file" | sed '$d'
    for name in samp_to_float downmix_stereo voice_frame_stereo mix_voice mix_block; do
        printf '#undef %s\n' "$name"
    done
} > "$root_dir/tests/reference_mixer.inc"
