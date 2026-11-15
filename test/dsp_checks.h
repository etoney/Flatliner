#pragma once
// Pure C++ DSP checks for the Flatliner engine (no JUCE needed, so they also run on a bare compiler).
#include "../Source/FlatlinerEngine.h"
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

namespace flatcheck
{
using Check = std::function<void (bool, const std::string&)>;
using Gen = std::function<float (int)>;

inline std::string fmt (const char* f, ...)
{
    char buf[400];
    va_list a; va_start (a, f); std::vsnprintf (buf, sizeof buf, f, a); va_end (a);
    return buf;
}

constexpr double SR = 48000.0;
inline float sine (int i, float amp, float freq = 1000.0f) { return amp * (float) std::sin (6.283185307179586 * freq * (double) i / SR); }
inline float dbOf (float x) { return 20.0f * std::log10 (std::max (x, 1.0e-9f)); }

inline std::vector<float> render (flat::Engine& e, int total, const Gen& gen, const Gen& scGen = {})
{
    std::vector<float> out ((size_t) total), L (512), R (512), S (512);
    for (int pos = 0; pos < total; pos += 512)
    {
        const int n = std::min (512, total - pos);
        for (int i = 0; i < n; ++i) { L[(size_t) i] = R[(size_t) i] = gen (pos + i); S[(size_t) i] = scGen ? scGen (pos + i) : 0.0f; }
        float* ch[2] = { L.data(), R.data() };
        const float* sc[1] = { S.data() };
        e.process (ch, 2, scGen ? sc : nullptr, scGen ? 1 : 0, n);
        std::copy (L.begin(), L.begin() + n, out.begin() + pos);
    }
    return out;
}

inline float rmsDb (const std::vector<float>& v, int a, int b)
{
    double s = 0; for (int i = a; i < b; ++i) s += (double) v[(size_t) i] * v[(size_t) i];
    return 10.0f * std::log10 ((float) (s / (b - a)) + 1.0e-12f);
}
inline float peakOf (const std::vector<float>& v, int a, int b)
{
    float m = 0; for (int i = a; i < b; ++i) m = std::max (m, std::abs (v[(size_t) i])); return m;
}

inline std::unique_ptr<flat::Engine> make (const flat::Params& p)
{
    auto e = std::make_unique<flat::Engine>();
    e->prepare (SR, 512);
    e->setParams (p, true);
    return e;
}

inline void runEngineChecks (const Check& check)
{
    const int sec = (int) SR;

    // 1) it levels: quiet and loud passages both land on the TARGET
    {
        flat::Params p; p.targetDb = -19.0f; p.rangeDb = 12.0f; p.speed = 1.0f; p.lookaheadMs = 0.0f;
        auto e = make (p);
        const float quiet = 0.05f, loud = 0.5f;   // about -29 dB and -9 dB RMS
        auto y = render (*e, 8 * sec, [&] (int i) { return sine (i, ((i / (2 * sec)) % 2 == 0) ? quiet : loud); });
        const float q1 = rmsDb (y, (int) (1.6 * sec), 2 * sec), l1 = rmsDb (y, (int) (3.6 * sec), 4 * sec);
        const float q2 = rmsDb (y, (int) (5.6 * sec), 6 * sec), l2 = rmsDb (y, (int) (7.6 * sec), 8 * sec);
        check (std::abs (q1 + 19.0f) < 0.6f && std::abs (q2 + 19.0f) < 0.6f, fmt ("quiet passages ridden up to the target: %.2f / %.2f dB (target -19)", q1, q2));
        check (std::abs (l1 + 19.0f) < 0.6f && std::abs (l2 + 19.0f) < 0.6f, fmt ("loud passages ridden down to the target: %.2f / %.2f dB (target -19)", l1, l2));
    }

    // 2) RIDE off = a straight wire
    {
        flat::Params p; p.ride = false; p.lookaheadMs = 0.0f;
        auto e = make (p);
        auto y = render (*e, sec, [&] (int i) { return sine (i, 0.3f, 440.0f); });
        float err = 0; for (int i = 0; i < sec; ++i) err = std::max (err, std::abs (y[(size_t) i] - sine (i, 0.3f, 440.0f)));
        check (err < 1.0e-5f, fmt ("RIDE off is transparent (max error %.2g)", err));
    }

    // 3) RANGE limits the fader
    {
        flat::Params p; p.targetDb = -30.0f; p.rangeDb = 3.0f; p.speed = 1.0f; p.lookaheadMs = 0.0f;
        auto e = make (p);
        render (*e, 3 * sec, [&] (int i) { return sine (i, 0.5f); });
        const float f = e->faderDb.load();
        check (std::abs (f + 3.0f) < 0.05f, fmt ("fader stops at the range limit: %.2f dB (range 3)", f));
    }

    // 4) GATE: noise below the gate is left alone, without a gate it gets pushed up
    {
        flat::Params p; p.targetDb = -18.0f; p.rangeDb = 12.0f; p.speed = 1.0f; p.lookaheadMs = 0.0f;
        const float hiss = 0.0004f;   // about -71 dB RMS
        p.gateDb = -55.0f; auto closed = make (p);
        render (*closed, 4 * sec, [&] (int i) { return sine (i, hiss); });
        p.gateDb = -80.0f; auto open = make (p);
        render (*open, 4 * sec, [&] (int i) { return sine (i, hiss); });
        check (std::abs (closed->faderDb.load()) < 0.01f, fmt ("gate closed: fader holds at %.3f dB", closed->faderDb.load()));
        check (open->faderDb.load() > 11.5f, fmt ("gate open: the same noise gets ridden up to %.2f dB", open->faderDb.load()));
    }

    // 5) LOOKAHEAD: the audio is delayed by exactly the reported latency
    {
        flat::Params p; p.ride = false; p.lookaheadMs = 10.0f;
        auto e = make (p);
        check (e->getLatency() == 480 && e->latencyFor (0.0f) == 0 && e->latencyFor (50.0f) == 2400, fmt ("latency: 10 ms = %d samples, 50 ms = %d samples", e->getLatency(), e->latencyFor (50.0f)));
        auto y = render (*e, 3000, [] (int i) { return i == 1000 ? 1.0f : 0.0f; });
        int at = 0; for (int i = 0; i < 3000; ++i) if (std::abs (y[(size_t) i]) > 0.5f) at = i;
        check (at == 1000 + 480, fmt ("impulse comes out %d samples late", at - 1000));
    }

    // 6) LOOKAHEAD does its job: a sudden shout is caught before it reaches the output
    {
        auto burst = [&] (float lookMs)
        {
            flat::Params p; p.targetDb = -19.0f; p.rangeDb = 12.0f; p.speed = 1.0f; p.lookaheadMs = lookMs;
            auto e = make (p);
            auto y = render (*e, 2 * sec, [&] (int i) { return i < sec ? sine (i, 0.03f) : sine (i, 0.5f); });
            return peakOf (y, sec, sec + (int) (0.3 * SR));
        };
        const float noLook = burst (0.0f), look = burst (20.0f);
        check (dbOf (noLook) - dbOf (look) > 3.0f, fmt ("a sudden shout peaks at %.1f dBFS without lookahead, %.1f dBFS with 20 ms", dbOf (noLook), dbOf (look)));
    }

    // 7) MUSIC: louder backing track raises the vocal target
    {
        flat::Params p; p.targetDb = -25.0f; p.rangeDb = 12.0f; p.speed = 1.0f; p.lookaheadMs = 0.0f;
        const float vox = 0.1f;   // about -23 dB RMS
        auto e0 = make (p);
        render (*e0, 5 * sec, [&] (int i) { return sine (i, vox); }, [&] (int i) { return sine (i, 0.355f, 220.0f); });
        p.music = 1.0f;
        auto e1 = make (p);
        render (*e1, 5 * sec, [&] (int i) { return sine (i, vox); }, [&] (int i) { return sine (i, 0.355f, 220.0f); });
        check (std::abs (e0->faderDb.load() + 2.0f) < 0.4f, fmt ("MUSIC 0%%: fader sits at %.2f dB (expect -2)", e0->faderDb.load()));
        check (e1->faderDb.load() > 8.0f, fmt ("MUSIC 100%% with a loud backing track: fader rides up to %.2f dB", e1->faderDb.load()));
    }

    // 8) OUTPUT trim, and mono works
    {
        flat::Params p; p.ride = false; p.lookaheadMs = 0.0f; p.outputDb = -6.0f;
        auto e = make (p);
        auto y = render (*e, sec, [&] (int i) { return sine (i, 0.5f); });
        check (std::abs (dbOf (peakOf (y, sec / 2, sec)) - dbOf (0.5f) + 6.0f) < 0.1f, "OUTPUT trim of -6 dB");

        flat::Engine m; m.prepare (SR, 512); p.outputDb = 0.0f; m.setParams (p, true);
        std::vector<float> buf (512, 0.25f); float* ch[1] = { buf.data() };
        m.process (ch, 1, nullptr, 0, 512);
        check (std::abs (buf[511] - 0.25f) < 1.0e-6f, "mono processing");
    }

    // 9) silence stays silent and finite
    {
        flat::Params p;
        auto e = make (p);
        auto y = render (*e, sec, [] (int) { return 0.0f; });
        bool ok = true; for (float v : y) ok = ok && std::isfinite (v) && v == 0.0f;
        check (ok, "silence in, silence out");
    }
}
} // namespace flatcheck
