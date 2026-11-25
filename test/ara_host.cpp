// FLATLINER - ARA HOST TEST
// A tiny ARA host: loads the built Flatliner VST3, applies it to a clip (an in-memory vocal take), lets it
// read the take, then plays the clip through it and checks that the planned ride is on the audio:
// the words even out, the chorus stays louder, nothing comes out late. This is what Studio Pro does when you
// drag Flatliner onto a clip, minus the screen.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <ARA_API/ARAInterface.h>
#include <ARA_Library/Dispatch/ARAHostDispatch.h>
#include <cstdio>

using namespace juce;

static int failures = 0;
static void check (bool ok, const String& what) { std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8()); if (! ok) ++failures; }

//==============================================================================
// the take: 20 s of verse around -30 dB, then 20 s of chorus around -18 dB; words of 0.4 s, each 5 dB up or
// down from the last, 0.15 s apart, a 1 s breath after every 4 words
struct Word { int start, end; float amp; };
static constexpr double SR = 48000.0;
static std::vector<Word> makeWords()
{
    std::vector<Word> w;
    const int sec = (int) SR, len = (int) (0.4 * SR), gap = (int) (0.15 * SR), breath = (int) (1.0 * SR);
    const float jitter[6] = { 1.8f, 0.55f, 1.25f, 0.6f, 1.6f, 0.8f };
    int pos = sec / 2, k = 0;
    while (pos + len < 40 * sec)
    {
        const bool chorus = pos >= 20 * sec;
        if (! chorus && pos + len > 20 * sec) { pos = 20 * sec + sec / 4; continue; }
        w.push_back ({ pos, pos + len, (chorus ? 0.18f : 0.045f) * jitter[k % 6] });
        pos += len + (((k + 1) % 4 == 0) ? breath : gap);
        ++k;
    }
    return w;
}

struct MemorySource
{
    AudioBuffer<float> audio;
    std::unique_ptr<ARAHostModel::AudioSource> source;
    using Converter = ARAHostModel::ConversionFunctions<MemorySource*, ARA::ARAAudioSourceHostRef>;
    bool read (float* const* buffers, int64 start, int64 n)
    {
        for (int c = 0; c < audio.getNumChannels(); ++c)
            for (int64 i = 0; i < n; ++i)
                buffers[c][i] = (start + i >= 0 && start + i < audio.getNumSamples()) ? audio.getSample (c, (int) (start + i)) : 0.0f;
        return true;
    }
};

class AudioAccess final : public ARA::Host::AudioAccessControllerInterface
{
public:
    struct Reader { ARA::ARAAudioSourceHostRef src; };
    using RC = ARAHostModel::ConversionFunctions<Reader*, ARA::ARAAudioReaderHostRef>;
    ARA::ARAAudioReaderHostRef createAudioReaderForSource (ARA::ARAAudioSourceHostRef s, bool use64) noexcept override
    {
        if (use64) return nullptr;
        auto r = std::make_unique<Reader> (Reader { s });
        auto ref = RC::toHostRef (r.get());
        const std::lock_guard<std::mutex> l (lock);
        readers.emplace (r.get(), std::move (r));
        return ref;
    }
    bool readAudioSamples (ARA::ARAAudioReaderHostRef r, ARA::ARASamplePosition pos, ARA::ARASampleCount n, void* const* buffers) noexcept override
    {
        ++reads;
        return MemorySource::Converter::fromHostRef (RC::fromHostRef (r)->src)->read (reinterpret_cast<float* const*> (buffers), pos, n);
    }
    void destroyAudioReader (ARA::ARAAudioReaderHostRef r) noexcept override { const std::lock_guard<std::mutex> l (lock); readers.erase (RC::fromHostRef (r)); }
    std::atomic<int> reads { 0 };
private:
    std::mutex lock;
    std::map<Reader*, std::unique_ptr<Reader>> readers;
};

class Archiving final : public ARA::Host::ArchivingControllerInterface
{
public:
    ARA::ARASize getArchiveSize (ARA::ARAArchiveReaderHostRef) noexcept override { return 0; }
    bool readBytesFromArchive (ARA::ARAArchiveReaderHostRef, ARA::ARASize, ARA::ARASize, ARA::ARAByte*) noexcept override { return false; }
    bool writeBytesToArchive (ARA::ARAArchiveWriterHostRef, ARA::ARASize, ARA::ARASize, const ARA::ARAByte*) noexcept override { return true; }
    void notifyDocumentArchivingProgress (float) noexcept override {}
    void notifyDocumentUnarchivingProgress (float) noexcept override {}
    ARA::ARAPersistentID getDocumentArchiveID (ARA::ARAArchiveReaderHostRef) noexcept override { return nullptr; }
};

