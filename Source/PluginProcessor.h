#pragma once

#include <atomic>
#include <JuceHeader.h>

class MiniSamplerAudioProcessor final : public juce::AudioProcessor
{
public:
    MiniSamplerAudioProcessor();
    ~MiniSamplerAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Mini Sampler"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    bool loadSample(const juce::File& file);
    juce::String getLoadedSampleName() const;
    juce::MidiKeyboardState& getKeyboardState() { return keyboardState; }
    void writeDiagnostic(const juce::String& message) const;

private:
    juce::AudioFormatManager formatManager;
    juce::Synthesiser synth;
    juce::MidiKeyboardState keyboardState;
    mutable juce::CriticalSection synthLock;
    juce::String loadedSampleName { "No sample loaded" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MiniSamplerAudioProcessor)
};
