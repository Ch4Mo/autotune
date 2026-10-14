#include "effects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>

#include "dsp.hpp"

namespace vfx {

namespace {

// out[n] = max (or min) of in[n .. n+window], O(N) with a monotonic deque.
std::vector<float> slidingExtreme(const std::vector<float>& in, size_t window, bool wantMax) {
  const size_t n = in.size();
  std::vector<float> out(n);
  std::deque<size_t> dq;
  auto better = [&](float a, float b) { return wantMax ? a >= b : a <= b; };
  size_t next = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t end = std::min(n - 1, i + window);
    while (next <= end) {
      while (!dq.empty() && better(in[next], in[dq.back()])) dq.pop_back();
      dq.push_back(next);
      ++next;
    }
    while (dq.front() < i) dq.pop_front();
    out[i] = in[dq.front()];
  }
  return out;
}

}  // namespace

DelayLine::DelayLine(size_t maxDelay) {
  size_t n = 1;
  while (n < maxDelay + 1) n <<= 1;
  buf_.assign(n, 0.0f);
  mask_ = n - 1;
}

void noiseGate(std::vector<float>& x, double sr, float thresholdDb, float rangeDb) {
  if (x.empty()) return;
  EnvelopeFollower env;
  env.setup(sr, 0.5, 60.0);
  std::vector<float> g(x.size());
  const float floorGain = dbToGain(-rangeDb);
  const float thrLin = dbToGain(thresholdDb);
  for (size_t i = 0; i < x.size(); ++i) {
    const float e = env.process(std::fabs(x[i]));
    if (e >= thrLin) { g[i] = 1.0f; continue; }
    const float under = thresholdDb - gainToDb(e);
    // 1:3 downward expansion below the threshold, limited to `rangeDb`.
    const float red = under > 0.0f ? std::min(rangeDb, under * 2.0f) : 0.0f;
    g[i] = std::max(floorGain, dbToGain(-red));
  }
  // Open 8 ms ahead of the signal so consonant onsets are never chopped,
  // then smooth the gain (fast open, slow close).
  g = slidingExtreme(g, static_cast<size_t>(0.008 * sr), true);
  EnvelopeFollower sm;
  sm.setup(sr, 1.0, 120.0);
  sm.reset(g[0]);
  for (size_t i = 0; i < x.size(); ++i) x[i] *= sm.process(g[i]);
}

void deEss(Stereo& s, double sr, float amount) {
  if (amount <= 0.0f || s.size() == 0) return;
  Biquad scL, scR, hpL, hpR;
  scL.setup(Biquad::Type::BandPass, sr, 7000.0, 0.9);
  scR.setup(Biquad::Type::BandPass, sr, 7000.0, 0.9);
  hpL.setup(Biquad::Type::HighPass, sr, 4500.0, 0.7071);
  hpR.setup(Biquad::Type::HighPass, sr, 4500.0, 0.7071);
  EnvelopeFollower sib, full, gainSm;
  sib.setup(sr, 0.5, 40.0);
  full.setup(sr, 0.5, 40.0);
  gainSm.setup(sr, 1.0, 50.0);
  gainSm.reset(1.0f);
  const float maxRed = 4.0f + 10.0f * amount;  // dB
  const float relThr = dbToGain(-9.0f), absThr = dbToGain(-50.0f);
  for (size_t i = 0; i < s.size(); ++i) {
    const float l = s.l[i], r = s.r[i];
    const float band = sib.process(std::max(std::fabs(scL.process(l)), std::fabs(scR.process(r))));
    const float fb = full.process(std::max(std::fabs(l), std::fabs(r)));
    // Sibilance: the 7 kHz band gets within 9 dB of the full-band level.
    float red = 0.0f;
    if (band > fb * relThr && band > absThr) {
      const float excess = gainToDb(band) - (gainToDb(fb) - 9.0f);
      red = std::min(maxRed, excess * 1.5f * (0.5f + amount));
    }
    const float g = gainSm.process(dbToGain(-red));
    const float hl = hpL.process(l), hr = hpR.process(r);
    s.l[i] = l + (g - 1.0f) * hl;
    s.r[i] = r + (g - 1.0f) * hr;
  }
}

void vocalEq(Stereo& s, double sr, float lowCutHz, float warmthDb, float presenceDb, float airDb) {
  for (auto* ch : {&s.l, &s.r}) {
    Biquad hp1, hp2, warm, pres, air;
    hp1.setup(Biquad::Type::HighPass, sr, lowCutHz, 0.5411961);
    hp2.setup(Biquad::Type::HighPass, sr, lowCutHz, 1.3065630);
    warm.setup(Biquad::Type::Peak, sr, 250.0, 0.8, warmthDb);
    pres.setup(Biquad::Type::Peak, sr, 3500.0, 0.9, presenceDb);
    air.setup(Biquad::Type::HighShelf, sr, 10000.0, 0.7071, airDb);
    for (auto& v : *ch) v = air.process(pres.process(warm.process(hp2.process(hp1.process(v)))));
  }
}

