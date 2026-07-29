// Minimal WAV writer: mono, 16-bit PCM. Host-only (assumes little-endian).
#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

inline bool writeWavMono16(const char* path, const int16_t* data, size_t n,
                           int sampleRate) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  bool ok = true;
  const uint32_t dataBytes = (uint32_t)(n * 2);
  auto w32 = [&](uint32_t v) { ok &= fwrite(&v, 4, 1, f) == 1; };
  auto w16 = [&](uint16_t v) { ok &= fwrite(&v, 2, 1, f) == 1; };
  ok &= fwrite("RIFF", 1, 4, f) == 4;
  w32(36 + dataBytes);
  ok &= fwrite("WAVE", 1, 4, f) == 4;
  ok &= fwrite("fmt ", 1, 4, f) == 4;
  w32(16);
  w16(1);                              // PCM
  w16(1);                              // mono
  w32((uint32_t)sampleRate);
  w32((uint32_t)sampleRate * 2);       // byte rate
  w16(2);                              // block align
  w16(16);                             // bits per sample
  ok &= fwrite("data", 1, 4, f) == 4;
  w32(dataBytes);
  ok &= fwrite(data, 2, n, f) == n;
  ok &= fclose(f) == 0;
  return ok;
}
