#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "FlatlinerEngine.h"
#include "Presets.h"
#include "CDLink.h"

class FlatlinerProcessor : public juce::AudioProcessor
{
public:
    FlatlinerProcessor();
    ~FlatlinerProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Flatliner"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;
    flat::Params currentParams() const;

    // metering, read by the editor
    flat::Engine& getEngine() { return engine; }
    bool isSidechainConnected() const { return scConnected.load(); }

    // ---- user presets (files shared by every project and DAW)
    static juce::File getUserPresetFolder();
    static juce::Array<juce::File> getUserPresets();
    static juce::File fileForPresetName (const juce::String& name);
    bool saveUserPreset (const juce::String& name);
    bool loadUserPreset (const juce::File& file);
    juce::String getCurrentPresetName() const { return currentPresetName; }
    std::atomic<int> presetVersion { 0 };

    // ---- Castle Danger Link: The Master (on the master bus) hears the whole mix; it reads how much
    //      this track's level swings and sets TARGET / RANGE / SPEED / GATE for the genre
    using Link = cdlink::PluginLink<juce::AudioProcessorValueTreeState>;
    Link& getLink() { return *link; }
    juce::String getTrackName() const { const juce::ScopedLock sl (nameLock); return trackName; }
    void updateTrackProperties (const TrackProperties& props) override { const juce::ScopedLock sl (nameLock); trackName = props.name.value_or (juce::String()); }
    juce::var levelStats() const;   // { p10, p50, p90, floorDb, seconds } of the input, since The Master's last RESET

private:
    void resetToDefaults();
    std::unique_ptr<Link> link;
    juce::String trackName;
    juce::CriticalSection nameLock;
    // input level, 100 ms RMS windows, as a histogram (0.5 dB steps, -100..0 dBFS)
    static constexpr int kHistBins = 200;
    std::array<std::atomic<int>, kHistBins> hist {};
    std::atomic<bool> histReset { false };
    double winPow = 0; int winCount = 0, winLen = 4800;
    flat::Engine engine;
    int currentProgram = 0, reportedLatency = -1;
    std::atomic<bool> scConnected { false };
    juce::String currentPresetName { "Init (Gentle Ride)" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlatlinerProcessor)
};
