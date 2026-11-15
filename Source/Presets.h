#pragma once
#include <juce_core/juce_core.h>
#include <vector>
#include <utility>

namespace flat
{
struct Preset { juce::String name; std::vector<std::pair<juce::String, float>> values; };

// ids: target (dB), range (dB), speed (%), gate (dB), lookahead (ms), music (%), output (dB), ride (0/1)
inline const std::vector<Preset>& factoryPresets()
{
    static const std::vector<Preset> p {
        { "Init (Gentle Ride)",   { } },
        { "Lead Vocal: Natural",  { { "target", -18 }, { "range", 4 },  { "speed", 40 }, { "lookahead", 10 } } },
        { "Lead Vocal: Tight",    { { "target", -17 }, { "range", 8 },  { "speed", 70 }, { "lookahead", 12 } } },
        { "Lead Vocal: Locked In",{ { "target", -16 }, { "range", 10 }, { "speed", 85 }, { "lookahead", 15 } } },
        { "Rap: On The Line",     { { "target", -16 }, { "range", 10 }, { "speed", 90 }, { "lookahead", 15 }, { "gate", -50 } } },
        { "Backing Vocals",       { { "target", -22 }, { "range", 5 },  { "speed", 30 }, { "lookahead", 10 } } },
        { "Whisper Booster",      { { "target", -20 }, { "range", 12 }, { "speed", 40 }, { "gate", -62 }, { "lookahead", 10 } } },
        { "Over The Music",       { { "target", -20 }, { "range", 9 },  { "speed", 50 }, { "music", 70 }, { "lookahead", 10 } } },
        { "Podcast Voice",        { { "target", -20 }, { "range", 10 }, { "speed", 60 }, { "gate", -50 }, { "lookahead", 10 } } },
        { "Slow Burn",            { { "target", -18 }, { "range", 3 },  { "speed", 15 }, { "lookahead", 20 } } },
        { "Live Tracking",        { { "target", -18 }, { "range", 6 },  { "speed", 50 }, { "lookahead", 0 } } },
        { "Dead Flat",            { { "target", -16 }, { "range", 18 }, { "speed", 100 }, { "lookahead", 20 } } },
    };
    return p;
}
} // namespace flat
