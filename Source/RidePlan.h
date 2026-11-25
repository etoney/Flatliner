#pragma once
// FLATLINER - THE RIDE PLANNED WITH THE WHOLE TAKE IN VIEW (ARA mode)
// When Flatliner is applied to the vocal clip itself (ARA), it can read the whole take before playing it.
// That lets the ride do what a hand on the fader does with the lyric sheet in front of it:
//   - SECTIONS are centred on each moment (the 3 s before AND after), so a chorus gets its level as it
//     starts, not a few seconds in.
//   - Every move is planned forward and backward (zero phase), so the fader is already there when the word
//     starts: no lookahead, no latency.
//   - Gaps and breaths are known: the fader holds through them and glides to the next phrase's level just
//     before it starts, so it never pumps on silence.
// Pure C++ (no JUCE), so it's tested on its own.
#include "FlatlinerEngine.h"
#include <cstdint>
#include <vector>

namespace flat
{
constexpr double kHopSeconds = 0.01;   // the plan has one step every 10 ms

// What the take sounds like: the vocal's level every 10 ms, through the same ears as the live rider
// (120 Hz high-pass, mean square). Fed in chunks, in order.
struct Analysis
{
    double sampleRate = 48000.0;
    int hop = 480;                     // samples per step
    std::int64_t samples = 0;          // length of the take
    std::vector<float> levelDb;        // per step
    float p10 = -100, p50 = -100, p90 = -100, floorDb = -100;   // over the steps with something in them
    double seconds() const { return sampleRate > 0 ? (double) samples / sampleRate : 0.0; }
};

class Analyser
{
public:
    explicit Analyser (double sampleRate)
    {
        a.sampleRate = sampleRate > 1000.0 ? sampleRate : 44100.0;
        a.hop = std::max (1, (int) std::lround (a.sampleRate * kHopSeconds));
        hpCoef = 1.0f - std::exp (-6.2831853f * 120.0f / (float) a.sampleRate);
    }
    // mono samples
    void push (const float* x, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            hp += hpCoef * (x[i] - hp);
            const double y = (double) (x[i] - hp);
            acc += y * y;
            if (++count == a.hop) { raw.push_back ((float) (acc / count)); acc = 0; count = 0; }
        }
        a.samples += n;
    }
    Analysis finish()
    {
        if (count > 0) { raw.push_back ((float) (acc / count)); acc = 0; count = 0; }
        // a word's level: the power over 100 ms centred on each step (so syllables don't each get a ride)
        const int w = 5;
        const size_t N = raw.size();
        std::vector<double> pre (N + 1, 0.0);
        for (size_t k = 0; k < N; ++k) pre[k + 1] = pre[k] + raw[k];
        a.levelDb.assign (N, -100.0f);
        for (size_t k = 0; k < N; ++k)
        {
            const size_t lo = k >= (size_t) w ? k - (size_t) w : 0, hi = std::min (N, k + (size_t) w + 1);
            const double m = (pre[hi] - pre[lo]) / (double) (hi - lo);
            a.levelDb[k] = (float) std::max (-100.0, 10.0 * std::log10 (m + 1.0e-10));
        }
        // the stats The Master reads (same meaning as the live histogram: what the take does)
        std::vector<float> s;
        for (auto v : a.levelDb) if (v > -90.0f) s.push_back (v);
        if (! s.empty())
        {
            std::sort (s.begin(), s.end());
            auto at = [&] (double q) { return s[std::min (s.size() - 1, (size_t) (q * (double) (s.size() - 1)))]; };
            a.p10 = at (0.10); a.p50 = at (0.50); a.p90 = at (0.90); a.floorDb = at (0.02);
        }
        return a;
    }
private:
    Analysis a;
    std::vector<float> raw;
    double acc = 0;
    int count = 0;
    float hp = 0, hpCoef = 0;
};

struct Plan
{
    double sampleRate = 48000.0;
    int hop = 480;
    std::vector<float> gainDb;    // the fader, per step
    std::vector<float> lineDb;    // where it aims (the section level, or TARGET)

    // the fader at a sample of the take (between steps: a straight line)
    float gainAt (std::int64_t sample) const
    {
        if (gainDb.empty()) return 0.0f;
        const double pos = (double) sample / (double) hop - 0.5;
        if (pos <= 0.0) return gainDb.front();
        const size_t k = (size_t) pos;
        if (k + 1 >= gainDb.size()) return gainDb.back();
        const float t = (float) (pos - (double) k);
        return gainDb[k] + t * (gainDb[k + 1] - gainDb[k]);
    }
};

// SPEED: how quickly the planned fader may move. 0% = 250 ms, 100% = 20 ms (each way: it's smoothed
// forward and backward).
inline float plannedSmoothingMs (float speed) { return 250.0f + (20.0f - 250.0f) * std::clamp (speed, 0.0f, 1.0f); }