class ModelUpdates final : public ARA::Host::ModelUpdateControllerInterface
{
public:
    void notifyAudioSourceAnalysisProgress (ARA::ARAAudioSourceHostRef, ARA::ARAAnalysisProgressState state, float) noexcept override
    {
        if (state == ARA::kARAAnalysisProgressStarted) started = true;
        if (state == ARA::kARAAnalysisProgressCompleted) completed = true;
    }
    void notifyAudioSourceContentChanged (ARA::ARAAudioSourceHostRef, const ARA::ARAContentTimeRange*, ARA::ContentUpdateScopes) noexcept override {}
    void notifyAudioModificationContentChanged (ARA::ARAAudioModificationHostRef, const ARA::ARAContentTimeRange*, ARA::ContentUpdateScopes) noexcept override {}
    void notifyPlaybackRegionContentChanged (ARA::ARAPlaybackRegionHostRef, const ARA::ARAContentTimeRange*, ARA::ContentUpdateScopes scopes) noexcept override
    {
        if (scopes.affectSamples()) regionChanged = true;
    }
    void notifyDocumentDataChanged() noexcept override {}
    bool started = false, completed = false, regionChanged = false;
};

class Playback final : public ARA::Host::PlaybackControllerInterface
{
public:
    void requestStartPlayback() noexcept override {}
    void requestStopPlayback() noexcept override {}
    void requestSetPlaybackPosition (ARA::ARATimePosition) noexcept override {}
    void requestSetCycleRange (ARA::ARATimePosition, ARA::ARATimeDuration) noexcept override {}
    void requestEnableCycle (bool) noexcept override {}
};

struct Head final : AudioPlayHead
{
    Optional<PositionInfo> getPosition() const override { PositionInfo p; p.setTimeInSamples (pos); p.setIsPlaying (true); p.setTimeInSeconds ((double) pos / SR); return p; }
    int64 pos = 0;
};

