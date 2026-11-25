#pragma once
// FLATLINER - ARA MODE
// Applied to the vocal clip itself (Studio Pro: drag Flatliner onto the clip; Logic, Cubase, Reaper: as an
// ARA extension), Flatliner reads the whole take as soon as the DAW lets it, works out its level every 10 ms
// (RidePlan.h), and plans the ride with the whole take in view. Playback then reads the clip and applies the
// planned fader: no lookahead, no latency, no learning pass.
//
//   FlatDocumentController  one per song: reads each audio source on a background thread (the analysis)
//   FlatRenderer            one per Flatliner instance: plans the ride for its clips with this instance's
//                           knobs, and plays them back with the fader applied
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "RidePlan.h"

#if JucePlugin_Enable_ARA
#include <map>
#include <memory>
#include <mutex>
#include <set>

class FlatDocumentController;

//==============================================================================
// One read of one audio source, start to finish, off the DAW's threads
class FlatAnalysisJob : public juce::ThreadPoolJob
{
public:
    FlatAnalysisJob (FlatDocumentController& dcIn, juce::ARAAudioSource* src)
        : juce::ThreadPoolJob ("Flatliner analysis"), dc (dcIn), source (src),
          reader (std::make_unique<juce::ARAAudioSourceReader> (src)),   // made on the document thread, read here
          channels (juce::jmax (1, (int) src->getChannelCount())), length (src->getSampleCount()), rate (src->getSampleRate())
    {}
    JobStatus runJob() override;
    juce::ARAAudioSource* getSource() const { return source; }
private:
    FlatDocumentController& dc;
    juce::ARAAudioSource* source;
    std::unique_ptr<juce::ARAAudioSourceReader> reader;
    int channels;
    juce::int64 length;
    double rate;
};

//==============================================================================
class FlatDocumentController final : public juce::ARADocumentControllerSpecialisation
{
public:
    using juce::ARADocumentControllerSpecialisation::ARADocumentControllerSpecialisation;
    ~FlatDocumentController() override { pool.removeAllJobs (true, 4000); }

    // the analysis of a source (nullptr until it's read). Any thread.
    std::shared_ptr<const flat::Analysis> analysisFor (const juce::ARAAudioSource* src) const
    {
        const std::lock_guard<std::mutex> l (lock);
        const auto it = analyses.find (src);
        return it == analyses.end() ? nullptr : it->second;
    }
    std::atomic<int> analysisVersion { 0 };   // goes up whenever an analysis arrives or goes

    // from the job
    void analysisDone (juce::ARAAudioSource* src, flat::Analysis a)
    {
        {
            const std::lock_guard<std::mutex> l (lock);
            if (live.count (src) == 0) return;
            analyses[src] = std::make_shared<const flat::Analysis> (std::move (a));
        }
        ++analysisVersion;
        // let the DAW know the ride changed the clip's sound (redraws, re-renders)
        juce::MessageManager::callAsync ([this, src, safe = juce::WeakReference<FlatDocumentController> (this)]
        {
            if (safe == nullptr) return;
            { const std::lock_guard<std::mutex> l (lock); if (live.count (src) == 0) return; }
            for (auto* mod : src->getAudioModifications())
                for (auto* region : mod->getPlaybackRegions())
                    region->notifyContentChanged (juce::ARAContentUpdateScopes::samplesAreAffected(), true);
        });
    }

protected:
    juce::ARAAudioSource* doCreateAudioSource (juce::ARADocument* document, ARA::ARAAudioSourceHostRef hostRef) noexcept override
    {
        auto* src = new juce::ARAAudioSource (document, hostRef);
        src->addListener (&watcher);
        { const std::lock_guard<std::mutex> l (lock); live.insert (src); }
        return src;
    }

    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() noexcept override;

    // nothing of our own to store in the song: the analysis is read again from the audio, the knobs live
    // in each plugin's own state
    bool doRestoreObjectsFromStream (juce::ARAInputStream& input, const juce::ARARestoreObjectsFilter*) noexcept override { return ! input.failed(); }
    bool doStoreObjectsToStream (juce::ARAOutputStream&, const juce::ARAStoreObjectsFilter*) noexcept override { return true; }

private:
    // what happens to the audio sources (sample access, edits, deletion)
    struct Watcher : juce::ARAAudioSource::Listener
    {
        explicit Watcher (FlatDocumentController& d) : dc (d) {}
        void didEnableAudioSourceSamplesAccess (juce::ARAAudioSource* s, bool enable) override { dc.accessChanged (s, enable); }
        void doUpdateAudioSourceContent (juce::ARAAudioSource* s, juce::ARAContentUpdateScopes scopes) override { dc.contentChanged (s, scopes); }
        void didUpdateAudioSourceProperties (juce::ARAAudioSource* s) override { dc.propertiesChanged (s); }
        void willDestroyAudioSource (juce::ARAAudioSource* s) override { dc.sourceGoing (s); }
        FlatDocumentController& dc;
    };
    Watcher watcher { *this };

