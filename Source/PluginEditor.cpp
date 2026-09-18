#include "PluginEditor.h"

namespace
{
bool looksLikeAudioFile(const juce::File& file)
{
    return file.hasFileExtension("wav;aif;aiff;flac;mp3;ogg;opus") && file.existsAsFile();
}

class SampleLoadJob final : public juce::ThreadPoolJob
{
public:
    SampleLoadJob(const juce::File& fileToLoad,
                  MiniSamplerAudioProcessor& processorToUse,
                  std::function<void(const juce::File&, bool)> completionToUse)
        : ThreadPoolJob("MiniSampler sample loader"),
          file(fileToLoad),
          processor(processorToUse),
          completion(std::move(completionToUse))
    {
    }

    JobStatus runJob() override
    {
        const auto success = processor.loadSample(file);
        juce::MessageManager::callAsync([file = file, success, completion = std::move(completion)]() mutable
        {
            completion(file, success);
        });
        return jobHasFinished;
    }

private:
    juce::File file;
    MiniSamplerAudioProcessor& processor;
    std::function<void(const juce::File&, bool)> completion;
};
}

MiniSamplerWaveformView::MiniSamplerWaveformView(juce::AudioThumbnail& thumbnailToUse)
    : thumbnail(thumbnailToUse)
{
    thumbnail.addChangeListener(this);
    setOpaque(true);
}

MiniSamplerWaveformView::~MiniSamplerWaveformView()
{
    thumbnail.removeChangeListener(this);
}

bool MiniSamplerWaveformView::isInterestedInFileDrag(const juce::StringArray& files)
{
    if (files.size() != 1)
        return false;

    const juce::File file(files[0]);
    return file.existsAsFile() || file.hasFileExtension("wav;aif;aiff;flac;mp3;ogg;opus");
}

void MiniSamplerWaveformView::filesDropped(const juce::StringArray& files, int x, int y)
{
    juce::ignoreUnused(x, y);
    if (files.size() == 1 && onFileDropped != nullptr)
        onFileDropped(juce::File(files[0]));
}

void MiniSamplerWaveformView::changeListenerCallback(juce::ChangeBroadcaster*)
{
    repaint();
}

double MiniSamplerWaveformView::gridLengthSeconds() const
{
    const auto secondsPerBeat = 60.0 / juce::jmax(1.0, tempo);
    switch (gridDivisionId)
    {
        case 1: return secondsPerBeat * 0.25;
        case 2: return secondsPerBeat * 0.5;
        case 4: return secondsPerBeat * 2.0;
        case 5: return secondsPerBeat * 4.0;
        default: return secondsPerBeat;
    }
}

double MiniSamplerWaveformView::timeForX(float x) const
{
    const auto area = getLocalBounds().toFloat().reduced(10.0f);
    const auto duration = thumbnail.getTotalLength();
    if (duration <= 0.0 || area.getWidth() <= 0.0f)
        return 0.0;

    return juce::jlimit(0.0, duration,
                        static_cast<double>((x - area.getX()) / area.getWidth()) * duration);
}

void MiniSamplerWaveformView::snapSelectionToGrid()
{
    const auto duration = thumbnail.getTotalLength();
    const auto grid = gridLengthSeconds();
    if (duration <= 0.0 || grid <= 0.0)
        return;

    const auto start = juce::jmin(selectionStart, selectionEnd);
    const auto end = juce::jmax(selectionStart, selectionEnd);
    selectionStart = juce::jlimit(0.0, duration, std::floor(start / grid) * grid);
    selectionEnd = juce::jlimit(0.0, duration, std::ceil(end / grid) * grid);

    if (selectionEnd <= selectionStart)
        selectionEnd = juce::jmin(duration, selectionStart + grid);
}

bool MiniSamplerWaveformView::hasSelection() const noexcept
{
    return thumbnail.getTotalLength() > 0.0 && selectionEnd > selectionStart;
}

