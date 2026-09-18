#include "PluginEditor.h"

namespace
{
bool looksLikeAudioFile(const juce::File& file)
{
    return file.hasFileExtension("wav;aif;aiff;flac;mp3;ogg;opus") && file.existsAsFile();
}
}

MiniSamplerAudioProcessorEditor::MiniSamplerAudioProcessorEditor(MiniSamplerAudioProcessor& p)
    : AudioProcessorEditor(&p),
      processor(p),
      keyboard(processor.getKeyboardState(), juce::MidiKeyboardComponent::horizontalKeyboard)
{
    thumbnailFormatManager.registerBasicFormats();
    thumbnail.addChangeListener(this);

    setResizable(true, true);
    setResizeLimits(520, 420, 1600, 1200);
    setSize(780, 560);

    gridDivisionBox.addItem("1/16", 1);
    gridDivisionBox.addItem("1/8", 2);
    gridDivisionBox.addItem("1/4", 3);
    gridDivisionBox.addItem("1/2", 4);
    gridDivisionBox.addItem("1/1", 5);
    gridDivisionBox.setSelectedId(3, juce::dontSendNotification);
    gridDivisionBox.onChange = [this] { repaint(); };

    loadButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Choose an audio sample", juce::File{}, "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");

        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& chooser)
            {
                loadSampleFile(chooser.getResult());
            });
    };

    sampleLabel.setText("Drop an audio file here or click Load sample...", juce::dontSendNotification);
    sampleLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    sampleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(loadButton);
    addAndMakeVisible(gridDivisionBox);
    addAndMakeVisible(sampleLabel);
    addAndMakeVisible(keyboard);

    displayedTempo = processor.getProjectTempo();
    displayedNumerator = processor.getProjectTimeSignatureNumerator();
    displayedDenominator = processor.getProjectTimeSignatureDenominator();
    startTimerHz(10);
}

MiniSamplerAudioProcessorEditor::~MiniSamplerAudioProcessorEditor()
{
    stopTimer();
    thumbnail.removeChangeListener(this);
}

bool MiniSamplerAudioProcessorEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    return files.size() == 1 && looksLikeAudioFile(juce::File(files[0]));
}

void MiniSamplerAudioProcessorEditor::filesDropped(const juce::StringArray& files, int x, int y)
{
    juce::ignoreUnused(x, y);
    if (files.size() == 1)
        loadSampleFile(juce::File(files[0]));
}

void MiniSamplerAudioProcessorEditor::loadSampleFile(const juce::File& file)
{
    if (!looksLikeAudioFile(file))
    {
        sampleLabel.setText("Unsupported audio file", juce::dontSendNotification);
        return;
    }

    if (!processor.loadSample(file))
    {
        sampleLabel.setText("Could not load that audio file", juce::dontSendNotification);
        return;
    }

    thumbnail.setSource(new juce::FileInputSource(file));
    sampleLabel.setText("Loaded: " + processor.getLoadedSampleName(), juce::dontSendNotification);
    repaint();
}

void MiniSamplerAudioProcessorEditor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    repaint();
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
        repaint();
    }
}

void MiniSamplerAudioProcessorEditor::drawTempoGrid(juce::Graphics& g,
                                                     juce::Rectangle<float> area) const
{
    const auto duration = thumbnail.getTotalLength();
    if (duration <= 0.0 || displayedTempo <= 0.0)
        return;

    const auto secondsPerBeat = 60.0 / displayedTempo;
    const auto beatsPerBar = juce::jmax(1, displayedNumerator);
    double beatsPerGridLine = 1.0;

    switch (gridDivisionBox.getSelectedId())
    {
        case 1: beatsPerGridLine = 0.25; break;
        case 2: beatsPerGridLine = 0.5; break;
        case 4: beatsPerGridLine = 2.0; break;
        case 5: beatsPerGridLine = 4.0; break;
        default: break;
    }

    const auto secondsPerGridLine = secondsPerBeat * beatsPerGridLine;
    for (int line = 0; line < 100000; ++line)
    {
        const auto time = line * secondsPerGridLine;
        if (time > duration)
            break;

        const auto x = area.getX() + static_cast<float>((time / duration) * area.getWidth());
        const auto beatPosition = line * beatsPerGridLine;
        const auto isBarLine = std::fmod(beatPosition, static_cast<double>(beatsPerBar)) < 0.001;
        g.setColour(isBarLine ? juce::Colour(0x805d6cff) : juce::Colour(0x303f4675));
        g.drawVerticalLine(static_cast<int>(x), area.getY(), area.getBottom());
    }
}

void MiniSamplerAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0f0e19));

    g.setColour(juce::Colours::white);
    g.setFont(22.0f);
    g.drawText("Mini Sampler", 16, 12, getWidth() - 32, 30, juce::Justification::centredLeft);

    g.setFont(13.0f);
    g.setColour(juce::Colour(0xffa7a5bd));
    g.drawText("Host: " + juce::String(displayedTempo, 1) + " BPM   "
                   + juce::String(displayedNumerator) + "/" + juce::String(displayedDenominator),
               16, 43, getWidth() - 32, 22, juce::Justification::centredLeft);

    auto waveformArea = waveformBounds.toFloat();
    g.setColour(juce::Colour(0xff151421));
    g.fillRoundedRectangle(waveformArea, 8.0f);
    g.setColour(juce::Colour(0xff2a2940));
    g.drawRoundedRectangle(waveformArea, 8.0f, 1.0f);

    auto contentArea = waveformArea.reduced(10.0f);
    drawTempoGrid(g, contentArea);

    if (thumbnail.getTotalLength() > 0.0)
    {
        g.setColour(juce::Colour(0xff45d6c7));
        thumbnail.drawChannels(g, contentArea.toNearestInt(), 0.0,
                               thumbnail.getTotalLength(), 1.0f);
    }
    else
    {
        g.setColour(juce::Colour(0xff77748f));
        g.setFont(15.0f);
        g.drawText("Drop audio here or click Load sample...", contentArea.toNearestInt(),
                   juce::Justification::centred);
    }
}

void MiniSamplerAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced(16);
    bounds.removeFromTop(68);

    auto keyboardArea = bounds.removeFromBottom(76);
    auto controls = bounds.removeFromBottom(42);
    waveformBounds = bounds;

    auto gridArea = controls.removeFromRight(100);
    gridDivisionBox.setBounds(gridArea);
    controls.removeFromRight(14);
    loadButton.setBounds(controls.removeFromLeft(150));
    controls.removeFromLeft(14);
    sampleLabel.setBounds(controls);
    keyboard.setBounds(keyboardArea);
}
