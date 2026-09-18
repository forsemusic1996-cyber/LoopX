#include "PluginProcessor.h"
#include "PluginEditor.h"

MiniSamplerAudioProcessor::MiniSamplerAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    writeDiagnostic("processor: constructor begin");
    formatManager.registerBasicFormats();
    writeDiagnostic("processor: formats registered");

    for (auto i = 0; i < 16; ++i)
        synth.addVoice(new juce::SamplerVoice());
    writeDiagnostic("processor: voices added");

    // Start with a tiny generated sample so the plugin is immediately testable.
    juce::AudioBuffer<float> generatedSample(1, 22050);
    writeDiagnostic("processor: generated buffer allocated");
    auto* samples = generatedSample.getWritePointer(0);
    for (int i = 0; i < generatedSample.getNumSamples(); ++i)
    {
        const auto t = static_cast<double>(i) / 44100.0;
        const auto envelope = std::exp(-t * 7.0);
        samples[i] = static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 440.0 * t) * envelope);
    }

    auto* wavData = new juce::MemoryOutputStream();
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(wavData, 44100.0, 1, 16, {}, 0));
    writeDiagnostic(writer != nullptr ? "processor: wav writer created" : "processor: wav writer FAILED");

    if (writer != nullptr)
    {
        writer->writeFromAudioSampleBuffer(generatedSample, 0, generatedSample.getNumSamples());
        {
            auto* input = new juce::MemoryInputStream(wavData->getData(), wavData->getDataSize(), false);
            std::unique_ptr<juce::AudioFormatReader> reader(wavFormat.createReaderFor(input, true));
            writeDiagnostic(reader != nullptr ? "processor: wav reader created" : "processor: wav reader FAILED");
            if (reader != nullptr)
            {
                juce::BigInteger notes;
                notes.setRange(0, 128, true);
                synth.addSound(new juce::SamplerSound("Generated test sample", *reader, notes, 60, 0.01, 0.2, 2.0));
                writeDiagnostic("processor: generated sound added");
            }
        }
        writer.reset();
    }
    else
    {
        delete wavData;
    }

    writeDiagnostic("processor: constructor complete");
}

void MiniSamplerAudioProcessor::writeDiagnostic(const juce::String& message) const
{
    const auto logFile = juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
                             .getChildFile("MiniSampler-diagnostics.log");
    logFile.appendText(juce::Time::getCurrentTime().toISO8601(true) + " " + message + "\n",
                       false, false, "UTF-8");
}

void MiniSamplerAudioProcessor::prepareToPlay(double sampleRate, int)
{
    writeDiagnostic("processor: prepareToPlay " + juce::String(sampleRate));
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
    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            if (const auto bpm = position->getBpm(); bpm.hasValue() && *bpm > 0.0)
                projectTempo.store(*bpm);

            if (const auto timeSignature = position->getTimeSignature(); timeSignature.hasValue())
            {
                timeSignatureNumerator.store(timeSignature->numerator);
                timeSignatureDenominator.store(timeSignature->denominator);
            }
        }
    }

    static std::atomic<bool> firstProcessBlock { false };
    if (!firstProcessBlock.exchange(true))
        writeDiagnostic("processor: first processBlock channels=" + juce::String(buffer.getNumChannels())
                        + " samples=" + juce::String(buffer.getNumSamples()));
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
    writeDiagnostic("processor: createEditor begin");
    auto* editor = new MiniSamplerAudioProcessorEditor(*this);
    writeDiagnostic("processor: createEditor complete");
    return editor;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MiniSamplerAudioProcessor();
}