void MiniSamplerWaveformView::setTempoInfo(double newTempo, int newNumerator, int newDenominator)
{
    tempo = newTempo;
    numerator = newNumerator;
    denominator = newDenominator;
    repaint();
}

void MiniSamplerWaveformView::setGridDivision(int divisionId)
{
    gridDivisionId = divisionId;
    if (hasSelection())
    {
        snapSelectionToGrid();
        if (onSelectionChanged != nullptr)
            onSelectionChanged(selectionStart, selectionEnd);
    }
    repaint();
}

void MiniSamplerWaveformView::setLoopActive(bool active)
{
    loopActive = active;
    repaint();
}

void MiniSamplerWaveformView::clearSelection()
{
    selectionStart = 0.0;
    selectionEnd = 0.0;
    selecting = false;
    loopActive = false;
    repaint();
}

void MiniSamplerWaveformView::showContextMenu()
{
    juce::PopupMenu menu;
    menu.addItem(1, "Create loop", hasSelection());
    menu.showMenuAsync(juce::PopupMenu::Options(), [this](int result)
    {
        if (result == 1 && hasSelection() && onCreateLoop != nullptr)
            onCreateLoop(selectionStart, selectionEnd);
    });
}

void MiniSamplerWaveformView::mouseDown(const juce::MouseEvent& event)
{
    if (event.mods.isRightButtonDown())
    {
        showContextMenu();
        return;
    }

    if (!event.mods.isLeftButtonDown() || thumbnail.getTotalLength() <= 0.0)
        return;

    selecting = true;
    selectionStart = timeForX(static_cast<float>(event.x));
    selectionEnd = selectionStart;
    loopActive = false;
    repaint();
}

void MiniSamplerWaveformView::mouseDrag(const juce::MouseEvent& event)
{
    if (selecting)
    {
        selectionEnd = timeForX(static_cast<float>(event.x));
        repaint();
    }
}

void MiniSamplerWaveformView::mouseUp(const juce::MouseEvent& event)
{
    juce::ignoreUnused(event);
    if (!selecting)
        return;

    selecting = false;
    snapSelectionToGrid();
    if (onSelectionChanged != nullptr)
        onSelectionChanged(selectionStart, selectionEnd);
    repaint();
}

void MiniSamplerWaveformView::drawTempoGrid(juce::Graphics& g,
                                             juce::Rectangle<float> area) const
{
    const auto duration = thumbnail.getTotalLength();
    if (duration <= 0.0 || tempo <= 0.0)
        return;

    const auto step = gridLengthSeconds();
    const auto barLength = (60.0 / tempo) * juce::jmax(1, numerator);
    for (int line = 0; line < 100000; ++line)
    {
        const auto time = line * step;
        if (time > duration)
            break;

        const auto x = area.getX() + static_cast<float>((time / duration) * area.getWidth());
        const auto isBarLine = std::fmod(time, barLength) < 0.0001;
        g.setColour(isBarLine ? juce::Colour(0xffa98cff) : juce::Colour(0xff416b91));
        g.drawVerticalLine(static_cast<int>(x), area.getY(), area.getBottom());
    }
}

