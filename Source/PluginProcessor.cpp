#include "PluginProcessor.h"
#include "PluginEditor.h"

MiniSamplerAudioProcessor::MiniSamplerAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    formatManager.registerBasicFormats();

    for (auto i = 0; i < 16; ++i)
        synth.addVoice(new juce::SamplerVoice());

    juce::AudioBuffer<float> generatedSample(1, 22050);
    auto* samples = generatedSample.getWritePointer(0);
    for (int i = 0; i < generatedSample.getNumSamples(); ++i)
    {
        const auto t = static_cast<double>(i) / 44100.0;
        const auto envelope = std::exp(-t * 7.0);
        samples[i] = static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 440.0 * t) * envelope);
    }

    juce::MemoryOutputStream wavData;
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(&wavData, 44100.0, 1, 16, {}, 0));

    if (writer != nullptr)
    {
        writer->writeFromAudioSampleBuffer(generatedSample, 0, generatedSample.getNumSamples());
        writer.reset();

        juce::MemoryInputStream input(wavData.getData(), wavData.getDataSize(), false);
        std::unique_ptr<juce::AudioFormatReader> reader(wavFormat.createReaderFor(&input, false));
        if (reader != nullptr)
        {
            juce::BigInteger notes;
            notes.setRange(0, 128, true);
            synth.addSound(new juce::SamplerSound("Generated test sample", *reader, notes, 60, 0.01, 0.2, 2.0));
        }
    }
}

void MiniSamplerAudioProcessor::prepareToPlay(double sampleRate, int)
{
    const juce::ScopedLock lock(synthLock);
    synth.setCurrentPlaybackSampleRate(sampleRate);
}

void MiniSamplerAudioProcessor::releaseResources()
{
}

bool MiniSamplerAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet().isDisabled();
}

void MiniSamplerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                             juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    const juce::ScopedLock lock(synthLock);
    keyboardState.processNextMidiBuffer(midiMessages, 0, buffer.getNumSamples(), true);
    synth.renderNextBlock(buffer, midiMessages, 0, buffer.getNumSamples());
}

bool MiniSamplerAudioProcessor::loadSample(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr)
        return false;

    juce::BigInteger notes;
    notes.setRange(0, 128, true);

    auto* newSound = new juce::SamplerSound(
        file.getFileNameWithoutExtension(), *reader, notes, 60, 0.01, 0.2, 30.0);

    {
        const juce::ScopedLock lock(synthLock);
        synth.clearSounds();
        synth.addSound(newSound);
        loadedSampleName = file.getFileName();
    }

    return true;
}

juce::String MiniSamplerAudioProcessor::getLoadedSampleName() const
{
    const juce::ScopedLock lock(synthLock);
    return loadedSampleName;
}

juce::AudioProcessorEditor* MiniSamplerAudioProcessor::createEditor()
{
    return new MiniSamplerAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MiniSamplerAudioProcessor();
}
