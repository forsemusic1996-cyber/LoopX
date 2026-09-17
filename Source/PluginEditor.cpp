#include "PluginEditor.h"

MiniSamplerAudioProcessorEditor::MiniSamplerAudioProcessorEditor(MiniSamplerAudioProcessor& p)
    : AudioProcessorEditor(&p),
      processor(p),
      keyboard(processor.getKeyboardState(), juce::MidiKeyboardComponent::horizontalKeyboard)
{
    setSize(640, 260);

    addAndMakeVisible(loadButton);
    loadButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Choose an audio sample",
            juce::File{},
            "*.wav;*.aif;*.aiff;*.flac");

        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();
                if (file.existsAsFile() && processor.loadSample(file))
                    sampleLabel.setText("Loaded: " + processor.getLoadedSampleName(), juce::dontSendNotification);
                else if (file.existsAsFile())
                    sampleLabel.setText("Could not load that audio file", juce::dontSendNotification);
            });
    };

    sampleLabel.setText("Loaded: " + processor.getLoadedSampleName(), juce::dontSendNotification);
    sampleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(sampleLabel);

    addAndMakeVisible(keyboard);
}

void MiniSamplerAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff20242b));
    g.setColour(juce::Colours::white);
    g.setFont(22.0f);
    g.drawText("Mini Sampler", 20, 16, getWidth() - 40, 30, juce::Justification::centredLeft);
    g.setFont(14.0f);
    g.setColour(juce::Colours::lightgrey);
    g.drawText("Load a WAV/AIFF/FLAC and play it with MIDI or the keyboard below.",
               20, 48, getWidth() - 40, 24, juce::Justification::centredLeft);
}

void MiniSamplerAudioProcessorEditor::resized()
{
    loadButton.setBounds(20, 84, 150, 34);
    sampleLabel.setBounds(185, 84, getWidth() - 205, 34);
    keyboard.setBounds(20, 145, getWidth() - 40, 80);
}