void MiniSamplerWaveformView::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff11101b));
    auto area = getLocalBounds().toFloat().reduced(10.0f);

    g.setColour(juce::Colour(0xff27253b));
    g.fillRoundedRectangle(area, 7.0f);
    g.setColour(juce::Colour(0xff575276));
    g.drawRoundedRectangle(area, 7.0f, 1.0f);

    auto content = area.reduced(8.0f);
    drawTempoGrid(g, content);

    if (hasSelection())
    {
        const auto duration = thumbnail.getTotalLength();
        const auto x1 = content.getX() + static_cast<float>((selectionStart / duration) * content.getWidth());
        const auto x2 = content.getX() + static_cast<float>((selectionEnd / duration) * content.getWidth());
        g.setColour(loopActive ? juce::Colour(0x7045d6c7) : juce::Colour(0x604f8cff));
        g.fillRect(juce::Rectangle<float>(x1, content.getY(), x2 - x1, content.getHeight()));
    }

    if (thumbnail.getTotalLength() > 0.0)
    {
        g.setColour(juce::Colour(0xff55e3d1));
        thumbnail.drawChannels(g, content.toNearestInt(), 0.0, thumbnail.getTotalLength(), 1.0f);
    }
    else
    {
        g.setColour(juce::Colour(0xffaaa5c4));
        g.setFont(15.0f);
        g.drawText("Drop an audio file here or click Load", content.toNearestInt(),
                   juce::Justification::centred);
    }

    if (hasSelection())
    {
        const auto duration = thumbnail.getTotalLength();
        const auto x1 = content.getX() + static_cast<float>((selectionStart / duration) * content.getWidth());
        const auto x2 = content.getX() + static_cast<float>((selectionEnd / duration) * content.getWidth());
        g.setColour(loopActive ? juce::Colours::white : juce::Colour(0xffffd166));
        g.drawVerticalLine(static_cast<int>(x1), content.getY(), content.getBottom());
        g.drawVerticalLine(static_cast<int>(x2), content.getY(), content.getBottom());
        g.setFont(12.0f);
        g.drawText(loopActive ? "LOOP" : "Right-click: Create loop",
                   juce::Rectangle<int>(static_cast<int>(x1) + 4, content.getY() + 4,
                                        juce::jmax(80, static_cast<int>(x2 - x1) - 8), 18),
                   juce::Justification::centredLeft);
    }
}

MiniSamplerAudioProcessorEditor::MiniSamplerAudioProcessorEditor(MiniSamplerAudioProcessor& p)
    : AudioProcessorEditor(&p),
      processor(p),
      waveform(thumbnail)
{
    thumbnailFormatManager.registerBasicFormats();
    setResizable(true, true);
    setResizeLimits(480, 300, 1600, 1000);
    setSize(780, 430);

    gridDivisionBox.addItem("1/16", 1);
    gridDivisionBox.addItem("1/8", 2);
    gridDivisionBox.addItem("1/4", 3);
    gridDivisionBox.addItem("1/2", 4);
    gridDivisionBox.addItem("1/1", 5);
    gridDivisionBox.setSelectedId(3, juce::dontSendNotification);
    gridDivisionBox.onChange = [this]
    {
        waveform.setGridDivision(gridDivisionBox.getSelectedId());
    };

    loadButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Choose an audio sample", juce::File{}, "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.opus");
        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& chooser)
            {
                loadSampleFile(chooser.getResult());
            });
    };

    loopButton.onClick = [this] { toggleLoop(); };
    loopButton.setEnabled(false);

    sampleLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    sampleLabel.setJustificationType(juce::Justification::centredLeft);
    sampleLabel.setText("No sample loaded", juce::dontSendNotification);

    waveform.onFileDropped = [this](const juce::File& file) { loadSampleFile(file); };
    waveform.onSelectionChanged = [this](double, double)
    {
        loopButton.setEnabled(waveform.hasSelection());
        if (!processor.isLooping())
            loopButton.setButtonText("Create loop");
    };
    waveform.onCreateLoop = [this](double start, double end) { createLoopFromSelection(start, end); };

    addAndMakeVisible(waveform);
    addAndMakeVisible(loadButton);
    addAndMakeVisible(gridDivisionBox);
    addAndMakeVisible(loopButton);
    addAndMakeVisible(sampleLabel);

    displayedTempo = processor.getProjectTempo();
    displayedNumerator = processor.getProjectTimeSignatureNumerator();
    displayedDenominator = processor.getProjectTimeSignatureDenominator();
    waveform.setTempoInfo(displayedTempo, displayedNumerator, displayedDenominator);
    waveform.setGridDivision(gridDivisionBox.getSelectedId());
    startTimerHz(10);
}