    void accessChanged (juce::ARAAudioSource* src, bool enable)
    {
        if (! enable) { cancel (src); return; }
        if (analysisFor (src) == nullptr) start (src);
    }
    void contentChanged (juce::ARAAudioSource* src, juce::ARAContentUpdateScopes scopes)
    {
        if (! scopes.affectSamples()) return;
        cancel (src);
        { const std::lock_guard<std::mutex> l (lock); analyses.erase (src); }
        ++analysisVersion;
        if (src->isSampleAccessEnabled()) start (src);
    }
    void propertiesChanged (juce::ARAAudioSource* src)
    {
        // a new sample rate or length means a new read
        if (auto a = analysisFor (src); a != nullptr && (a->samples != src->getSampleCount() || std::abs (a->sampleRate - src->getSampleRate()) > 0.5))
            contentChanged (src, juce::ARAContentUpdateScopes::samplesAreAffected());
    }
    void sourceGoing (juce::ARAAudioSource* src)
    {
        cancel (src);
        src->removeListener (&watcher);
        { const std::lock_guard<std::mutex> l (lock); live.erase (src); analyses.erase (src); }
        ++analysisVersion;
    }

    void start (juce::ARAAudioSource* src)
    {
        cancel (src);
        auto* job = new FlatAnalysisJob (*this, src);
        jobs[src] = job;
        pool.addJob (job, true);
    }
    void cancel (juce::ARAAudioSource* src)
    {
        if (auto it = jobs.find (src); it != jobs.end())
        {
            pool.removeJob (it->second, true, 4000);
            jobs.erase (it);
        }
    }

    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withThreadName ("Flatliner ARA").withNumberOfThreads (1) };
    std::map<juce::ARAAudioSource*, FlatAnalysisJob*> jobs;       // document thread only
    mutable std::mutex lock;
    std::map<const juce::ARAAudioSource*, std::shared_ptr<const flat::Analysis>> analyses;
    std::set<const juce::ARAAudioSource*> live;

    JUCE_DECLARE_WEAK_REFERENCEABLE (FlatDocumentController)
};

inline juce::ThreadPoolJob::JobStatus FlatAnalysisJob::runJob()
{
    if (source != nullptr) source->notifyAnalysisProgressStarted();
    flat::Analyser an (rate);
    const int block = 32768;
    juce::AudioBuffer<float> buf (channels, block);
    std::vector<float> mono ((size_t) block);
    for (juce::int64 pos = 0; pos < length; pos += block)
    {
        if (shouldExit()) return jobHasFinished;
        const int n = (int) juce::jmin ((juce::int64) block, length - pos);
        // the DAW may be busy or have switched access off: give it a moment, then give up (the next
        // "access on" starts a new read)
        bool ok = false;
        for (int tries = 0; tries < 50 && ! ok && ! shouldExit(); ++tries)
        {
            ok = reader->isValid() && reader->read (&buf, 0, n, pos, true, true);
            if (! ok) juce::Thread::sleep (20);
        }
        if (! ok) return jobHasFinished;
        for (int i = 0; i < n; ++i)
        {
            float s = 0.0f;
            for (int c = 0; c < channels; ++c) s += buf.getSample (c, i);
            mono[(size_t) i] = s / (float) channels;
        }
        an.push (mono.data(), n);
        if (source != nullptr && (pos / block) % 16 == 0) source->notifyAnalysisProgressUpdated ((float) ((double) pos / (double) juce::jmax ((juce::int64) 1, length)));
    }
    if (source != nullptr) source->notifyAnalysisProgressCompleted();
    dc.analysisDone (source, an.finish());
    return jobHasFinished;
}

//==============================================================================
// Plays the clips back through the planned fader
class FlatRenderer final : public juce::ARAPlaybackRenderer
{
public:
    FlatRenderer (ARA::PlugIn::DocumentController* d, FlatDocumentController& dcIn) : juce::ARAPlaybackRenderer (d), dc (dcIn) {}
    ~FlatRenderer() override { stopPlanner(); }

