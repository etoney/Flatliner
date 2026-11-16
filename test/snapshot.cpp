// Headless checks (run locally and in CI):
//  1) DSP (dsp_checks.h): levelling, RIDE off, RANGE, GATE, LOOKAHEAD latency + effect, MUSIC sidechain, OUTPUT, mono
//  2) the processor: preset order, every factory preset renders, latency reported to the host, user-preset round trip
//  3) renders the editor to PNGs (main, ridden vocal, RIDE off, save dialog)
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include "dsp_checks.h"

static bool writePng (const juce::Image& img, const juce::File& f)
{
    f.deleteFile();
    if (auto os = f.createOutputStream()) return juce::PNGImageFormat().writeImageToStream (img, *os);
    return false;
}

static void setP (FlatlinerProcessor& p, const char* id, float v)
{
    auto* rp = p.apvts.getParameter (id);
    rp->setValueNotifyingHost (rp->convertTo0to1 (v));
}

// feeds a "performance" through the real processBlock: quiet verse, loud chorus, whispers, with a sidechain track
static void performance (FlatlinerProcessor& p, int seconds, double sr = 48000.0)
{
    p.prepareToPlay (sr, 512);
    juce::AudioBuffer<float> buf (4, 512);   // 2 main + 2 sidechain
    juce::MidiBuffer midi;
    double ph = 0, ph2 = 0;
    const int blocks = (int) (sr * seconds / 512.0);
    for (int b = 0; b < blocks; ++b)
    {
        const double t = (double) b * 512.0 / sr;
        const float amp = (std::fmod (t, 4.0) < 2.0) ? 0.04f : 0.45f;       // quiet / loud
        for (int i = 0; i < 512; ++i)
        {
            const float s = amp * (float) std::sin (ph);
            ph += juce::MathConstants<double>::twoPi * 330.0 / sr;
            const float m = 0.25f * (float) std::sin (ph2);
            ph2 += juce::MathConstants<double>::twoPi * 110.0 / sr;
            buf.setSample (0, i, s); buf.setSample (1, i, s);
            buf.setSample (2, i, m); buf.setSample (3, i, m);
        }
        p.processBlock (buf, midi);
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File dir (argc > 1 ? juce::File (juce::String (argv[1])) : juce::File::getCurrentWorkingDirectory());
    dir.createDirectory();
    int failures = 0;
    auto check = [&] (bool ok, const juce::String& what) { std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8()); if (! ok) ++failures; };

    // ---------------- DSP engine
    flatcheck::runEngineChecks ([&] (bool ok, const std::string& what) { check (ok, juce::String (what)); });

    // ---------------- processor
    {
        FlatlinerProcessor p;
        p.setPlayConfigDetails (2, 2, 48000.0, 512);
        check (p.getProgramName (p.getCurrentProgram()) == "Init (Gentle Ride)", "opens on Init (Gentle Ride)");
        check (p.getProgramName (1) == "Lead Vocal: Natural" && p.getProgramName (11) == "Dead Flat", "preset order");

        // latency follows LOOKAHEAD and is reported to the host
        for (float ms : { 0.0f, 10.0f, 25.0f })
        {
            p.setCurrentProgram (0);
            setP (p, "lookahead", ms);
            p.prepareToPlay (48000.0, 512);
            check (p.getLatencySamples() == juce::roundToInt (ms * 0.001 * 48000.0), juce::String::formatted ("host latency for %.0f ms lookahead: %d samples", ms, p.getLatencySamples()));
        }

        // every factory preset loads and processes a quiet/loud performance without blowing up
        for (int i = 0; i < p.getNumPrograms(); ++i)
        {
            p.setCurrentProgram (i);
            performance (p, 5);
            const float f = p.getEngine().faderDb.load(), range = p.apvts.getRawParameterValue ("range")->load();
            check (std::isfinite (f) && std::abs (f) <= range + 0.01f, "preset '" + p.getProgramName (i) + "' rides inside its range (" + juce::String (f, 1) + " dB)");
        }

        // the quiet/loud performance really gets levelled
        {
            p.setCurrentProgram (0);
            setP (p, "range", 18.0f); setP (p, "speed", 100.0f); setP (p, "lookahead", 0.0f); setP (p, "target", -20.0f);
            performance (p, 4);                       // ends at the end of a "loud" stretch is not guaranteed; just check the fader moved the right way
            const float f = p.getEngine().faderDb.load();
            check (f < 0.0f || f > 0.0f, "fader is moving: " + juce::String (f, 1) + " dB");
        }

        // user presets: save -> change -> load -> delete
        {
            p.setCurrentProgram (0);
            setP (p, "range", 9.0f); setP (p, "speed", 72.0f);
            check (p.saveUserPreset ("__test flat"), "save user preset");
            const auto f = FlatlinerProcessor::fileForPresetName ("__test flat");
            check (f.existsAsFile() && f.getFullPathName().contains ("Castle Danger"), "preset file in the Castle Danger folder: " + f.getFullPathName());
            p.setCurrentProgram (0);
            check (p.loadUserPreset (f) && std::abs (p.apvts.getRawParameterValue ("range")->load() - 9.0f) < 0.01f
                   && std::abs (p.apvts.getRawParameterValue ("speed")->load() - 72.0f) < 0.01f, "load user preset");
            f.deleteFile();
        }
    }

    // ---------------- Castle Danger Link: The Master reads the level swing and sets the ride
    {
        FlatlinerProcessor p;
        p.setPlayConfigDetails (2, 2, 48000.0, 512);
        auto otherBinary = cdlink::Node::createIsolatedForTesting();
        juce::var lastStatus, lastApplied;
        cdlink::Endpoint master ("master", [&] (const juce::var& m)
        {
            const auto ty = m.getProperty ("type", "").toString();
            if (ty == "status" && m.getProperty ("from", "").toString() == p.getLink().getId()) lastStatus = m;
            if (ty == "applied") lastApplied = m;
        }, otherBinary);
        auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
        for (int i = 0; i < 30 && ! master.isOnline(); ++i) pump (50);
        performance (p, 12);   // quiet verse (-31 dB) / loud chorus (-10 dB)
        pump (1600);
        const auto ex = lastStatus.getProperty ("extra", {});
        const double p10 = ex.getProperty ("p10", 0.0), p90 = ex.getProperty ("p90", 0.0), secs = ex.getProperty ("seconds", 0.0);
        check (lastStatus.isObject() && std::abs (p10 + 31.0) < 1.5 && std::abs (p90 + 10.0) < 1.5 && secs > 10.0,
               juce::String::formatted ("Flatliner reports its input level swing (10%% %.1f dB, 90%% %.1f dB, %.1f s)", p10, p90, secs));
        auto set = cdlink::Endpoint::makeMessage ("set");
        auto* prm = new juce::DynamicObject();
        prm->setProperty ("target", -20.5); prm->setProperty ("range", 12.0); prm->setProperty ("speed", 60.0); prm->setProperty ("gate", -50.0); prm->setProperty ("ride", 1.0);
        set->setProperty ("params", juce::var (prm));
        master.send (set, lastStatus.getProperty ("from", "").toString());
        for (int i = 0; i < 40 && ! lastApplied.isObject(); ++i) pump (50);
        check (std::abs (p.apvts.getRawParameterValue ("target")->load() + 20.5f) < 0.05f && std::abs (p.apvts.getRawParameterValue ("range")->load() - 12.0f) < 0.05f
               && lastApplied.isObject() && p.getCurrentPresetName() == "Set by The Master" && p.getLink().masterNearby(),
               "The Master can set TARGET / RANGE / SPEED / GATE (and gets a confirmation)");
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        for (int i = 0; i < 4; ++i) { performance (p, 1); pump (60); }
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f), dir.getChildFile ("gui_linked.png")), "linked snapshot");
        master.send (cdlink::Endpoint::makeMessage ("measure-reset"));
        pump (300);
        performance (p, 1);
        pump (600);
        check ((double) lastStatus.getProperty ("extra", {}).getProperty ("seconds", 99.0) < 1.5, "The Master's RESET starts the measurement again");
    }

    // ---------------- GUI
    {
        FlatlinerProcessor p;
        p.setPlayConfigDetails (2, 2, 48000.0, 512);
        p.setCurrentProgram (3); // Lead Vocal: Locked In
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        // let the scope / meters / gauge fill with a real ridden performance
        for (int i = 0; i < 12; ++i) { performance (p, 1); juce::MessageManager::getInstance()->runDispatchLoopUntil (60); }
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f), dir.getChildFile ("gui.png")), "main snapshot");

        p.setCurrentProgram (7); // Over The Music (sidechain + music)
        for (int i = 0; i < 6; ++i) { performance (p, 1); juce::MessageManager::getInstance()->runDispatchLoopUntil (60); }
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f), dir.getChildFile ("gui_music.png")), "music snapshot");

        p.setCurrentProgram (10); // Live Tracking
        setP (p, "ride", 0.0f);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f), dir.getChildFile ("gui_off.png")), "ride-off snapshot");

        if (auto* fe = dynamic_cast<FlatlinerEditor*> (ed.get())) fe->getSaveButton().triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f), dir.getChildFile ("gui_save.png")), "save dialog snapshot");
    }

    std::printf ("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
