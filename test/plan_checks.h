#pragma once
// Checks for the ride planned with the whole take in view (ARA mode). Pure C++.
#include "dsp_checks.h"
#include "../Source/RidePlan.h"
#include <chrono>

namespace flatcheck
{
inline void runPlanChecks (const Check& check)
{
    const int sec = (int) SR;
    // A take: 20 s of verse around -30 dB, then 20 s of chorus around -18 dB. Words of 0.4 s with 0.15 s
    // between them, a 1 s breath after every 4 words, and each word 5 dB up or down from the last.
    struct Word { int start, end; float amp; };
    std::vector<Word> words;
    {
        const int wordLen = (int) (0.40 * SR), gap = (int) (0.15 * SR), breath = (int) (1.0 * SR);
        const float jitter[6] = { 1.8f, 0.55f, 1.25f, 0.6f, 1.6f, 0.8f };
        int pos = sec / 2, k = 0;
        while (pos + wordLen < 40 * sec)
        {
            const bool chorus = pos >= 20 * sec;
            // a section starts on a fresh phrase
            if (! chorus && pos + wordLen > 20 * sec) { pos = 20 * sec + sec / 4; continue; }
            words.push_back ({ pos, pos + wordLen, (chorus ? 0.18f : 0.045f) * jitter[k % 6] });
            pos += wordLen + (((k + 1) % 4 == 0) ? breath : gap);
            ++k;
        }
    }
    std::vector<float> dry ((size_t) (40 * sec), 0.0f);
    for (auto& w : words) for (int i = w.start; i < w.end; ++i) dry[(size_t) i] = sine (i, w.amp);

    flat::Analyser an (SR);
    for (size_t pos = 0; pos < dry.size(); pos += 4096) an.push (dry.data() + pos, (int) std::min<size_t> (4096, dry.size() - pos));
    const auto a = an.finish();
    check (a.levelDb.size() == 4000 && a.hop == 480 && std::abs (a.seconds() - 40.0) < 0.01,
           fmt ("ARA analysis: one step every 10 ms over the whole take (%d steps)", (int) a.levelDb.size()));

    flat::Params p; p.targetDb = -24.0f; p.rangeDb = 8.0f; p.speed = 0.55f; p.gateDb = -60.0f; p.follow = true; p.ride = true;
    const auto t0 = std::chrono::steady_clock::now();
    const auto plan = flat::planRide (a, p);
    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();

    std::vector<float> y (dry.size());
    for (size_t i = 0; i < y.size(); ++i) y[i] = dry[i] * flat::dbToGain (plan.gainAt ((std::int64_t) i));
    auto wordDb = [&] (const std::vector<float>& v, const Word& w) { return rmsDb (v, w.start + (w.end - w.start) / 4, w.end - (w.end - w.start) / 4); };
    std::vector<size_t> verseW, chorusW;
    for (size_t k = 0; k < words.size(); ++k)
    {
        if (words[k].start > 6 * sec && words[k].end < 19 * sec) verseW.push_back (k);
        if (words[k].start > 26 * sec && words[k].end < 39 * sec) chorusW.push_back (k);
    }
    auto meanDb = [&] (const std::vector<float>& v, const std::vector<size_t>& ws) { double s = 0; for (auto k : ws) s += wordDb (v, words[k]); return (float) (s / (double) ws.size()); };
    auto spread = [&] (const std::vector<float>& v, const std::vector<size_t>& ws)
    {
        float lo = 200, hi = -200;
        for (auto k : ws) { const float d = wordDb (v, words[k]); lo = std::min (lo, d); hi = std::max (hi, d); }
        return hi - lo;
    };
    const float lift = meanDb (y, chorusW) - meanDb (y, verseW);
    check (lift > 10.0f && lift < 13.0f, fmt ("ARA ride: the chorus stays louder than the verse (%.1f dB; it was 12)", lift));
    check (spread (y, chorusW) < 0.5f * spread (dry, chorusW) && spread (y, verseW) < 0.5f * spread (dry, verseW),
           fmt ("ARA ride: the words even out (chorus %.1f dB spread, was %.1f; verse %.1f, was %.1f)",
                spread (y, chorusW), spread (dry, chorusW), spread (y, verseW), spread (dry, verseW)));

    // the chorus gets its level as it starts: its first words already sit with the rest of it
    {
        std::vector<size_t> first;
        for (size_t k = 0; k < words.size() && first.size() < 4; ++k) if (words[k].start >= 20 * sec) first.push_back (k);
        const float d = meanDb (y, first) - meanDb (y, chorusW);
        check (std::abs (d) < 1.5f, fmt ("ARA ride: the chorus's first words already sit at its level (%+.1f dB off)", d));
    }

    // breaths: the fader holds through a gap and doesn't pump
    {
        bool ok = true; float worst = 0;
        for (size_t k = 0; k + 1 < words.size(); ++k)
        {
            const int gs = words[k].end, ge = words[k + 1].start;
            if (ge - gs < sec / 2) continue;
            const float a0 = plan.gainAt (words[k].end - sec / 20), a1 = plan.gainAt (words[k + 1].start + sec / 20);
            const float lo = std::min (a0, a1) - 0.5f, hi = std::max (a0, a1) + 0.5f;
            for (int i = gs; i < ge; i += 240)
            {
                const float g = plan.gainAt (i);
                if (g < lo || g > hi) { if (std::max (lo - g, g - hi) > worst) std::printf ("    gap %.2f-%.2f s at %.2f: %.2f (words %.2f / %.2f)\n", gs / SR, ge / SR, i / SR, g, a0, a1); ok = false; worst = std::max (worst, std::max (lo - g, g - hi)); }
            }
        }
        check (ok, fmt ("ARA ride: through every breath the fader stays between the two phrases' levels (worst %.2f dB outside)", worst));
    }

    // the move is already there when the word starts (no lookahead needed)
    {
        float worst = 0;
        for (size_t k = 1; k < words.size(); ++k)
        {
            if (words[k].start - words[k - 1].end > sec / 2) continue;   // after a short gap
            const float atStart = plan.gainAt (words[k].start + sec / 50), mid = plan.gainAt ((words[k].start + words[k].end) / 2);
            worst = std::max (worst, std::abs (atStart - mid));
        }
        check (worst < 2.5f, fmt ("ARA ride: 20 ms into each word the fader is within %.1f dB of where it rides that word", worst));
    }

    {
        flat::Params off = p; off.ride = false;
        const auto po = flat::planRide (a, off);
        bool zero = true; for (auto v : po.gainDb) zero = zero && v == 0.0f;
        check (zero, "ARA ride: RIDE off leaves the fader at 0 dB");
        flat::Params fixed = p; fixed.follow = false;
        const auto pf = flat::planRide (a, fixed);
        bool line = true; for (auto v : pf.lineDb) line = line && v == fixed.targetDb;
        std::vector<float> yf (dry.size());
        for (size_t i = 0; i < yf.size(); ++i) yf[i] = dry[i] * flat::dbToGain (pf.gainAt ((std::int64_t) i));
        check (line && meanDb (yf, chorusW) - meanDb (yf, verseW) < 5.0f, "ARA ride: SECTIONS off aims everything at TARGET");
        bool inRange = true; for (auto v : plan.gainDb) inRange = inRange && std::abs (v) <= p.rangeDb + 1e-4f;
        check (inRange, "ARA ride: the fader never goes past RANGE");
    }
    check (a.p50 > -40.0f && a.p50 < -14.0f && a.p10 < a.p50 && a.p90 > a.p50, fmt ("ARA analysis: level stats for The Master (p10 %.1f, p50 %.1f, p90 %.1f)", a.p10, a.p50, a.p90));
    check (ms < 50.0, fmt ("ARA ride: planning a 40 s take takes %.1f ms", ms));

    // silence and a take shorter than a step
    {
        flat::Analyser s (SR);
        std::vector<float> z (100, 0.0f); s.push (z.data(), 100);
        const auto sa = s.finish();
        const auto sp = flat::planRide (sa, p);
        check (sa.levelDb.size() == 1 && sp.gainAt (50) == 0.0f, "ARA ride: silence plans no move");
    }
}
} // namespace flatcheck