    // the plugin's knobs, every block (audio thread)
    void setParams (const flat::Params& p)
    {
        const std::lock_guard<std::mutex> l (paramLock);
        auto eq = [] (float x, float y) { return std::abs (x - y) < 1.0e-6f; };
        const bool same = eq (p.targetDb, wanted.targetDb) && eq (p.rangeDb, wanted.rangeDb) && eq (p.speed, wanted.speed) && eq (p.gateDb, wanted.gateDb)
                       && p.follow == wanted.follow && p.ride == wanted.ride && eq (p.outputDb, wanted.outputDb);
        if (! same) { wanted = p; ++paramsVersion; }
    }

    void prepareToPlay (double sampleRateIn, int maxBlock, int numChannelsIn, juce::AudioProcessor::ProcessingPrecision,
                        AlwaysNonRealtime alwaysNonRealtime) override
    {
        stopPlanner();
        sampleRate = sampleRateIn; maximumSamplesPerBlock = maxBlock; numChannels = numChannelsIn;
        readers.clear();
        sources.clear();
        const bool buffered = alwaysNonRealtime == AlwaysNonRealtime::no;
        for (auto* region : getPlaybackRegions())
        {
            auto* src = region->getAudioModification()->getAudioSource();
            if (readers.count (src) > 0) continue;
            sources.push_back (src);
            auto r = std::make_unique<juce::ARAAudioSourceReader> (src);
            if (buffered)
                readers[src] = { std::make_unique<juce::BufferingAudioReader> (r.release(), *timeSlice, juce::jmax (4 * maxBlock, juce::roundToInt (2.0 * sampleRate))), true };
            else
                readers[src] = { std::move (r), false };
        }
        sourceBuffer.setSize (8, maxBlock);
        mixBuffer.setSize (juce::jmax (1, numChannels), maxBlock);
        plans = std::make_shared<PlanSet>();
        startPlanner();
    }

    void releaseResources() override { stopPlanner(); readers.clear(); }

