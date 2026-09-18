#pragma once
#include "PluginProcessor.h"

inline juce::PopupMenu::Options miniSamplerMenuOptions(juce::Component& editor, juce::Point<int> screenPoint)
{
    return juce::PopupMenu::Options().withTargetComponent(&editor)
        .withTargetScreenArea({ screenPoint.x, screenPoint.y, 1, 1 })
        .withParentComponent(&editor).withStandardItemHeight(22);
}

class MiniSamplerWaveformView final : public juce::Component,
                                     public juce::FileDragAndDropTarget,
                                     public juce::TextDragAndDropTarget,
                                     private juce::Timer
{
public:
    explicit MiniSamplerWaveformView(MiniSamplerAudioProcessor&);
    ~MiniSamplerWaveformView() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool isInterestedInFileDrag(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray&, int, int) override;
    void fileDragEnter(const juce::StringArray&, int, int) override;
    void fileDragExit(const juce::StringArray&) override;
    bool isInterestedInTextDrag(const juce::String&) override;
    void textDropped(const juce::String&, int, int) override;
    void textDragEnter(const juce::String&, int, int) override { dragOver = true; repaint(); }
    void textDragExit(const juce::String&) override { dragOver = false; repaint(); }
    void applySelection();
    void setMusicalLength(double beats);
    void scaleLength(double factor);
    void moveLoop(double seconds);
    void resetZoom();
    void toggleStart();
    bool isEditingStart() const { return editingStart; }
    void refreshFromProcessor() { refresh(); repaint(); }
    bool stereo = false;
    bool brightGrid = false;
    std::function<void()> onChanged;
private:
    void timerCallback() override;
    void refresh();
    void rebuildWaveCache();
    void commit(double, double, double beats = 0.0);
    double snapTime(double) const;
    double timeForX(float) const;
    float xForTime(double) const;
    double gridSeconds() const;
    double duration() const;
    const MiniSamplerSample* drawingSample() const;
    int hitTestTool(float x, float y) const;
    double visibleLength() const;
    juce::Rectangle<float> waveArea() const;
    MiniSamplerAudioProcessor& processor;
    MiniSamplerAudioProcessor::ViewState state;
    juce::Image waveCache;
    bool dirtyCache = true, dragOver = false, pendingSelection = false;
    double lastResize = 0.0, viewStart = 0.0, zoom = 1.0;
    double selectionStart = 0.0, selectionEnd = 0.0, cursor = -1.0;
    double dragStart = 0.0, dragLoopStart = 0.0, dragLoopEnd = 0.0, dragViewStart = 0.0;
    int dragMode = 0;
    bool editingStart = false;
    double draftStart = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MiniSamplerWaveformView)
};

class MiniSamplerAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                             public juce::FileDragAndDropTarget,
                                             public juce::TextDragAndDropTarget,
                                             private juce::Timer
{
public:
    explicit MiniSamplerAudioProcessorEditor(MiniSamplerAudioProcessor&);
    ~MiniSamplerAudioProcessorEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override { return waveform.isInterestedInFileDrag(files); }
    void filesDropped(const juce::StringArray& files, int x, int y) override { waveform.filesDropped(files, x, y); }
    void fileDragEnter(const juce::StringArray& files, int x, int y) override { waveform.fileDragEnter(files, x, y); }
    void fileDragExit(const juce::StringArray& files) override { waveform.fileDragExit(files); }
    bool isInterestedInTextDrag(const juce::String& text) override { return waveform.isInterestedInTextDrag(text); }
    void textDropped(const juce::String& text, int x, int y) override { waveform.textDropped(text, x, y); }
    void textDragEnter(const juce::String& text, int x, int y) override { waveform.textDragEnter(text, x, y); }
    void textDragExit(const juce::String& text) override { waveform.textDragExit(text); }
private:
    struct Tool { int id; juce::String text; juce::Rectangle<int> rect; bool active; };
    void timerCallback() override;
    void layoutTools();
    void invoke(int, bool rightClick);
    void chooseFile();
    void showBpm();
    void menuFor(int);
    void handleMenu(int tool, int result);
    MiniSamplerAudioProcessor& processor;
    MiniSamplerWaveformView waveform;
    MiniSamplerAudioProcessor::ViewState state;
    std::vector<Tool> tools;
    std::unique_ptr<juce::FileChooser> fileChooser;
    double lastTempo = 120.0;
    bool lastPlaying = false;
    juce::Point<int> menuPosition;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MiniSamplerAudioProcessorEditor)
};
