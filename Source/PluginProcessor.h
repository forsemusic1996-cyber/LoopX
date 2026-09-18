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
    struct PeakLevel { int64_t stride = 64; std::vector<std::pair<float, float>> values; };
    std::array<std::vector<PeakLevel>, 2> peaks;
    void buildWaveform()
    {
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            auto& levels = peaks[static_cast<size_t>(ch)];
            PeakLevel base;
            const auto* data = audio.getReadPointer(ch);
            for (int64_t a = 0; a < audio.getNumSamples(); a += 64)
            {
                float low = data[a], high = low;
                for (int64_t i = a + 1; i < juce::jmin<int64_t>(a + 64, audio.getNumSamples()); ++i)
                { low = juce::jmin(low, data[i]); high = juce::jmax(high, data[i]); }
                base.values.emplace_back(low, high);
            }
            levels.push_back(std::move(base));
            while (levels.back().values.size() > 1)
            {
                const auto& previous = levels.back();
                PeakLevel next; next.stride = previous.stride * 4;
                for (size_t a = 0; a < previous.values.size(); a += 4)
                {
                    auto range = previous.values[a];
                    for (size_t i = a + 1; i < juce::jmin(a + 4, previous.values.size()); ++i)
                    { range.first = juce::jmin(range.first, previous.values[i].first); range.second = juce::jmax(range.second, previous.values[i].second); }
                    next.values.push_back(range);
                }
                levels.push_back(std::move(next));
            }
        }
    }
    std::pair<float, float> waveformRange(int channel, int first, int last) const
    {
        first = juce::jlimit(0, audio.getNumSamples() - 1, first);
        last = juce::jlimit(first + 1, audio.getNumSamples(), last);
        const auto* data = audio.getReadPointer(channel);
        std::pair<float, float> range { data[first], data[first] };
        // Exact original extrema: cached blocks are used ONLY when wholly
        // inside this pixel's source interval. Read unaligned edges verbatim.
        while (first < last)
        {
            const PeakLevel* level = nullptr;
            for (const auto& candidate : peaks[static_cast<size_t>(channel)])
                if (first % candidate.stride == 0 && candidate.stride <= last - first) level = &candidate;
            if (level)
            {
                const auto value = level->values[static_cast<size_t>(first / level->stride)];
                range.first = juce::jmin(range.first, value.first); range.second = juce::jmax(range.second, value.second);
                first += static_cast<int>(level->stride);
            }
            else
            {
                range.first = juce::jmin(range.first, data[first]); range.second = juce::jmax(range.second, data[first]); ++first;
            }
        }
        return range;
    }
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
        bool snap = true, triplet = false, zeroCross = false, midiKeyTracking = false;
        bool brightGrid = false, stereoWaveform = false;
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
    void setDisplaySettings(bool brightGrid, bool stereoWaveform);
    void setEditorSize(int, int);
    double getProjectTempo() const { return projectTempo.load(); }
    int getProjectTimeSignatureNumerator() const { return numerator.load(); }
    int getProjectTimeSignatureDenominator() const { return denominator.load(); }
    double getPlaybackSeconds() const { return playbackSeconds.load(); }
    bool isHostPlaying() const { return hostPlaying.load(); }
    bool hasHostPosition() const { return hostConnected.load(); }
    void setMidiKeyTracking(bool enabled)
    {
        const juce::ScopedLock lock(stateLock);
        state.midiKeyTracking = enabled; midiKeyTracking.store(enabled);
    }
private:
    void run() override;
    void publishLoop(const Loop&);
    struct Voice { double position = 0, step = 1; float gain = 0; int note = -1; };
    std::array<Voice, 16> voices {};
    double outputRate = 44100.0, fallbackBeat = 0.0;
    int loopMidiNote = 60;
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
    std::atomic<bool> midiKeyTracking { false };
    std::atomic<double> projectTempo { 120.0 }, playbackSeconds { -1.0 };
    std::atomic<int> numerator { 4 }, denominator { 4 };
    std::atomic<bool> hostPlaying { false }, hostConnected { false }, loopEnabled { false };
    std::atomic<unsigned> loopSequence { 0 };
    std::atomic<double> loopStart { 0.0 }, loopEnd { 0.0 }, loopBeats { 0.0 };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MiniSamplerAudioProcessor)
};