    bool processBlock (juce::AudioBuffer<float>& buffer, juce::AudioProcessor::Realtime realtime,
                       const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept override
    {
        const int numSamples = buffer.getNumSamples();
        const auto blockStart = positionInfo.getTimeInSamples().orFallback (0);
        bool success = true, any = false;
        float outTarget;
        { const std::lock_guard<std::mutex> l (paramLock); outTarget = flat::dbToGain (wanted.outputDb); }
        const auto planSet = std::atomic_load (&plans);

        if (positionInfo.getIsPlaying())
        {
            const auto blockRange = juce::Range<juce::int64>::withStartAndLength (blockStart, numSamples);
            for (auto* region : getPlaybackRegions())
            {
                const auto playRange = region->getSampleRange (sampleRate, juce::ARAPlaybackRegion::IncludeHeadAndTail::no);
                auto range = blockRange.getIntersectionWith (playRange);
                if (range.isEmpty()) continue;
                const juce::Range<juce::int64> modRange { region->getStartInAudioModificationSamples(), region->getEndInAudioModificationSamples() };
                const auto offset = modRange.getStart() - playRange.getStart();
                range = range.getIntersectionWith (modRange.movedToStartAt (playRange.getStart()));
                if (range.isEmpty()) continue;

                auto* src = region->getAudioModification()->getAudioSource();
                auto it = readers.find (src);
                if (it == readers.end() || std::abs (src->getSampleRate() - sampleRate) > 0.5) { success = false; continue; }   // Phase 1: same rate as the song
                if (it->second.buffered) static_cast<juce::BufferingAudioReader*> (it->second.reader.get())->setReadTimeout (realtime == juce::AudioProcessor::Realtime::no ? 100 : 0);

                const int n = (int) range.getLength(), at = (int) (range.getStart() - blockStart);
                const auto srcStart = range.getStart() + offset;
                const int srcCh = juce::jlimit (1, 8, (int) src->getChannelCount());
                if (! it->second.reader->read (&sourceBuffer, 0, n, srcStart, true, true)) { success = false; continue; }

                // the planned fader, in the take's own time
                const flat::Plan* plan = nullptr;
                if (planSet != nullptr) if (auto pit = planSet->find (src); pit != planSet->end()) plan = pit->second.get();
                for (int i = 0; i < n; ++i)
                {
                    const float g = (plan != nullptr ? flat::dbToGain (plan->gainAt (srcStart + i)) : 1.0f) * outTarget;
                    for (int c = 0; c < numChannels; ++c)
                    {
                        float x;
                        if (srcCh == numChannels) x = sourceBuffer.getSample (c, i);
                        else if (srcCh == 1) x = sourceBuffer.getSample (0, i);                                     // mono clip, stereo track
                        else { x = 0; for (int k = 0; k < srcCh; ++k) x += sourceBuffer.getSample (k, i); x /= (float) srcCh; }   // stereo clip, mono track
                        x *= g;
                        if (any) buffer.addSample (c, at + i, x); else mixBuffer.setSample (c, i, x);
                    }
                }
                if (! any)
                {
                    buffer.clear();
                    for (int c = 0; c < numChannels; ++c) buffer.copyFrom (c, at, mixBuffer, c, 0, n);
                    any = true;
                }
                // where we are in the take, for the editor
                playSource.store (src); playPos.store (srcStart + n);
            }
        }
        if (! any) buffer.clear();
        return success;
    }
    using juce::ARAPlaybackRenderer::processBlock;

    // ---- for the editor and The Master
    struct PlanView { std::shared_ptr<const flat::Analysis> analysis; std::shared_ptr<const flat::Plan> plan; juce::int64 playPos = -1; };
    PlanView currentView() const
    {
        PlanView v;
        const auto set = std::atomic_load (&plans);
        const auto* playing = playSource.load();
        for (auto* src : sources)
        {
            if (playing != nullptr && src != playing) continue;
            v.analysis = dc.analysisFor (src);
            if (set != nullptr) if (auto it = set->find (src); it != set->end()) v.plan = it->second;
            if (src == playing) v.playPos = playPos.load();
            if (v.analysis != nullptr) break;
        }
        return v;
    }
    int numSources() const { return (int) sources.size(); }

private:
    using PlanSet = std::map<const juce::ARAAudioSource*, std::shared_ptr<const flat::Plan>>;

    // re-plans when the knobs or the analyses change (cheap: a 4-minute take plans in about a millisecond)
    struct Planner : juce::Thread
    {
        explicit Planner (FlatRenderer& r) : juce::Thread ("Flatliner planner"), owner (r) {}
        void run() override
        {
            int seenParams = -1, seenAnalyses = -1;
            while (! threadShouldExit())
            {
                const int pv = owner.paramsVersion.load(), av = owner.dc.analysisVersion.load();
                if (pv != seenParams || av != seenAnalyses)
                {
                    seenParams = pv; seenAnalyses = av;
                    flat::Params p;
                    { const std::lock_guard<std::mutex> l (owner.paramLock); p = owner.wanted; }
                    auto set = std::make_shared<PlanSet>();
                    for (auto* src : owner.sources)
                        if (auto a = owner.dc.analysisFor (src)) (*set)[src] = std::make_shared<const flat::Plan> (flat::planRide (*a, p));
                    std::atomic_store (&owner.plans, std::shared_ptr<const PlanSet> (set));
                }
                wait (40);
            }
        }
        FlatRenderer& owner;
    };
    void startPlanner() { planner = std::make_unique<Planner> (*this); planner->startThread(); }
    void stopPlanner() { if (planner != nullptr) { planner->stopThread (2000); planner.reset(); } }

    struct Reader { std::unique_ptr<juce::AudioFormatReader> reader; bool buffered = false; };
    struct SharedThread : juce::TimeSliceThread { SharedThread() : juce::TimeSliceThread ("Flatliner reader") { startThread(); } };

    FlatDocumentController& dc;
    juce::SharedResourcePointer<SharedThread> timeSlice;
    std::map<juce::ARAAudioSource*, Reader> readers;
    std::vector<juce::ARAAudioSource*> sources;
    juce::AudioBuffer<float> sourceBuffer, mixBuffer;
    double sampleRate = 48000.0;
    int maximumSamplesPerBlock = 512, numChannels = 2;

    std::mutex paramLock;
    flat::Params wanted;
    std::atomic<int> paramsVersion { 0 };
    std::shared_ptr<const PlanSet> plans;
    std::unique_ptr<Planner> planner;
    std::atomic<const juce::ARAAudioSource*> playSource { nullptr };
    std::atomic<juce::int64> playPos { -1 };
};

inline juce::ARAPlaybackRenderer* FlatDocumentController::doCreatePlaybackRenderer() noexcept
{
    return new FlatRenderer (getDocumentController(), *this);
}
#endif