static float rmsDb (const AudioBuffer<float>& b, int a, int e)
{
    double s = 0; for (int i = a; i < e; ++i) s += (double) b.getSample (0, i) * b.getSample (0, i);
    return 10.0f * std::log10 ((float) (s / (e - a)) + 1.0e-12f);
}

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    ScopedJuceInitialiser_GUI init;
    if (argc < 2) { std::printf ("usage: FlatlinerARAHost /path/to/Flatliner.vst3\n"); return 2; }

    VST3PluginFormat fmt;
    OwnedArray<PluginDescription> descs;
    fmt.findAllTypesForFile (descs, String (argv[1]));
    check (! descs.isEmpty(), "found Flatliner in " + String (argv[1]));
    if (descs.isEmpty()) return 1;
    check (descs[0]->hasARAExtension, "Flatliner says it has an ARA extension");

    String err;
    auto plugin = fmt.createInstanceFromDescription (*descs[0], SR, 512, err);
    check (plugin != nullptr, "loaded (" + err + ")");
    if (plugin == nullptr) return 1;

    ARAFactoryWrapper factory;
    bool gotFactory = false;
    createARAFactoryAsync (*plugin, [&] (ARAFactoryWrapper f) { factory = std::move (f); gotFactory = true; });
    for (int i = 0; i < 100 && ! gotFactory; ++i) MessageManager::getInstance()->runDispatchLoopUntil (20);
    check (factory.get() != nullptr, "got its ARA factory");
    if (factory.get() == nullptr) return 1;

    auto audioAccess = std::make_unique<AudioAccess>();
    auto* access = audioAccess.get();
    auto modelUpdates = std::make_unique<ModelUpdates>();
    auto* updates = modelUpdates.get();
    auto doc = ARAHostDocumentController::create (factory, "Flatliner ARA test", std::move (audioAccess), std::make_unique<Archiving>(),
                                                  nullptr, std::move (modelUpdates), std::make_unique<Playback>());
    check (doc != nullptr, "ARA document created");
    if (doc == nullptr) return 1;
    auto& dc = doc->getDocumentController();

    // the take
    const auto words = makeWords();
    MemorySource take;
    take.audio.setSize (1, (int) (40 * SR));
    take.audio.clear();
    for (auto& w : words) for (int i = w.start; i < w.end; ++i) take.audio.setSample (0, i, w.amp * (float) std::sin (6.283185307179586 * 1000.0 * i / SR));

    std::unique_ptr<ARAHostModel::MusicalContext> ctx;
    std::unique_ptr<ARAHostModel::RegionSequence> seq;
    std::unique_ptr<ARAHostModel::AudioModification> mod;
    std::unique_ptr<ARAHostModel::PlaybackRegion> region;
    {
        const ARAEditGuard edit (dc);
        auto cp = ARAHostModel::MusicalContext::getEmptyProperties(); cp.name = "song"; cp.orderIndex = 0;
        ctx = std::make_unique<ARAHostModel::MusicalContext> (nullptr, dc, cp);
        auto sp = ARAHostModel::RegionSequence::getEmptyProperties(); sp.name = "Lead Vox"; sp.orderIndex = 0; sp.musicalContextRef = ctx->getPluginRef();
        seq = std::make_unique<ARAHostModel::RegionSequence> (nullptr, dc, sp);
        auto ap = ARAHostModel::AudioSource::getEmptyProperties();
        ap.name = "take"; ap.persistentID = "take"; ap.sampleCount = take.audio.getNumSamples(); ap.sampleRate = SR; ap.channelCount = 1; ap.merits64BitSamples = false;
        take.source = std::make_unique<ARAHostModel::AudioSource> (MemorySource::Converter::toHostRef (&take), dc, ap);
        auto mp = ARAHostModel::AudioModification::getEmptyProperties(); mp.persistentID = "mod";
        mod = std::make_unique<ARAHostModel::AudioModification> (nullptr, dc, *take.source, mp);
        auto rp = ARAHostModel::PlaybackRegion::getEmptyProperties();
        rp.transformationFlags = ARA::kARAPlaybackTransformationNoChanges;
        rp.startInModificationTime = 0.0; rp.durationInModificationTime = 40.0; rp.startInPlaybackTime = 0.0; rp.durationInPlaybackTime = 40.0;
        rp.musicalContextRef = ctx->getPluginRef(); rp.regionSequenceRef = seq->getPluginRef();
        region = std::make_unique<ARAHostModel::PlaybackRegion> (nullptr, dc, *mod, rp);
    }

    auto ext = doc->bindDocumentToPluginInstance (*plugin, ARA::kARAPlaybackRendererRole | ARA::kARAEditorRendererRole,
                                                  ARA::kARAPlaybackRendererRole | ARA::kARAEditorRendererRole);
    check (ext.isValid(), "Flatliner bound to the document (applied to the clip)");
    auto renderer = ext.getPlaybackRendererInterface();
    renderer.add (*region);

    Head head;
    plugin->setPlayHead (&head);
    plugin->setPlayConfigDetails (1, 1, SR, 512);
    plugin->setNonRealtime (true);   // render like an export (a DAW playing in real time reads ahead instead)
    plugin->prepareToPlay (SR, 512);
    check (plugin->getLatencySamples() == 0, "ARA mode: no latency (" + String (plugin->getLatencySamples()) + " samples)");

    // the DAW gives access to the samples: Flatliner reads the take by itself
    {
        const ARAEditGuard edit (dc);
        take.source->enableAudioSourceSamplesAccess (true);
    }
    const auto t0 = Time::getMillisecondCounterHiRes();
    for (int i = 0; i < 400 && ! updates->completed; ++i) { MessageManager::getInstance()->runDispatchLoopUntil (10); dc.notifyModelUpdates(); }
    for (int i = 0; i < 20; ++i) { MessageManager::getInstance()->runDispatchLoopUntil (10); dc.notifyModelUpdates(); }
    check (updates->started && updates->completed, String::formatted ("it reads the whole take without playing it (%.0f ms, %d reads)", Time::getMillisecondCounterHiRes() - t0, access->reads.load()));
    check (updates->regionChanged, "and tells the DAW the clip's sound changed");
    Thread::sleep (150);   // the planner picks the analysis up

    // play the clip through it
    AudioBuffer<float> out (1, take.audio.getNumSamples()), block (1, 512);
    MidiBuffer midi;
    for (int pos = 0; pos < out.getNumSamples(); pos += 512)
    {
        const int n = jmin (512, out.getNumSamples() - pos);
        block.setSize (1, n, false, false, true);
        block.clear();
        head.pos = pos;
        plugin->processBlock (block, midi);
        out.copyFrom (0, pos, block, 0, 0, n);
    }

    auto wordDb = [&] (const AudioBuffer<float>& b, const Word& w) { return rmsDb (b, w.start + (w.end - w.start) / 4, w.end - (w.end - w.start) / 4); };
    std::vector<const Word*> verse, chorus;
    for (auto& w : words)
    {
        if (w.start > 6 * SR && w.end < 19 * SR) verse.push_back (&w);
        if (w.start > 26 * SR && w.end < 39 * SR) chorus.push_back (&w);
    }
    auto stats = [&] (const AudioBuffer<float>& b, const std::vector<const Word*>& ws, float& mean, float& spread)
    {
        float lo = 200, hi = -200; double s = 0;
        for (auto* w : ws) { const float d = wordDb (b, *w); lo = jmin (lo, d); hi = jmax (hi, d); s += d; }
        mean = (float) (s / (double) ws.size()); spread = hi - lo;
    };
    float vIn, vInS, cIn, cInS, vOut, vOutS, cOut, cOutS;
    stats (take.audio, verse, vIn, vInS); stats (take.audio, chorus, cIn, cInS);
    stats (out, verse, vOut, vOutS); stats (out, chorus, cOut, cOutS);
    check (cOutS < 0.5f * cInS && vOutS < 0.5f * vInS,
           String::formatted ("played back through the plugin: the words even out (chorus %.1f dB spread, was %.1f; verse %.1f, was %.1f)", cOutS, cInS, vOutS, vInS));
    check (cOut - vOut > 10.0f, String::formatted ("and the chorus stays louder (%.1f dB over the verse; it was %.1f)", cOut - vOut, cIn - vIn));
    // nothing late: inside a word the output is the take times the fader, sample for sample
    {
        const auto& w = *chorus[2];
        double oi = 0, ii = 0;
        for (int i = w.start + 2000; i < w.start + 4000; ++i) { oi += out.getSample (0, i) * take.audio.getSample (0, i); ii += take.audio.getSample (0, i) * take.audio.getSample (0, i); }
        const double g = ii > 0 ? oi / ii : 0;
        double res = 0, sig = 0;
        for (int i = w.start + 2000; i < w.start + 4000; ++i) { const double e = out.getSample (0, i) - g * take.audio.getSample (0, i); res += e * e; sig += out.getSample (0, i) * out.getSample (0, i); }
        check (g > 0 && res < 0.001 * sig, String::formatted ("nothing comes out late: the output is the take times the fader (%.4f%% left over)", 100.0 * res / juce::jmax (1e-12, sig)));
    }

    // turn RIDE off: it re-plans, and the clip plays back untouched
    {
        AudioProcessorParameter* ride = nullptr;
        for (auto* prm : plugin->getParameters()) if (prm->getName (32) == "Ride") ride = prm;
        check (ride != nullptr, "found the RIDE switch");
        if (ride != nullptr)
        {
            ride->setValueNotifyingHost (0.0f);
            for (int pos = 0; pos < 4 * 512; pos += 512) { block.setSize (1, 512, false, false, true); block.clear(); head.pos = pos; plugin->processBlock (block, midi); }   // the knob reaches the planner
            Thread::sleep (200);
            float worst = 0;
            for (auto* w : { chorus[1], verse[3] })
                for (int pos = w->start; pos < w->end - 512; pos += 512)
                {
                    block.setSize (1, 512, false, false, true); block.clear(); head.pos = pos;
                    plugin->processBlock (block, midi);
                    for (int i = 0; i < 512; ++i) worst = jmax (worst, std::abs (block.getSample (0, i) - take.audio.getSample (0, pos + i)));
                }
            check (worst < 1.0e-4f, String::formatted ("RIDE off: re-planned, the clip plays back untouched (max difference %.6f)", worst));
            ride->setValueNotifyingHost (1.0f);
        }
    }

    // a second take, removed while Flatliner is still reading it
    {
        MemorySource t2;
        t2.audio.makeCopyOf (take.audio);
        auto ap = ARAHostModel::AudioSource::getEmptyProperties();
        ap.name = "take 2"; ap.persistentID = "take2"; ap.sampleCount = t2.audio.getNumSamples(); ap.sampleRate = SR; ap.channelCount = 1;
        {
            const ARAEditGuard edit (dc);
            t2.source = std::make_unique<ARAHostModel::AudioSource> (MemorySource::Converter::toHostRef (&t2), dc, ap);
            t2.source->enableAudioSourceSamplesAccess (true);
        }
        Thread::sleep (5);
        {
            const ARAEditGuard edit (dc);
            t2.source->enableAudioSourceSamplesAccess (false);
            t2.source.reset();
        }
        MessageManager::getInstance()->runDispatchLoopUntil (100);
        dc.notifyModelUpdates();
        check (true, "a take removed while it's being read: no crash");
    }

    plugin->releaseResources();
    renderer.remove (*region);
    {
        const ARAEditGuard edit (dc);
        take.source->enableAudioSourceSamplesAccess (false);
        region.reset(); mod.reset(); take.source.reset(); seq.reset(); ctx.reset();
    }
    doc.reset();
    factory = {};      // the factory goes before the plugin's library is unloaded
    plugin.reset();
    std::printf ("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
