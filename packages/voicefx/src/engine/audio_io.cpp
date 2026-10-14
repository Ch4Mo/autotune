#include "audio_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>

#include "lame.h"

#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3/minimp3_ex.h"

namespace vfx {

namespace {

struct FileCloser {
  void operator()(FILE* f) const { if (f) std::fclose(f); }
};
using File = std::unique_ptr<FILE, FileCloser>;

File openFile(const std::string& path, const char* mode) {
  File f(std::fopen(path.c_str(), mode));
  if (!f) throw std::runtime_error("cannot open file: " + path);
  return f;
}

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xff); b.push_back(v >> 8); }
void put32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}

void writeWavInterleaved(const std::string& path, const std::vector<const std::vector<float>*>& chans,
                         double sampleRate, int bits) {
  if (bits != 16 && bits != 24 && bits != 32) throw std::runtime_error("unsupported bit depth");
  const uint16_t nch = static_cast<uint16_t>(chans.size());
  const size_t frames = chans.empty() ? 0 : chans[0]->size();
  const uint16_t bytesPer = static_cast<uint16_t>(bits / 8);
  const uint64_t dataBytes64 = static_cast<uint64_t>(frames) * nch * bytesPer;
  if (dataBytes64 > 0xFFFFFFF0ull - 44) throw std::runtime_error("audio too long for WAV");
  const uint32_t dataBytes = static_cast<uint32_t>(dataBytes64);
  const uint32_t sr = static_cast<uint32_t>(std::lround(sampleRate));

  std::vector<uint8_t> hdr;
  hdr.insert(hdr.end(), {'R', 'I', 'F', 'F'});
  put32(hdr, 36 + dataBytes);
  hdr.insert(hdr.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(hdr, 16);
  put16(hdr, bits == 32 ? 3 : 1);  // 3 = IEEE float
  put16(hdr, nch);
  put32(hdr, sr);
  put32(hdr, sr * nch * bytesPer);
  put16(hdr, static_cast<uint16_t>(nch * bytesPer));
  put16(hdr, static_cast<uint16_t>(bits));
  hdr.insert(hdr.end(), {'d', 'a', 't', 'a'});
  put32(hdr, dataBytes);

  File f = openFile(path, "wb");
  if (std::fwrite(hdr.data(), 1, hdr.size(), f.get()) != hdr.size()) throw std::runtime_error("write failed");

  std::mt19937 rng(12345);
  std::uniform_real_distribution<float> uni(-0.5f, 0.5f);
  std::vector<uint8_t> buf;
  const size_t chunk = 8192;
  for (size_t start = 0; start < frames; start += chunk) {
    const size_t end = std::min(frames, start + chunk);
    buf.clear();
    for (size_t i = start; i < end; ++i) {
      for (const auto* ch : chans) {
        const float v = std::clamp((*ch)[i], -1.0f, 1.0f);
        if (bits == 16) {
          // TPDF dither (+-1 LSB triangular) before truncation to 16 bits.
          const float d = uni(rng) + uni(rng);
          const long q = std::clamp(std::lround(v * 32767.0f + d), -32768L, 32767L);
          put16(buf, static_cast<uint16_t>(static_cast<int16_t>(q)));
        } else if (bits == 24) {
          const long q = std::clamp(std::lround(v * 8388607.0f), -8388608L, 8388607L);
          const uint32_t u = static_cast<uint32_t>(q) & 0xFFFFFF;
          buf.push_back(u & 0xff); buf.push_back((u >> 8) & 0xff); buf.push_back((u >> 16) & 0xff);
        } else {
          uint32_t u;
          std::memcpy(&u, &v, 4);
          put32(buf, u);
        }
      }
    }
    if (std::fwrite(buf.data(), 1, buf.size(), f.get()) != buf.size()) throw std::runtime_error("write failed");
  }
  if (std::fflush(f.get()) != 0) throw std::runtime_error("write failed");
}

}  // namespace