MiniSamplerAudioProcessorEditor::~MiniSamplerAudioProcessorEditor()
{
    stopTimer();
    sampleLoadPool.removeAllJobs(true, 3000);
}

void MiniSamplerAudioProcessorEditor::loadSampleFile(const juce::File& file)
{
    if (!looksLikeAudioFile(file))
    {
        sampleLabel.setText("Unsupported file - drop a local audio file", juce::dontSendNotification);
        return;
    }

    processor.stopLoop();
    waveform.clearSelection();
    loopButton.setButtonText("Create loop");
    loopButton.setEnabled(false);
    sampleLabel.setText("Loading...", juce::dontSendNotification);

    juce::Component::SafePointer<MiniSamplerAudioProcessorEditor> safeThis(this);
    sampleLoadPool.addJob(new SampleLoadJob(file, processor,
        [safeThis](const juce::File& loadedFile, bool success)
        {
            if (safeThis != nullptr)
                safeThis->finishSampleLoad(loadedFile, success);
        }), true);
}

void MiniSamplerAudioProcessorEditor::finishSampleLoad(const juce::File& file, bool success)
{
    if (!success)
    {
        sampleLabel.setText("Could not load that audio file", juce::dontSendNotification);
        return;
    }

    thumbnail.setSource(new juce::FileInputSource(file));
    sampleLabel.setText("Loaded: " + processor.getLoadedSampleName(), juce::dontSendNotification);
    waveform.clearSelection();
    repaint();
}

void MiniSamplerAudioProcessorEditor::createLoopFromSelection(double start, double end)
{
    processor.setLoopSelection(start, end);
    loopButton.setButtonText("Stop loop");
    loopButton.setEnabled(true);
    waveform.setLoopActive(true);
}

void MiniSamplerAudioProcessorEditor::toggleLoop()
{
    if (processor.isLooping())
    {
        processor.stopLoop();
        loopButton.setButtonText("Create loop");
        waveform.setLoopActive(false);
    }
    else if (waveform.hasSelection())
    {
        createLoopFromSelection(waveform.getSelectionStart(), waveform.getSelectionEnd());
    }
}

void MiniSamplerAudioProcessorEditor::timerCallback()
{
    const auto tempo = processor.getProjectTempo();
    const auto numerator = processor.getProjectTimeSignatureNumerator();
    const auto denominator = processor.getProjectTimeSignatureDenominator();

    if (tempo != displayedTempo || numerator != displayedNumerator || denominator != displayedDenominator)
    {
        displayedTempo = tempo;
        displayedNumerator = numerator;
        displayedDenominator = denominator;
        waveform.setTempoInfo(displayedTempo, displayedNumerator, displayedDenominator);
        repaint();
    }
}

void MiniSamplerAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0f0e19));
    g.setColour(juce::Colours::white);
    g.setFont(18.0f);
    g.drawText("Mini Sampler", 12, 4, 140, 24, juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xffb7b1d0));
    g.setFont(12.0f);
    g.drawText("Host " + juce::String(displayedTempo, 1) + " BPM  "
                   + juce::String(displayedNumerator) + "/" + juce::String(displayedDenominator),
               160, 4, getWidth() - 172, 24, juce::Justification::centredRight);
}

void MiniSamplerAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced(10);
    bounds.removeFromTop(28);
    auto controls = bounds.removeFromBottom(30);
    waveform.setBounds(bounds);

    loadButton.setBounds(controls.removeFromLeft(68));
    controls.removeFromLeft(6);
    gridDivisionBox.setBounds(controls.removeFromLeft(66));
    controls.removeFromLeft(6);
    loopButton.setBounds(controls.removeFromLeft(98));
    controls.removeFromLeft(8);
    sampleLabel.setBounds(controls);
}