void compress(Stereo& s, double sr, float thresholdDb, float ratio, float attackMs, float releaseMs) {
  if (ratio <= 1.0f) return;
  const float knee = 6.0f;
  EnvelopeFollower det;
  det.setup(sr, attackMs, releaseMs);
  const float slope = 1.0f - 1.0f / ratio;
  const float kneeStart = dbToGain(thresholdDb - knee * 0.5f);
  for (size_t i = 0; i < s.size(); ++i) {
    const float e = det.process(std::max(std::fabs(s.l[i]), std::fabs(s.r[i])));
    if (e <= kneeStart) continue;
    const float lv = gainToDb(e);
    const float over = lv - thresholdDb;
    float red;
    if (over <= -knee * 0.5f) red = 0.0f;
    else if (over >= knee * 0.5f) red = slope * over;
    else red = slope * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);
    const float g = dbToGain(-red);
    s.l[i] *= g;
    s.r[i] *= g;
  }
}

PingPongDelay::PingPongDelay(double sr, float timeMs, float feedback, float toneHz)
    : d_(std::max<size_t>(1, static_cast<size_t>(timeMs * 0.001 * sr))),
      fb_(std::clamp(feedback, 0.0f, 0.85f)),
      l_(d_ + 1),
      r_(d_ + 1) {
  toneL_.setup(Biquad::Type::LowPass, sr, toneHz, 0.7071);
  toneR_.setup(Biquad::Type::LowPass, sr, toneHz, 0.7071);
  hp_.setup(Biquad::Type::HighPass, sr, 250.0, 0.7071);
}

void PingPongDelay::process(float inL, float inR, float& outL, float& outR) {
  const float yl = l_.read(d_), yr = r_.read(d_);
  l_.write(hp_.process(0.5f * (inL + inR)) + fb_ * toneR_.process(yr));
  r_.write(fb_ * toneL_.process(yl));
  outL = yl;
  outR = yr;
}

FdnReverb::FdnReverb(double sr, float size, float decaySeconds, float damping, float predelayMs)
    : preDelay_(std::max<size_t>(1, static_cast<size_t>(predelayMs * 0.001 * sr))) {
  static const std::array<double, kLines> kBase{1103, 1327, 1523, 1789, 2003, 2269, 2503, 2777};
  const double scale = (0.45 + 1.1 * std::clamp(size, 0.0f, 1.0f)) * sr / 48000.0;
  for (int i = 0; i < kLines; ++i) {
    len_[i] = std::max<size_t>(8, static_cast<size_t>(kBase[i] * scale));
    g_[i] = static_cast<float>(std::pow(10.0, -3.0 * len_[i] / (sr * std::max(0.1f, decaySeconds))));
    lines_[i] = DelayLine(len_[i] + 1);
  }
  damp_ = 0.05f + 0.6f * std::clamp(damping, 0.0f, 1.0f);
  const double ds = sr / 48000.0;
  const std::pair<double, float> diff[4] = {{142, 0.7f}, {107, 0.7f}, {379, 0.65f}, {277, 0.65f}};
  for (const auto& d : diff) {
    const size_t len = std::max<size_t>(1, static_cast<size_t>(d.first * ds));
    diffusers_.push_back(Allpass{DelayLine(len + 1), len, d.second});
  }
  pre_ = DelayLine(preDelay_ + 1);
  inHp_.setup(Biquad::Type::HighPass, sr, 180.0, 0.7071);
  inLp_.setup(Biquad::Type::LowPass, sr, 9000.0, 0.7071);
}

void FdnReverb::process(float inL, float inR, float& outL, float& outR) {
  float x = inLp_.process(inHp_.process(0.5f * (inL + inR)));
  pre_.write(x);
  x = pre_.read(preDelay_);
  for (auto& a : diffusers_) x = a.process(x);

  std::array<float, kLines> o{}, v{};
  for (int i = 0; i < kLines; ++i) {
    const float raw = lines_[i].read(len_[i]);
    lp_[i] = (1.0f - damp_) * raw + damp_ * lp_[i];
    o[i] = raw;
    v[i] = lp_[i] * g_[i];
  }
  // Fast 8-point Hadamard mix (orthogonal after 1/sqrt(8) scaling).
  for (int h = 1; h < kLines; h <<= 1)
    for (int i = 0; i < kLines; i += h << 1)
      for (int j = i; j < i + h; ++j) {
        const float a = v[j], b = v[j + h];
        v[j] = a + b;
        v[j + h] = a - b;
      }
  const float norm = 0.35355339f;
  for (int i = 0; i < kLines; ++i) lines_[i].write(v[i] * norm + ((i & 1) ? -x : x) * 0.5f);

  outL = 0.3f * (o[0] - o[2] + o[4] - o[6] + 0.5f * (o[1] + o[5]));
  outR = 0.3f * (o[1] - o[3] + o[5] - o[7] + 0.5f * (o[2] + o[6]));
}