namespace {

// Decodes a WAV into per-channel float buffers.
std::vector<std::vector<float>> decodeWav(const std::string& path, double& sampleRateOut) {
  File f = openFile(path, "rb");
  std::fseek(f.get(), 0, SEEK_END);
  const long fileSize = std::ftell(f.get());
  std::fseek(f.get(), 0, SEEK_SET);
  if (fileSize < 44) throw std::runtime_error("file too small to be a WAV");
  std::vector<uint8_t> data(static_cast<size_t>(fileSize));
  if (std::fread(data.data(), 1, data.size(), f.get()) != data.size()) throw std::runtime_error("read failed");
  if (std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0)
    throw std::runtime_error("not a RIFF/WAVE file");

  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t sampleRate = 0;
  size_t dataOff = 0, dataLen = 0;
  size_t p = 12;
  while (p + 8 <= data.size()) {
    const uint8_t* c = data.data() + p;
    uint32_t len = le32(c + 4);
    const size_t body = p + 8;
    if (std::memcmp(c, "fmt ", 4) == 0 && body + 16 <= data.size()) {
      format = le16(data.data() + body);
      channels = le16(data.data() + body + 2);
      sampleRate = le32(data.data() + body + 4);
      bits = le16(data.data() + body + 14);
      if (format == 0xFFFE && len >= 40 && body + 26 <= data.size()) format = le16(data.data() + body + 24);
    } else if (std::memcmp(c, "data", 4) == 0) {
      dataOff = body;
      // Recorders that were interrupted may leave a 0 / bogus length: clamp to file.
      if (len == 0 || body + len > data.size()) len = static_cast<uint32_t>(data.size() - body);
      dataLen = len;
      break;
    }
    p = body + len + (len & 1);
  }
  if (!channels || !sampleRate || !dataOff) throw std::runtime_error("invalid WAV header");
  const bool isFloat = format == 3;
  if (!(format == 1 || isFloat)) throw std::runtime_error("unsupported WAV encoding");
  const size_t bps = bits / 8;
  if (bps == 0 || (isFloat && bps != 4 && bps != 8) || (!isFloat && bps > 4))
    throw std::runtime_error("unsupported WAV bit depth");

  const size_t frames = dataLen / (bps * channels);
  sampleRateOut = sampleRate;
  std::vector<std::vector<float>> out(channels, std::vector<float>(frames));
  const uint8_t* src = data.data() + dataOff;
  for (size_t i = 0; i < frames; ++i) {
    for (uint16_t ch = 0; ch < channels; ++ch) {
      const uint8_t* s = src + (i * channels + ch) * bps;
      double v = 0.0;
      if (isFloat) {
        if (bps == 4) { float t; std::memcpy(&t, s, 4); v = t; }
        else { double t; std::memcpy(&t, s, 8); v = t; }
      } else if (bps == 1) {
        v = (static_cast<int>(s[0]) - 128) / 128.0;
      } else if (bps == 2) {
        v = static_cast<int16_t>(le16(s)) / 32768.0;
      } else if (bps == 3) {
        int32_t t = static_cast<int32_t>(s[0] | (s[1] << 8) | (s[2] << 16));
        if (t & 0x800000) t |= ~0xFFFFFF;
        v = t / 8388608.0;
      } else {
        v = static_cast<int32_t>(le32(s)) / 2147483648.0;
      }
      out[ch][i] = static_cast<float>(v);
    }
  }
  return out;
}

bool hasWavMagic(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) throw std::runtime_error("cannot open file: " + path);
  char h[12] = {0};
  const size_t n = std::fread(h, 1, 12, f);
  std::fclose(f);
  return n == 12 && std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WAVE", 4) == 0;
}

}  // namespace

MonoAudio readWav(const std::string& path) {
  MonoAudio out;
  const auto chans = decodeWav(path, out.sampleRate);
  const size_t frames = chans.empty() ? 0 : chans[0].size();
  out.samples.assign(frames, 0.0f);
  const float k = 1.0f / static_cast<float>(chans.size());
  for (const auto& c : chans)
    for (size_t i = 0; i < frames; ++i) out.samples[i] += c[i] * k;
  return out;
}

