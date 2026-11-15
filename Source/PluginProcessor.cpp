#include "PluginProcessor.h"
#include "PluginEditor.h"

#ifndef JucePlugin_VersionString
 #define JucePlugin_VersionString "dev"
#endif

FlatlinerProcessor::FlatlinerProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                                       .withInput ("Music (Sidechain)", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "Flatliner", createLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout FlatlinerProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    auto dbAttr = AudioParameterFloatAttributes().withLabel ("dB")
        .withStringFromValueFunction ([] (float v, int) { return (v > 0.04f ? "+" : "") + String (v, 1) + " dB"; });
    auto pctAttr = AudioParameterFloatAttributes().withLabel ("%")
        .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + "%"; });
    auto msAttr = AudioParameterFloatAttributes().withLabel ("ms")
        .withStringFromValueFunction ([] (float v, int) { return roundToInt (v) == 0 ? String ("OFF") : String (roundToInt (v)) + " ms"; });

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "target", 1 },    "Target",    NormalisableRange<float> (-40.0f, 0.0f, 0.1f),  -18.0f, dbAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "range", 1 },     "Range",     NormalisableRange<float> (0.0f, 18.0f, 0.1f),     6.0f, dbAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "speed", 1 },     "Speed",     NormalisableRange<float> (0.0f, 100.0f, 1.0f),   50.0f, pctAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "gate", 1 },      "Gate",      NormalisableRange<float> (-80.0f, -20.0f, 0.1f), -55.0f, dbAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "lookahead", 1 }, "Lookahead", NormalisableRange<float> (0.0f, 50.0f, 1.0f),    10.0f, msAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "music", 1 },     "Music",     NormalisableRange<float> (0.0f, 100.0f, 1.0f),    0.0f, pctAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 },    "Output",    NormalisableRange<float> (-12.0f, 12.0f, 0.1f),   0.0f, dbAttr));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "ride", 1 },      "Ride",      true));
    return { p.begin(), p.end() };
}

flat::Params FlatlinerProcessor::currentParams() const
{
    auto v = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    flat::Params p;
    p.targetDb = v ("target");
    p.rangeDb = v ("range");
    p.speed = v ("speed") * 0.01f;
    p.gateDb = v ("gate");
    p.lookaheadMs = v ("lookahead");
    p.music = v ("music") * 0.01f;
    p.outputDb = v ("output");
    p.ride = v ("ride") > 0.5f;
    return p;
}

bool FlatlinerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo()) return false;
    const auto sc = layouts.getChannelSet (true, 1);
    return sc.isDisabled() || sc == juce::AudioChannelSet::mono() || sc == juce::AudioChannelSet::stereo();
}

void FlatlinerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    const auto p = currentParams();
    engine.setParams (p, true);
    reportedLatency = engine.getLatency();
    setLatencySamples (reportedLatency);
}

void FlatlinerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    auto mainBus = getBusBuffer (buffer, true, 0);
    const int chs = mainBus.getNumChannels();

    int scChs = 0;
    juce::AudioBuffer<float> scBus;
    if (auto* b = getBus (true, 1); b != nullptr && b->isEnabled())
    {
        scBus = getBusBuffer (buffer, true, 1);
        scChs = scBus.getNumChannels();
    }
    scConnected.store (scChs > 0);

    engine.setParams (currentParams());
    engine.process (mainBus.getArrayOfWritePointers(), chs, scChs > 0 ? scBus.getArrayOfReadPointers() : nullptr, scChs, n);
    if (engine.getLatency() != reportedLatency)
    {
        reportedLatency = engine.getLatency();
        setLatencySamples (reportedLatency);
    }
}

//==============================================================================
int FlatlinerProcessor::getNumPrograms() { return (int) flat::factoryPresets().size(); }

const juce::String FlatlinerProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, getNumPrograms()) ? flat::factoryPresets()[(size_t) index].name : "";
}

void FlatlinerProcessor::resetToDefaults()
{
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            rp->setValueNotifyingHost (rp->getDefaultValue());
}

void FlatlinerProcessor::setCurrentProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, getNumPrograms())) return;
    currentProgram = index;
    currentPresetName = flat::factoryPresets()[(size_t) index].name;
    resetToDefaults();
    for (auto& [id, v] : flat::factoryPresets()[(size_t) index].values)
        if (auto* rp = apvts.getParameter (id))
            rp->setValueNotifyingHost (rp->convertTo0to1 (v));
    ++presetVersion;
}

juce::File FlatlinerProcessor::getUserPresetFolder()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory); // ~/Library on macOS, %APPDATA% on Windows
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("Castle Danger").getChildFile ("Flatliner").getChildFile ("Presets");
}

juce::Array<juce::File> FlatlinerProcessor::getUserPresets()
{
    auto files = getUserPresetFolder().findChildFiles (juce::File::findFiles, false, "*.flatliner");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
               { return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension()) < 0; });
    return files;
}

juce::File FlatlinerProcessor::fileForPresetName (const juce::String& name)
{
    return getUserPresetFolder().getChildFile (juce::File::createLegalFileName (name.trim()) + ".flatliner");
}

bool FlatlinerProcessor::saveUserPreset (const juce::String& rawName)
{
    const auto name = rawName.trim();
    if (name.isEmpty()) return false;
    const auto file = fileForPresetName (name);
    if (! file.getParentDirectory().createDirectory()) return false;

    juce::XmlElement xml ("FlatlinerPreset");
    xml.setAttribute ("name", name);
    xml.setAttribute ("pluginVersion", JucePlugin_VersionString);
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            auto* e = xml.createNewChildElement ("PARAM");
            e->setAttribute ("id", rp->getParameterID());
            e->setAttribute ("value", (double) rp->convertFrom0to1 (rp->getValue()));
        }
    if (! xml.writeTo (file)) return false;
    currentPresetName = name;
    ++presetVersion;
    return true;
}

bool FlatlinerProcessor::loadUserPreset (const juce::File& file)
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("FlatlinerPreset")) return false;
    resetToDefaults(); // so parameters added in later versions behave sensibly
    for (auto* e : xml->getChildWithTagNameIterator ("PARAM"))
        if (auto* rp = apvts.getParameter (e->getStringAttribute ("id")))
            rp->setValueNotifyingHost (rp->convertTo0to1 ((float) e->getDoubleAttribute ("value")));
    currentPresetName = xml->getStringAttribute ("name", file.getFileNameWithoutExtension());
    ++presetVersion;
    return true;
}

void FlatlinerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("program", currentProgram, nullptr);
    state.setProperty ("presetName", currentPresetName, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void FlatlinerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto vt = juce::ValueTree::fromXml (*xml);
            currentProgram = vt.getProperty ("program", 0);
            currentPresetName = vt.getProperty ("presetName", getProgramName (currentProgram)).toString();
            apvts.replaceState (vt);
            ++presetVersion;
        }
}

juce::AudioProcessorEditor* FlatlinerProcessor::createEditor() { return new FlatlinerEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new FlatlinerProcessor(); }
