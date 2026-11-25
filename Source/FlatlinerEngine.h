#pragma once
// FLATLINER engine: an auto leveler.
//   Listens to the vocal, works out how far it is from TARGET, and moves a virtual fader
//   (within +/- RANGE dB) to flatten the performance onto that line.
//   Lookahead delays the audio so the fader is already moving when a loud or quiet phrase arrives.
//   Pure C++ (no JUCE) so the DSP can be tested on its own.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace flat
{
constexpr float maxLookaheadMs = 50.0f;

struct Params
{
    float targetDb    = -18.0f;  // RMS level the rider aims for (dBFS)
    float rangeDb     = 6.0f;    // max fader travel, up and down
    float speed       = 0.5f;    // 0..1   how fast the fader moves
    float gateDb      = -55.0f;  // below this the fader freezes
    float lookaheadMs = 10.0f;   // 0..50  (reported to the host as latency)
    float music       = 0.0f;    // 0..1   how much the sidechain raises the target
    bool  follow      = false;   // FOLLOW SECTIONS (the plugin turns it on by default): the line follows the vocal's own level over the last few seconds
    float outputDb    = 0.0f;    // final trim
    bool  ride        = true;    // off = fader parked at 0 dB
};

inline float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }

class Engine
{
public:
    void prepare (double sampleRate, int /*maxBlock*/)
    {
        sr = sampleRate > 1000.0 ? sampleRate : 44100.0;
        delaySize = (int) std::ceil ((double) maxLookaheadMs * 0.001 * sr) + 2;
        for (auto& l : line) l.assign ((size_t) delaySize, 0.0f);
        writePos = 0;
        hpState = fastMs = vocalMs = musicMs = 0.0f;
        sectionDb = -100.0f;
        sectionCoef = 1.0f - std::exp (-1.0f / (6.0f * (float) sr));          // ~6 s: a line or two, never the whole song
        fader = 0.0f;
        hpCoef    = 1.0f - std::exp (-6.2831853f * 120.0f / (float) sr);       // ~120 Hz high-pass so rumble doesn't move the fader
        fastCoef  = 1.0f - std::exp (-1.0f / (0.010f * (float) sr));           // 10 ms mean-square first, so the waveform ripple doesn't bias the reading
        atkCoef   = 1.0f - std::exp (-1.0f / (0.015f * (float) sr));           // then: quick to hear a loud phrase...
        relCoef   = 1.0f - std::exp (-1.0f / (0.150f * (float) sr));           // ...slow to let go
        musicCoef = 1.0f - std::exp (-1.0f / (0.400f * (float) sr));
        setParams (params, true);
    }

    void setParams (const Params& p, bool jump = false)
    {
        params = p;
        lookaheadSamples = std::clamp ((int) std::lround ((double) p.lookaheadMs * 0.001 * sr), 0, delaySize - 1);
        const float s = std::clamp (p.speed, 0.0f, 1.0f);
        const float downMs = 400.0f + (25.0f - 400.0f) * s;     // gain cuts are faster than boosts, like a human hand
        const float upMs   = 1800.0f + (120.0f - 1800.0f) * s;
        downCoef = 1.0f - std::exp (-1.0f / (0.001f * downMs * (float) sr));
        upCoef   = 1.0f - std::exp (-1.0f / (0.001f * upMs   * (float) sr));
        if (jump) outGain = dbToGain (p.outputDb);
    }

    int getLatency() const { return lookaheadSamples; }
    int latencyFor (float ms) const { return std::clamp ((int) std::lround ((double) ms * 0.001 * sr), 0, delaySize - 1); }

