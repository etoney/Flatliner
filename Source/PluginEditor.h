#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "Rebel.h"

class FlatLookAndFeel : public juce::LookAndFeel_V4
{
public:
    FlatLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int h) override;
    juce::Font getLabelFont (juce::Label& l) override { return l.getFont(); }
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool hi, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool hi, bool down) override;
    juce::Image rustDisc;
};

// Segmented row of chunky rusty buttons bound to a choice / bool parameter
class Selector : public juce::Component, public juce::SettableTooltipClient
{
public:
    Selector (juce::RangedAudioParameter& p, juce::StringArray labels, juce::StringArray subLabels = {});
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
    int getIndex() const { return index; }
    std::function<void()> onChange;
private:
    int segmentAt (juce::Point<int>) const;
    juce::StringArray labels, subs;
    juce::ParameterAttachment att;
    int index = 0, hover = -1;
};

// Round iron gauge in the header: the needle is the virtual fader (centre = 0 dB, left = cut, right = boost)
class FaderGauge : public juce::Component
{
public:
    void paint (juce::Graphics&) override;
    void set (float faderDb, float rangeDb, bool riding) { fader = faderDb; range = rangeDb; ride = riding; }
private:
    float fader = 0.0f, range = 6.0f;
    bool ride = true;
};

// THE LINE: scrolling scope. Dim trace = the vocal as it comes in, bright trace = after the fader. The goal: a flat line on the target.
class ScopeDisplay : public juce::Component
{
public:
    explicit ScopeDisplay (FlatlinerProcessor& p) : proc (p) { voice.fill (-100.0f); ridden.fill (-100.0f); }
    void paint (juce::Graphics&) override;
    void push (float voiceDb, float riddenDb);
private:
    void paintWholeTake (juce::Graphics&, juce::Rectangle<float> r, juce::Rectangle<float> plot, const FlatlinerProcessor::AraView&);
    static constexpr int N = 300;   // about 10 seconds at 30 fps
    FlatlinerProcessor& proc;
    std::array<float, N> voice, ridden;
};

// VOICE / FADER / OUT meters and the fader read-out
class MeterPanel : public juce::Component
{
public:
    void paint (juce::Graphics&) override;
    float in = 0, out = 0, fader = 0, range = 6;
    bool ride = true;
};

// In-window modal dialog (message, confirm, or text entry), drawn over the editor
class FlatDialog : public juce::Component
{
public:
    FlatDialog (const juce::String& title, const juce::String& message, bool withTextField,
                const juce::String& initialText, juce::StringArray buttons,
                std::function<void (int buttonIndex, const juce::String& text)> onClose);
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void parentHierarchyChanged() override;
    void mouseDown (const juce::MouseEvent&) override {}
private:
    void finish (int index);
    juce::String title, message;
    juce::TextEditor field;
    bool hasField;
    juce::OwnedArray<juce::TextButton> buttons;
    std::function<void (int, const juce::String&)> onClose;
    juce::Rectangle<int> panel;
};

class FlatlinerEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit FlatlinerEditor (FlatlinerProcessor&);
    ~FlatlinerEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    // used by the headless snapshot test
    juce::TextButton& getSaveButton() { return saveButton; }

private:
    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };
    struct Plate { juce::String title; juce::Rectangle<int> bounds; bool hot = false; };

    Knob& makeKnob (const juce::String& id, const juce::String& text);
    void placeKnob (Knob&, juce::Rectangle<int>);
    void timerCallback() override;
    void buildBackdrop();

    void refreshPresetBox();
    void stepPreset (int delta);
    void promptSave();
    void confirmDelete();
    void showDialog (const juce::String& title, const juce::String& message, bool withTextField, const juce::String& initialText,
                     juce::StringArray buttons, std::function<void (int, const juce::String&)> onResult);
    void showMessage (const juce::String& title, const juce::String& text);

    FlatlinerProcessor& proc;
    FlatLookAndFeel lnf;
    std::vector<std::unique_ptr<Knob>> knobs;
    Knob *targetKnob = nullptr, *rangeKnob = nullptr, *speedKnob = nullptr, *gateKnob = nullptr,
         *lookKnob = nullptr, *musicKnob = nullptr, *outKnob = nullptr;
    Selector rideSel;
    FaderGauge gauge;
    juce::TooltipWindow tooltips { this, 700 };
    ScopeDisplay scope;
    MeterPanel meters;

    juce::ComboBox presetBox;
    juce::TextButton prevButton { "<" }, nextButton { ">" }, saveButton { "SAVE" }, deleteButton { "DELETE" };
    juce::Array<juce::File> userFiles;
    int lastPresetVersion = -1;
    std::unique_ptr<FlatDialog> dialog;
    static constexpr int kUserIdBase = 1001;

    std::vector<Plate> plates;
    juce::Rectangle<int> logoArea, badgeArea, crestArea, meterArea, presetArea, stripeArea, scArea, latArea;
    juce::Image backdrop, logoImage;
    float inMeter = 0, outMeter = 0, flicker = 0;
    int lastStatusKey = -1, linkTick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlatlinerEditor)
    cdrebel::Button rebel { proc.apvts };   // REBEL: The Master hands off
    juce::ToggleButton followButton { "SECTIONS" };   // FOLLOW SECTIONS   // the line follows the vocal's own level over the last few seconds
    juce::AudioProcessorValueTreeState::ButtonAttachment followAtt { proc.apvts, "follow", followButton };
};
