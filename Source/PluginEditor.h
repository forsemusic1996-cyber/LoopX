#pragma once

#include "PluginProcessor.h"

class MiniSamplerAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                              private juce::FileDragAndDropTarget,
                                              private juce::ChangeListener,
                                              private juce::Timer
{
public:
    explicit MiniSamplerAudioProcessorEditor(MiniSamplerAudioProcessor&);
    ~MiniSamplerAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void loadSampleFile(const juce::File& file);
    void drawTempoGrid(juce::Graphics&, juce::Rectangle<float>) const;

    MiniSamplerAudioProcessor& processor;
    juce::TextButton loadButton { "Load sample..." };
    juce::Label sampleLabel;
    juce::MidiKeyboardComponent keyboard;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::AudioFormatManager thumbnailFormatManager;
    juce::AudioThumbnailCache thumbnailCache { 5 };
    juce::AudioThumbnail thumbnail { 512, thumbnailFormatManager, thumbnailCache };
    juce::Rectangle<int> waveformBounds;
    double displayedTempo { 120.0 };
    int displayedNumerator { 4 };
    int displayedDenominator { 4 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MiniSamplerAudioProcessorEditor)
};
