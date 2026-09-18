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

        const auto loaded = p.getViewState().sample;
        for (const auto interval : { std::pair<int, int>{ 129, 9131 }, { 410, 413 }, { 0, 88200 }, { 65537, 88111 } })
        {
            const auto actual = loaded->waveformRange(0, interval.first, interval.second);
            const auto* data = loaded->audio.getReadPointer(0);
            float low = data[interval.first], high = low;
            for (int i = interval.first; i < interval.second; ++i) { low = juce::jmin(low, data[i]); high = juce::jmax(high, data[i]); }
            check(actual.first == low && actual.second == high, "waveform cache returns EXACT original peaks including unaligned edges");
        }
        const auto anchor = editor->localPointToGlobal(juce::Point<int>{ 420, 12 });
        const auto options = miniSamplerMenuOptions(*editor, anchor);
        check(options.getParentComponent() == editor.get() && options.getTargetScreenArea().getPosition() == anchor,
              "popup is constrained to plugin editor and anchored at clicked cursor position");
        const auto eventFor = [](juce::Component* component, float x, float y, int modifiers, bool dragging = false)
        {
            return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), { x, y }, juce::ModifierKeys(modifiers),
                1.0f, 0, 0, 0, 0, component, component, juce::Time::getCurrentTime(), { x, y }, juce::Time::getCurrentTime(), 1, dragging);
        };
        p.setLoopSelection(0.5, 1.0, 1); wave->refreshFromProcessor();
        const auto slotBaseline = wave->createComponentSnapshot(wave->getLocalBounds());
        check(wave->getBottom() == editor->getHeight() - 4, "waveform fills editor down to bottom margin with no footer");
        check(slotBaseline.getPixelAt(10, 5) == juce::Colour(0xff64798c), "scrollbar is above waveform and ruler");
        editor->mouseDown(eventFor(editor.get(), 98, 12, juce::ModifierKeys::leftButtonModifier));
        auto slotImage = wave->createComponentSnapshot(wave->getLocalBounds());
        const int slotX = 4 + static_cast<int>((wave->getWidth() - 8) * 0.35);
        check(slotImage.getPixelAt(slotX, wave->getHeight() - 19) == juce::Colour(0xffe07a5f), "coral slot line overlays waveform immediately without playback or timer");
        check(slotImage.getPixelAt(slotX, wave->getHeight() - 16) == slotBaseline.getPixelAt(slotX, wave->getHeight() - 16), "slot marker is only three pixels thick and does not occupy loop bar");
        check(slotImage.getPixelAt(slotX, wave->getHeight() - 15) != juce::Colour(0xff101315)
              && slotImage.getPixelAt(slotX, wave->getHeight() - 6) != juce::Colour(0xff101315), "bottom loop handle is twelve pixels high");
        editor->mouseDown(eventFor(editor.get(), 98, 12, juce::ModifierKeys::rightButtonModifier));
        slotImage = wave->createComponentSnapshot(wave->getLocalBounds());
        check(p.getViewState().slots.empty() && slotImage.getPixelAt(slotX, wave->getHeight() - 19) == slotBaseline.getPixelAt(slotX, wave->getHeight() - 19),
              "deleting slot removes its line immediately without playback");
        p.setLoopSelection(0.25, 0.5, 0.5); wave->refreshFromProcessor();
        const float selectionA = 4.0f + (wave->getWidth() - 8) * 0.5f;
        const float selectionB = 4.0f + (wave->getWidth() - 8) * 0.75f;
        wave->mouseDown(eventFor(wave, selectionA, 100, juce::ModifierKeys::leftButtonModifier));
        wave->mouseDrag(eventFor(wave, selectionB, 100, juce::ModifierKeys::leftButtonModifier, true));
        wave->mouseUp(eventFor(wave, selectionB, 100, 0, true));
        const auto selectionImage = wave->createComponentSnapshot(wave->getLocalBounds());
        check(selectionImage.getPixelAt(static_cast<int>((selectionA + selectionB) * 0.5f), wave->getHeight() - 9) == juce::Colour(0xff101315)
              && p.getViewState().loop.start == 0.25, "pending selection has no bottom loop marker and does not move the active loop");
        wave->applySelection();

        check(!p.getViewState().midiKeyTracking, "original loop on every MIDI note is the DEFAULT");
        p.stopLoop(); p.prepareToPlay(48000, 256);
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0); p.processBlock(audio, midi);
        const double rootCursor = p.getPlaybackSeconds(); const float rootAudio = audio.getSample(0, 128);
        p.prepareToPlay(48000, 256); midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 72, 1.0f), 0); p.processBlock(audio, midi);
        check(p.getPlaybackSeconds() == rootCursor && audio.getSample(0, 128) == rootAudio, "C4 and C5 play identical original audio and speed when note tracking is OFF");
        p.setMidiKeyTracking(true); p.prepareToPlay(48000, 256); p.processBlock(audio, midi);
        check(std::abs(p.getPlaybackSeconds() - rootCursor * 2) < 0.000001, "optional MIDI pitch/speed tracking doubles speed one octave up");
        p.setMidiKeyTracking(false); midi.clear(); p.prepareToPlay(48000, 256);

        p.setLoopSelection(0.25, 1.25, 2.0); p.saveSlot();
        host.ppq = 0.5; p.processBlock(audio, midi);
        const double expected = 0.25 + LoopMath::phase(0.5 + 255.0 * 120.0 / (60.0 * 48000.0), 2.0);
        check(std::abs(p.getPlaybackSeconds() - expected) < 0.000001, "playback cursor follows host PPQ at different sample rates");
        check(audio.getMagnitude(0, audio.getNumSamples()) > 0.01f, "loop produces audio");
        midi.addEvent(juce::MidiMessage::noteOn(1, 72, 1.0f), 0); p.processBlock(audio, midi);
        check(std::abs(p.getPlaybackSeconds() - expected) < 0.000001, "active loop stays original on a different MIDI note by default");
        midi.clear();
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
        p.setUiSettings(5, 3, false, true, true); p.setMidiKeyTracking(true);
        juce::MemoryBlock saved; p.getStateInformation(saved);
        MiniSamplerAudioProcessor restored; restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize())); waitForLoad(restored);
        const auto restoredState = restored.getViewState();
        check(restoredState.slots.size() == 1 && restoredState.grid == 5 && restoredState.zeroCross && restoredState.triplet && !restoredState.snap && restoredState.midiKeyTracking,
              "DAW project state restores sample path, slots, grid, Snap and ZC");
        p.deleteSlot(0); check(p.getViewState().slots.empty(), "slot deletion");
        check(std::abs(LoopMath::phase(-0.5, 2) - 0.75) < 0.000001, "negative project PPQ wraps correctly");
        check(LoopMath::divisionBeats(1, 7, 8) == 3.5, "bar division respects time signature");
        editor.reset(); p.setPlayHead(nullptr); file.deleteFile();
        std::cout << "All MiniSampler checks passed\n"; return 0;
    }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << std::endl; file.deleteFile(); return 1; }
}