inline Plan planRide (const Analysis& a, const Params& p)
{
    Plan pl;
    pl.sampleRate = a.sampleRate; pl.hop = a.hop;
    const size_t N = a.levelDb.size();
    pl.gainDb.assign (N, 0.0f);
    pl.lineDb.assign (N, p.targetDb);
    if (N == 0) return pl;

    std::vector<char> on (N, 0);
    for (size_t k = 0; k < N; ++k) on[k] = a.levelDb[k] >= p.gateDb ? 1 : 0;

    // SECTIONS: the vocal's own level over the 3 s before and after (only where it sings). Where those two
    // halves differ a lot, this is where a section changes: the step takes the side it belongs to, so the
    // new section gets its level right from its first word.
    if (p.follow)
    {
        const size_t W = (size_t) std::lround (3.0 / kHopSeconds), L = (size_t) std::lround (0.6 / kHopSeconds),
                     minSteps = (size_t) std::lround (0.5 / kHopSeconds);
        std::vector<double> sum (N + 1, 0.0);
        std::vector<int> cnt (N + 1, 0);
        for (size_t k = 0; k < N; ++k) { sum[k + 1] = sum[k] + (on[k] ? a.levelDb[k] : 0.0); cnt[k + 1] = cnt[k] + on[k]; }
        const float whole = cnt[N] > 0 ? (float) (sum[N] / cnt[N]) : p.targetDb;
        auto mean = [&] (size_t lo, size_t hi, float fallback) { const int c = cnt[hi] - cnt[lo]; return c > 0 ? (float) ((sum[hi] - sum[lo]) / c) : fallback; };
        for (size_t k = 0; k < N; ++k)
        {
            size_t lo = k >= W ? k - W : 0, hi = std::min (N, k + W + 1);
            // not enough singing nearby (an intro, a lone word): widen until there is
            while (cnt[hi] - cnt[lo] < (int) minSteps && (lo > 0 || hi < N)) { lo = lo >= W ? lo - W : 0; hi = std::min (N, hi + W); }
            float line = mean (lo, hi, whole);
            const int nb = cnt[k] - cnt[lo], na = cnt[hi] - cnt[k + 1];
            if (nb >= (int) minSteps && na >= (int) minSteps)
            {
                const float before = mean (lo, k, line), after = mean (k + 1, hi, line);
                if (std::abs (before - after) > 4.0f)
                {
                    const float local = mean (k >= L ? k - L : 0, std::min (N, k + L + 1), line);
                    line = std::abs (local - before) < std::abs (local - after) ? before : after;
                }
            }
            pl.lineDb[k] = line;
        }
    }

    if (! p.ride) return pl;   // RIDE off: the fader stays at 0 dB

    // where the fader wants to be while the vocal sings
    std::vector<float> want (N, 0.0f);
    std::vector<char> has (N, 0);
    // only the body of each word sets the fader: its edges (where the level falls away into a gap) are
    // treated like the gap, so a breath never pulls the fader up
    {
        const size_t E = (size_t) std::lround (0.15 / kHopSeconds);
        for (size_t k = 0; k < N; ++k)
        {
            if (! on[k]) continue;
            float mx = -200.0f;
            for (size_t j = (k >= E ? k - E : 0); j < std::min (N, k + E + 1); ++j) mx = std::max (mx, a.levelDb[j]);
            if (a.levelDb[k] < mx - 4.0f) continue;
            want[k] = std::clamp (pl.lineDb[k] - a.levelDb[k], -p.rangeDb, p.rangeDb); has[k] = 1;
        }
    }

    // the gaps: hold the last word's fader, then glide to the next word's just before it starts. A word's
    // fader is the average over its last (or first) 200 ms, not its very edge.
    const size_t glide = (size_t) std::lround (0.08 / kHopSeconds), span = (size_t) std::lround (0.2 / kHopSeconds);
    auto edgeMean = [&] (size_t from, int dir)
    {
        double sum = 0; size_t c = 0;
        for (long long i = (long long) from; i >= 0 && i < (long long) N && c < span; i += dir)
        {
            if (! has[(size_t) i]) { if (c > 0) break; continue; }
            sum += want[(size_t) i]; ++c;
        }
        return c > 0 ? (float) (sum / (double) c) : 0.0f;
    };
    size_t first = 0; while (first < N && ! has[first]) ++first;
    if (first == N) return pl;
    std::vector<float> filled (want);
    size_t k = 0;
    while (k < N)
    {
        if (has[k]) { ++k; continue; }
        size_t e = k; while (e < N && ! has[e]) ++e;   // gap [k, e)
        const float last = k > 0 ? edgeMean (k - 1, -1) : edgeMean (e, +1);
        const float next = e < N ? edgeMean (e, +1) : last;
        // the next word's onset (its first steps over the gate, before its body): the fader is there by then
        size_t o = e;
        if (e < N) while (o > k && on[o - 1]) --o;
        for (size_t i = k; i < e; ++i)
        {
            if (e < N && i >= o) { filled[i] = next; continue; }
            const size_t toNext = o - i;
            if (e < N && toNext <= glide) { const float t = 1.0f - (float) toNext / (float) (glide + 1); filled[i] = last + t * (next - last); }
            else filled[i] = last;
        }
        k = e;
    }
    want.swap (filled);

    // smooth forward and backward: no lag, and the move is already under way when the word starts
    const float ms = plannedSmoothingMs (p.speed);
    const float c = 1.0f - std::exp (-(float) (kHopSeconds * 1000.0) / ms);
    float s = want[0];
    for (size_t i = 0; i < N; ++i) { s += c * (want[i] - s); pl.gainDb[i] = s; }
    s = pl.gainDb[N - 1];
    for (size_t i = N; i-- > 0;) { s += c * (pl.gainDb[i] - s); pl.gainDb[i] = std::clamp (s, -p.rangeDb, p.rangeDb); }
    return pl;
}
} // namespace flat
