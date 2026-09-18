#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <array>
#include "LoopMath.h"

struct MiniSamplerSample
{
    juce::AudioBuffer<float> audio;
    double rate = 44100.0;
    juce::File file;
    std::array<std::vector<std::pair<float, float>>, 2> peaks;
    double duration() const { return audio.getNumSamples() / rate; }
};

class MiniSamplerAudioProcessor final : public juce::AudioProcessor, private juce::Thread
{
public:
    struct Loop { double start = 0.0, end = 0.0, beats = 0.0; };
    struct ViewState
    {
        std::shared_ptr<const MiniSamplerSample> sample;
        Loop loop;
        std::vector<Loop> slots;
        int grid = 3, segments = 1;
        bool snap = true, triplet = false, zeroCross = false;
        int width = 1000, height = 390;
        juce::String status;
    };
    MiniSamplerAudioProcessor();
    ~MiniSamplerAudioProcessor() override;
    void prepareToPlay(double, int) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
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
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    void requestSampleLoad(const juce::File&, bool restoring = false);
    bool isLoading() const { return loading.load(); }
    ViewState getViewState() const;
    void setLoopSelection(double start, double end, double beats = 0.0);
    void stopLoop();
    bool isLooping() const { return loopEnabled.load(); }
    void saveSlot();
    void recallSlot(int);
    void deleteSlot(int);
    void setUiSettings(int grid, int segments, bool snap, bool triplet, bool zeroCross);
    void setEditorSize(int, int);
    double getProjectTempo() const { return projectTempo.load(); }
    int getProjectTimeSignatureNumerator() const { return numerator.load(); }
    int getProjectTimeSignatureDenominator() const { return denominator.load(); }
    double getPlaybackSeconds() const { return playbackSeconds.load(); }
    bool isHostPlaying() const { return hostPlaying.load(); }
    bool hasHostPosition() const { return hostConnected.load(); }
private:
    void run() override;
    void publishLoop(const Loop&);
    struct Voice { double position = 0, step = 1; float gain = 0; int note = -1; };
    std::array<Voice, 16> voices {};
    double outputRate = 44100.0, fallbackBeat = 0.0;
    Loop audioLoop;
    const MiniSamplerSample* lastAudioSample = nullptr;
    std::atomic<const MiniSamplerSample*> audioSample { nullptr };
    // Old sample buffers are reclaimed by the loader, never by the audio callback.
    std::atomic<unsigned> audioReaders { 0 };
    std::vector<std::shared_ptr<const MiniSamplerSample>> retired;
    mutable juce::CriticalSection stateLock;
    ViewState state;
    juce::File pendingFile;
    bool pendingRestore = false;
    uint64_t requestVersion = 0;
    std::atomic<bool> loading { false };
    std::atomic<double> projectTempo { 120.0 }, playbackSeconds { -1.0 };
    std::atomic<int> numerator { 4 }, denominator { 4 };
    std::atomic<bool> hostPlaying { false }, hostConnected { false }, loopEnabled { false };
    std::atomic<unsigned> loopSequence { 0 };
    std::atomic<double> loopStart { 0.0 }, loopEnd { 0.0 }, loopBeats { 0.0 };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MiniSamplerAudioProcessor)
};
