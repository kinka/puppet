// IMA ADPCM 解码 (单声道, 连续流, 初始 predictor=0/index=0, 每字节低 4 位在前)。
// BLE 带宽有限, Mac 端把 16bit PCM 压到 1/4 再发。
#pragma once
#include <stdint.h>
#include <stddef.h>

static const int16_t kAdpcmStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t kAdpcmIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

inline void adpcmDecode(const uint8_t* in, size_t len, int16_t* out) {
  int pred = 0, index = 0;
  for (size_t i = 0; i < len * 2; i++) {
    uint8_t nib = (i & 1) ? (in[i >> 1] >> 4) : (in[i >> 1] & 0x0F);
    int step = kAdpcmStep[index];
    int diff = step >> 3;
    if (nib & 4) diff += step;
    if (nib & 2) diff += step >> 1;
    if (nib & 1) diff += step >> 2;
    pred += (nib & 8) ? -diff : diff;
    if (pred > 32767) pred = 32767;
    if (pred < -32768) pred = -32768;
    index += kAdpcmIndex[nib];
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    out[i] = (int16_t)pred;
  }
}
