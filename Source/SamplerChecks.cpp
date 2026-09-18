#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <iostream>
#include <stdexcept>
#include <thread>
namespace
{
void check(bool passed, const char* message)
{
    if (!passed) throw std::runtime_error(message);
    std::cout << "PASS: " << message << std::endl;
}
void waitForLoad(MiniSamplerAudioProcessor& p)
{
    for (int i = 0; i < 1000 && p.isLoading(); ++i) juce::Thread::sleep(10);
    check(!p.isLoading() && p.getViewState().sample != nullptr, "async sample loading");
}
struct Transport final : juce::AudioPlayHead
{
    double bpm = 120, ppq = 0; bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p; p.setBpm(bpm); p.setPpqPosition(ppq); p.setIsPlaying(playing);
        p.setTimeSignature(TimeSignature { 4, 4 }); return p;
    }
};
}
int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("MiniSampler-test", ".wav");
    try
    {
        juce::AudioBuffer<float> source(2, 88200);
        for (int i = 0; i < source.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch) source.setSample(ch, i, static_cast<float>(std::sin(i * 0.017 + ch * 0.7) * 0.3));
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(file.createOutputStream().release(), 44100, 2, 16, {}, 0));
        check(writer != nullptr && writer->writeFromAudioSampleBuffer(source, 0, source.getNumSamples()), "test WAV creation"); writer.reset();
        MiniSamplerAudioProcessor p; Transport host;
        p.setPlayHead(&host); p.prepareToPlay(48000, 256);
        juce::AudioBuffer<float> audio(2, 256); juce::MidiBuffer midi; p.processBlock(audio, midi);
        std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
        auto* drop = dynamic_cast<juce::FileDragAndDropTarget*>(editor.get());
        MiniSamplerWaveformView* wave = nullptr;
        for (auto* child : editor->getChildren()) if (auto* found = dynamic_cast<MiniSamplerWaveformView*>(child)) wave = found;
        auto* waveDrop = dynamic_cast<juce::FileDragAndDropTarget*>(wave);
        check(drop && wave && waveDrop, "JUCE RTTI finds PUBLIC drop targets on editor and waveform");
        juce::StringArray paths { file.getFullPathName() };
        check(drop->isInterestedInFileDrag(paths) && waveDrop->isInterestedInFileDrag(paths), "local audio drag is accepted");
        waveDrop->fileDragEnter(paths, 50, 50);
        const auto hover = wave->createComponentSnapshot(wave->getLocalBounds());
        check(hover.getPixelAt(0, 40).getGreen() > 150, "empty waveform shows visible drag highlight");
        waveDrop->filesDropped(paths, 50, 50); waitForLoad(p);
        check(p.getViewState().sample->audio.getNumChannels() == 2, "stereo audio loaded by drop callback");
        check(wave->isInterestedInTextDrag("file:///" + file.getFullPathName().replaceCharacter('\\', '/')), "browser file URI drag is accepted");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
        waveDrop->fileDragEnter(paths, 50, 50);
        const auto loadedHover = wave->createComponentSnapshot(wave->getLocalBounds());
        check(loadedHover.getPixelAt(0, 40).getGreen() > 150, "loaded waveform shows visible drag highlight"); waveDrop->fileDragExit(paths);
        p.setLoopSelection(0.25, 1.25, 2.0); p.saveSlot();
        host.ppq = 0.5; p.processBlock(audio, midi);
        const double expected = 0.25 + LoopMath::phase(0.5 + 255.0 * 120.0 / (60.0 * 48000.0), 2.0);
        check(std::abs(p.getPlaybackSeconds() - expected) < 0.000001, "playback cursor follows host PPQ at different sample rates");
        check(audio.getMagnitude(0, audio.getNumSamples()) > 0.01f, "loop produces audio");
        const float reference = audio.getSample(0, 128);
        host.ppq = 2.5; p.processBlock(audio, midi);
        check(std::abs(audio.getSample(0, 128) - reference) < 0.00001f, "host seek/loop repeat is phase consistent");
        host.bpm = 90; host.ppq = 0.5; p.processBlock(audio, midi);
        const double slower = 0.25 + LoopMath::phase(0.5 + 255.0 * 90.0 / (60.0 * 48000.0), 2.0);
        check(std::abs(p.getPlaybackSeconds() - slower) < 0.000001, "tempo change adjusts playback increment");
        host.playing = false; p.processBlock(audio, midi);
        check(audio.getMagnitude(0, audio.getNumSamples()) == 0 && p.getPlaybackSeconds() < 0, "DAW Stop silences loop and hides cursor"); host.playing = true;
        p.setLoopSelection(0.0, 0.5, 1); p.recallSlot(0);
        check(p.getViewState().loop.start == 0.25 && p.getViewState().loop.beats == 2, "slot recall preserves region and musical length");
        wave->scaleLength(0.5);
        check(std::abs(p.getViewState().loop.end - 0.75) < 0.000001 && p.getViewState().loop.start == 0.25, "divide loop by two preserves start");
        wave->scaleLength(2);
        check(std::abs(p.getViewState().loop.end - 1.25) < 0.000001, "multiply loop by two");
        wave->setMusicalLength(1);
        check(std::abs(p.getViewState().loop.beats - 1) < 0.000001, "Loop Length sets musical duration");
        editor.reset(); editor.reset(p.createEditor());
        check(p.getViewState().sample != nullptr && p.getViewState().slots.size() == 1, "closing and reopening editor keeps sample and slots");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
        std::atomic<bool> running { true }; std::atomic<int> blocks { 0 };
        std::thread audioThread([&]
        {
            juce::AudioBuffer<float> b(2, 256); juce::MidiBuffer m;
            while (running.load()) { p.processBlock(b, m); ++blocks; std::this_thread::yield(); }
        });
        const auto start = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < 80; ++i)
        {
            editor->setSize(800 + (i % 12) * 60, 280 + (i % 5) * 40);
            const auto image = editor->createComponentSnapshot(editor->getLocalBounds()); juce::ignoreUnused(image);
        }
        const double elapsed = juce::Time::getMillisecondCounterHiRes() - start;
        running.store(false); audioThread.join();
        check(blocks.load() > 20 && elapsed < 15000, "live resize and waveform painting leave audio callbacks running");
        std::cout << "Resize stress: " << elapsed << " ms, " << blocks << " audio blocks\n";
        p.setUiSettings(5, 3, false, true, true);
        juce::MemoryBlock saved; p.getStateInformation(saved);
        MiniSamplerAudioProcessor restored; restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize())); waitForLoad(restored);
        const auto restoredState = restored.getViewState();
        check(restoredState.slots.size() == 1 && restoredState.grid == 5 && restoredState.zeroCross && restoredState.triplet && !restoredState.snap,
              "DAW project state restores sample path, slots, grid, Snap and ZC");
        p.deleteSlot(0); check(p.getViewState().slots.empty(), "slot deletion");
        check(std::abs(LoopMath::phase(-0.5, 2) - 0.75) < 0.000001, "negative project PPQ wraps correctly");
        check(LoopMath::divisionBeats(1, 7, 8) == 3.5, "bar division respects time signature");
        editor.reset(); p.setPlayHead(nullptr); file.deleteFile();
        std::cout << "All MiniSampler checks passed\n"; return 0;
    }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << std::endl; file.deleteFile(); return 1; }
}
