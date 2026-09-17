#pragma once

#include "PluginProcessor.h"

class MiniSamplerAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit MiniSamplerAudioProcessorEditor(MiniSamplerAudioProcessor&);
    ~MiniSamplerAudioProcessorEditor() override = default;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    MiniSamplerAudioProcessor& processor;
    juce::TextButton loadButton { "Load sample..." };
    juce::Label sampleLabel;
    juce::MidiKeyboardComponent keyboard;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MiniSamplerAudioProcessorEditor)
};