    // audio: numCh (1 or 2) channels, processed in place. sc: optional sidechain (the music), numSc channels.
    void process (float* const* audio, int numCh, const float* const* sc, int numSc, int n)
    {
        const int chs = std::clamp (numCh, 1, 2);
        const float outTarget = dbToGain (params.outputDb);
        const float outStep = (outTarget - outGain) / (float) std::max (1, n);
        const bool haveSc = sc != nullptr && numSc > 0;
        float blkIn = 0.0f, blkOut = 0.0f, lastVocalDb = -100.0f, lastMusicDb = -100.0f;

        for (int i = 0; i < n; ++i)
        {
            // ---- vocal detector: mono sum -> high-pass -> 10 ms mean-square -> fast attack / slow release
            float x = 0.0f;
            for (int c = 0; c < chs; ++c) { x += audio[c][i]; blkIn = std::max (blkIn, std::abs (audio[c][i])); }
            x /= (float) chs;
            hpState += hpCoef * (x - hpState);
            const float y = x - hpState;
            fastMs += fastCoef * (y * y - fastMs);
            vocalMs += (fastMs > vocalMs ? atkCoef : relCoef) * (fastMs - vocalMs);

            // ---- music detector (sidechain)
            float s = 0.0f;
            if (haveSc)
            {
                for (int c = 0; c < numSc; ++c) s += sc[c][i];
                s /= (float) numSc;
            }
            musicMs += musicCoef * (s * s - musicMs);

            const float vocalDb = 10.0f * std::log10 (vocalMs + 1.0e-10f);
            const float musicDb = 10.0f * std::log10 (musicMs + 1.0e-10f);
            lastVocalDb = vocalDb;
            lastMusicDb = musicDb;

            // ---- where should the fader be?
            float desired = 0.0f;
            if (params.ride)
            {
                if (vocalDb < params.gateDb)
                    desired = fader;                      // gate closed: hold, don't push breaths and room noise up
                else
                {
                    // FOLLOW SECTIONS: aim at where the vocal has been sitting for the last few seconds,
                    // so it still evens out the words but a louder chorus stays louder
                    if (sectionDb < -99.0f) sectionDb = vocalDb;
                    else sectionDb += sectionCoef * (vocalDb - sectionDb);
                    float target = params.follow ? sectionDb : params.targetDb;
                    if (haveSc && params.music > 0.0f)    // louder music -> the vocal target rises (0 dB at -30 dB music, +12 dB at -18 dB)
                        target += params.music * std::clamp (musicDb + 30.0f, 0.0f, 12.0f);
                    desired = std::clamp (target - vocalDb, -params.rangeDb, params.rangeDb);
                }
            }
            const float diff = desired - fader;
            fader += diff * (diff < 0.0f ? downCoef : upCoef);

            // ---- delay the audio (lookahead) and apply the fader + output trim
            outGain += outStep;
            const float g = dbToGain (fader) * outGain;
            for (int c = 0; c < chs; ++c)
            {
                float* d = audio[c];
                float* l = line[(size_t) c].data();
                l[writePos] = d[i];
                int rp = writePos - lookaheadSamples;
                if (rp < 0) rp += delaySize;
                d[i] = l[rp] * g;
                blkOut = std::max (blkOut, std::abs (d[i]));
            }
            if (++writePos >= delaySize) writePos = 0;
        }
        outGain = outTarget;

        inPeak.store (std::max (inPeak.load (std::memory_order_relaxed), blkIn), std::memory_order_relaxed);
        outPeak.store (std::max (outPeak.load (std::memory_order_relaxed), blkOut), std::memory_order_relaxed);
        voiceDb.store (lastVocalDb, std::memory_order_relaxed);
        musicDb.store (lastMusicDb, std::memory_order_relaxed);
        faderDb.store (fader, std::memory_order_relaxed);
        lineDb.store (params.follow ? (sectionDb > -99.0f ? sectionDb : params.targetDb) : params.targetDb, std::memory_order_relaxed);
    }

    // telemetry for the editor (peaks are read-and-reset with exchange (0))
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };
    std::atomic<float> voiceDb { -100.0f }, musicDb { -100.0f }, faderDb { 0.0f }, lineDb { -18.0f };   // lineDb: where the rider aims now

private:
    double sr = 44100.0;
    Params params;
    std::vector<float> line[2] = { std::vector<float> (1, 0.0f), std::vector<float> (1, 0.0f) };
    int delaySize = 1, writePos = 0, lookaheadSamples = 0;
    float hpCoef = 0, fastCoef = 0, atkCoef = 0, relCoef = 0, musicCoef = 0, downCoef = 0, upCoef = 0;
    float hpState = 0, fastMs = 0, vocalMs = 0, musicMs = 0, fader = 0, outGain = 1.0f;
    float sectionDb = -100.0f, sectionCoef = 0;
};
} // namespace flat
