#pragma once

#include "PluginProcessor.h"

class MiniSamplerWaveformView final : public juce::Component,
                                      private juce::FileDragAndDropTarget,
                                      private juce::ChangeListener
{
public:
    explicit MiniSamplerWaveformView(juce::AudioThumbnail& thumbnailToUse);
    ~MiniSamplerWaveformView() override;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

    void setTempoInfo(double tempo, int numerator, int denominator);
    void setGridDivision(int divisionId);
    void setLoopActive(bool active);
    void clearSelection();
    bool hasSelection() const noexcept;
    double getSelectionStart() const noexcept { return selectionStart; }
    double getSelectionEnd() const noexcept { return selectionEnd; }

    std::function<void(const juce::File&)> onFileDropped;
    std::function<void(double, double)> onSelectionChanged;
    std::function<void(double, double)> onCreateLoop;

private:
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void showContextMenu();
    void snapSelectionToGrid();
    double timeForX(float x) const;
    double gridLengthSeconds() const;
    void drawTempoGrid(juce::Graphics&, juce::Rectangle<float>) const;

    juce::AudioThumbnail& thumbnail;
    double tempo { 120.0 };
    int numerator { 4 };
    int denominator { 4 };
    int gridDivisionId { 3 };
    double selectionStart { 0.0 };
    double selectionEnd { 0.0 };
    bool selecting { false };
    bool loopActive { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MiniSamplerWaveformView)
};

class MiniSamplerAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                              private juce::Timer
{
public:
    explicit MiniSamplerAudioProcessorEditor(MiniSamplerAudioProcessor&);
    ~MiniSamplerAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void loadSampleFile(const juce::File& file);
    void finishSampleLoad(const juce::File& file, bool success);
    void createLoopFromSelection(double start, double end);
    void toggleLoop();

    MiniSamplerAudioProcessor& processor;
    juce::AudioFormatManager thumbnailFormatManager;
    juce::AudioThumbnailCache thumbnailCache { 5 };
    juce::AudioThumbnail thumbnail { 512, thumbnailFormatManager, thumbnailCache };
    MiniSamplerWaveformView waveform;
    juce::TextButton loadButton { "Load" };
    juce::ComboBox gridDivisionBox;
    juce::TextButton loopButton { "Create loop" };
    juce::Label sampleLabel;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::ThreadPool sampleLoadPool { 1 };
    double displayedTempo { 120.0 };
    int displayedNumerator { 4 };
    int displayedDenominator { 4 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MiniSamplerAudioProcessorEditor)
};