StereoAudio readAudioFile(const std::string& path) {
  StereoAudio out;
  if (hasWavMagic(path)) {
    auto chans = decodeWav(path, out.sampleRate);
    out.samples.l = std::move(chans[0]);
    out.samples.r = chans.size() > 1 ? std::move(chans[1]) : out.samples.l;
    return out;
  }
  mp3dec_t dec;
  mp3dec_file_info_t info;
  std::memset(&info, 0, sizeof(info));
  const int rc = mp3dec_load(&dec, path.c_str(), &info, nullptr, nullptr);
  struct Free { mp3dec_file_info_t& i; ~Free() { std::free(i.buffer); } } guard{info};
  if (rc != 0 || info.samples == 0 || info.channels <= 0 || info.hz <= 0)
    throw std::runtime_error("unsupported audio file (use MP3 or WAV)");
  const size_t ch = static_cast<size_t>(info.channels);
  const size_t frames = info.samples / ch;
  out.sampleRate = info.hz;
  out.samples = Stereo(frames);
  for (size_t i = 0; i < frames; ++i) {
    out.samples.l[i] = info.buffer[i * ch];
    out.samples.r[i] = info.buffer[i * ch + (ch > 1 ? 1 : 0)];
  }
  return out;
}



void writeWav(const std::string& path, const Stereo& s, double sampleRate, int bits) {
  writeWavInterleaved(path, {&s.l, &s.r}, sampleRate, bits);
}

void writeWavMono(const std::string& path, const std::vector<float>& x, double sampleRate, int bits) {
  writeWavInterleaved(path, {&x}, sampleRate, bits);
}

void writeMp3(const std::string& path, const Stereo& s, double sampleRate, int kbps) {
  lame_t gf = lame_init();
  if (!gf) throw std::runtime_error("lame_init failed");
  struct Guard { lame_t g; ~Guard() { lame_close(g); } } guard{gf};

  lame_set_in_samplerate(gf, static_cast<int>(std::lround(sampleRate)));
  lame_set_num_channels(gf, 2);
  lame_set_mode(gf, JOINT_STEREO);
  lame_set_VBR(gf, vbr_off);
  lame_set_brate(gf, kbps);
  lame_set_quality(gf, 2);  // 2 = near-best psychoacoustic quality
  lame_set_bWriteVbrTag(gf, 1);  // Xing/LAME tag: exact duration + gapless info
  if (lame_init_params(gf) < 0) throw std::runtime_error("invalid MP3 encoder settings");

  File f = openFile(path, "wb+");
  const int chunk = 4096;
  std::vector<unsigned char> mp3(static_cast<size_t>(1.25 * chunk + 7200));
  for (size_t start = 0; start < s.size(); start += chunk) {
    const int n = static_cast<int>(std::min<size_t>(chunk, s.size() - start));
    const int bytes = lame_encode_buffer_ieee_float(gf, s.l.data() + start, s.r.data() + start, n,
                                                    mp3.data(), static_cast<int>(mp3.size()));
    if (bytes < 0) throw std::runtime_error("MP3 encoding failed");
    if (std::fwrite(mp3.data(), 1, static_cast<size_t>(bytes), f.get()) != static_cast<size_t>(bytes))
      throw std::runtime_error("write failed");
  }
  const int bytes = lame_encode_flush(gf, mp3.data(), static_cast<int>(mp3.size()));
  if (bytes < 0) throw std::runtime_error("MP3 encoding failed");
  if (std::fwrite(mp3.data(), 1, static_cast<size_t>(bytes), f.get()) != static_cast<size_t>(bytes))
    throw std::runtime_error("write failed");
  std::fflush(f.get());
  lame_mp3_tags_fid(gf, f.get());
  if (std::fflush(f.get()) != 0) throw std::runtime_error("write failed");
}

}  // namespace vfx