double integratedLoudness(const Stereo& s, double sr) {
  // K-weighting filter (pre-filter shelf + RLB high-pass), any sample rate.
  auto makeFilters = [&](Biquad& shelf, Biquad& hp) {
    {
      const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
      const double K = std::tan(kPi * f0 / sr);
      const double Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
      const double a0 = 1.0 + K / Q + K * K;
      shelf.setCoefficients((Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0,
                            (Vh - Vb * K / Q + K * K) / a0, 2.0 * (K * K - 1.0) / a0,
                            (1.0 - K / Q + K * K) / a0);
    }
    {
      const double f0 = 38.13547087602444, Q = 0.5003270373238773;
      const double K = std::tan(kPi * f0 / sr);
      const double a0 = 1.0 + K / Q + K * K;
      hp.setCoefficients(1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0);
    }
  };
  Biquad shL, hpL, shR, hpR;
  makeFilters(shL, hpL);
  makeFilters(shR, hpR);

  // 100 ms segments; 400 ms gating blocks = 4 consecutive segments (75 % overlap).
  const size_t seg = static_cast<size_t>(0.1 * sr);
  if (seg == 0 || s.size() < 4 * seg) return -120.0;
  std::vector<double> segPower;
  segPower.reserve(s.size() / seg + 1);
  double acc = 0.0;
  size_t inSeg = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const double l = hpL.process(shL.process(s.l[i]));
    const double r = hpR.process(shR.process(s.r[i]));
    acc += l * l + r * r;
    if (++inSeg == seg) { segPower.push_back(acc); acc = 0.0; inSeg = 0; }
  }
  std::vector<double> z;
  for (size_t k = 0; k + 4 <= segPower.size(); ++k)
    z.push_back((segPower[k] + segPower[k + 1] + segPower[k + 2] + segPower[k + 3]) / (4.0 * seg));

  auto lufs = [](double ms) { return -0.691 + 10.0 * std::log10(std::max(ms, 1e-20)); };
  double sum = 0; size_t cnt = 0;
  for (double v : z) if (lufs(v) > -70.0) { sum += v; ++cnt; }
  if (cnt == 0) return -120.0;
  const double rel = lufs(sum / cnt) - 10.0;
  sum = 0; cnt = 0;
  for (double v : z) if (lufs(v) > -70.0 && lufs(v) > rel) { sum += v; ++cnt; }
  return cnt ? lufs(sum / cnt) : -120.0;
}

void limit(Stereo& s, double sr, float ceilingDb, float releaseMs) {
  const size_t n = s.size();
  if (n == 0) return;
  const float ceiling = dbToGain(ceilingDb);
  std::vector<float> req(n);
  for (size_t i = 0; i < n; ++i) {
    const float pk = std::max(std::fabs(s.l[i]), std::fabs(s.r[i]));
    req[i] = pk > ceiling ? ceiling / pk : 1.0f;
  }
  const size_t la = std::max<size_t>(1, static_cast<size_t>(0.002 * sr));  // 2 ms look-ahead
  std::vector<float> g = slidingExtreme(req, la, false);
  std::vector<float>().swap(req);
  // Release smoothing (instant attack: g is already the look-ahead minimum).
  const float rel = EnvelopeFollower::coef(sr, releaseMs);
  float cur = 1.0f;
  for (size_t i = 0; i < n; ++i) {
    cur = g[i] < cur ? g[i] : rel * cur + (1.0f - rel) * g[i];
    g[i] = cur;
  }
  // Backward-looking box filter of length la+1: keeps gain <= required at
  // every peak while removing the attack step (no clicks).
  std::vector<float> ring(la + 1, 1.0f);
  double acc = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t slot = i % (la + 1);
    if (i >= la + 1) acc -= ring[slot];
    ring[slot] = g[i];
    acc += g[i];
    const size_t cnt = std::min(i + 1, la + 1);
    const float gain = static_cast<float>(acc / cnt);
    s.l[i] = std::clamp(s.l[i] * gain, -ceiling, ceiling);
    s.r[i] = std::clamp(s.r[i] * gain, -ceiling, ceiling);
  }
}

}  // namespace vfx
