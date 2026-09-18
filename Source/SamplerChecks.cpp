#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "StretchRender.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <limits>

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
    double bpm = 120, ppq = 0;
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p; p.setBpm(bpm); p.setPpqPosition(ppq); p.setIsPlaying(playing);
        p.setTimeSignature(TimeSignature { 4, 4 }); return p;
    }
};
struct StateQueryHost final : juce::AudioProcessorListener
{
    int changes=0;
    void query(juce::AudioProcessor* p)
    {
        // Simulate a host obtaining its state on a second thread while a
        // synchronous notification is being handled. Must not hold stateLock.
        std::thread reader([p]{juce::MemoryBlock state; p->getStateInformation(state);}); reader.join(); ++changes;
    }
    void audioProcessorParameterChanged(juce::AudioProcessor* p,int,float) override { query(p); }
    void audioProcessorChanged(juce::AudioProcessor* p,const ChangeDetails&) override { query(p); }
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
            for (int ch = 0; ch < 2; ++ch) source.setSample(ch, i, static_cast<float>(std::sin(i * juce::MathConstants<double>::twoPi * 440 / 44100) * (ch == 0 ? 0.3 : 0.15)));
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(file.createOutputStream().release(), 44100, 2, 16, {}, 0));
        check(writer != nullptr && writer->writeFromAudioSampleBuffer(source, 0, source.getNumSamples()), "test WAV creation"); writer.reset();

        MiniSamplerAudioProcessor p(juce::File{});
        check(!p.getViewState().brightGrid, "Bright Grid is OFF by default");
        Transport host; p.setPlayHead(&host); p.prepareToPlay(48000, 256);
        juce::AudioBuffer<float> audio(2, 256); juce::MidiBuffer midi;
        p.processBlock(audio, midi);
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
        check(slotBaseline.getPixelAt(10, 5) == juce::Colour(0xff484a4c)
              && slotBaseline.getPixelAt(10, 11) == juce::Colour(0xff484a4c), "muted gray scrollbar has the compact 14-pixel height");
        bool labelOnWaveform = false;
        const int labelX = 4 + static_cast<int>((wave->getWidth() - 8) * 0.75);
        for (int y = 16; y < 30; ++y)
            for (int x = labelX + 3; x < labelX + 30; ++x)
                if (slotBaseline.getPixelAt(x, y).getRed() > 110) labelOnWaveform = true;
        check(labelOnWaveform, "bar and beat numbers are drawn below scrollbar on waveform");
        editor->mouseDown(eventFor(editor.get(), 98, 12, juce::ModifierKeys::leftButtonModifier));
        auto slotImage = wave->createComponentSnapshot(wave->getLocalBounds());
        const int slotX = 4 + static_cast<int>((wave->getWidth() - 8) * 0.35);
        check(slotImage.getPixelAt(slotX, wave->getHeight() - 21) == juce::Colour(0xffe07a5f), "coral slot line overlays waveform immediately without playback or timer");
        check(slotImage.getPixelAt(slotX, wave->getHeight() - 18) == slotBaseline.getPixelAt(slotX, wave->getHeight() - 18), "slot marker is only three pixels thick and does not occupy loop bar");
        check(slotImage.getPixelAt(slotX, wave->getHeight() - 17) != juce::Colour(0xff101315)
              && slotImage.getPixelAt(slotX, wave->getHeight() - 6) != juce::Colour(0xff101315), "bottom loop handle is fourteen pixels high");
        editor->mouseDown(eventFor(editor.get(), 98, 12, juce::ModifierKeys::rightButtonModifier));
        slotImage = wave->createComponentSnapshot(wave->getLocalBounds());
        check(p.getViewState().slots.empty() && slotImage.getPixelAt(slotX, wave->getHeight() - 21) == slotBaseline.getPixelAt(slotX, wave->getHeight() - 21),
              "deleting slot removes its line immediately without playback");
        p.setLoopSelection(0.25, 0.5, 0.5); wave->refreshFromProcessor();
        const float selectionA = 4.0f + (wave->getWidth() - 8) * 0.5f;
        const float selectionB = 4.0f + (wave->getWidth() - 8) * 0.75f;
        const auto selectionBaseline = wave->createComponentSnapshot(wave->getLocalBounds());
        wave->mouseDown(eventFor(wave, selectionA, 100, juce::ModifierKeys::leftButtonModifier));
        wave->mouseDrag(eventFor(wave, selectionB, 100, juce::ModifierKeys::leftButtonModifier, true));
        wave->mouseUp(eventFor(wave, selectionB, 100, 0, true));
        const auto selectionImage = wave->createComponentSnapshot(wave->getLocalBounds());
        const int tintX = static_cast<int>((selectionA + selectionB) * 0.5f) + 7, tintY = wave->getHeight() - 50;
        const auto tint = selectionImage.getPixelAt(tintX, tintY);
        const auto expectedTint = selectionBaseline.getPixelAt(tintX, tintY).overlaidWith(juce::Colour(0x90548787));
        check(std::abs(tint.getRed() - expectedTint.getRed()) <= 2 && std::abs(tint.getGreen() - expectedTint.getGreen()) <= 2
              && std::abs(tint.getBlue() - expectedTint.getBlue()) <= 2, "selection is lighter gray-teal, not blue");
        check(selectionImage.getPixelAt(static_cast<int>((selectionA + selectionB) * 0.5f), wave->getHeight() - 9) == juce::Colour(0xff101315)
              && p.getViewState().loop.start == 0.25, "pending selection has no bottom loop marker and does not move the active loop");
        wave->applySelection();

        check(!p.getViewState().midiKeyTracking, "original loop on every MIDI note is the DEFAULT");
        p.setLoopSelection(0, 1, 2); p.prepareToPlay(48000, 256);
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0); p.processBlock(audio, midi);
        const double rootCursor = p.getPlaybackSeconds(); const float rootAudio = audio.getSample(0, 128);
        p.prepareToPlay(48000, 256); midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 72, 1.0f), 0); p.processBlock(audio, midi);
        check(p.getPlaybackSeconds() == rootCursor && audio.getSample(0, 128) == rootAudio, "C4 and C5 play identical original audio and speed when note tracking is OFF");
        p.setMidiKeyTracking(true); p.prepareToPlay(48000, 256); p.processBlock(audio, midi);
        check(std::abs(p.getPlaybackSeconds() - rootCursor * 2) < 0.000001, "optional MIDI pitch/speed tracking doubles speed one octave up");
        p.setMidiKeyTracking(false); midi.clear(); p.prepareToPlay(48000, 256);

        p.setPlaybackSettings(1, 0);
        p.setLoopSelection(0.25, 1.25, 2.0); p.saveSlot();
        host.ppq = 0.5; p.processBlock(audio, midi);
        const double expected = 0.25 + LoopMath::phase(0.5 + 255.0 * 120.0 / (60.0 * 48000.0), 2.0);
        check(std::abs(p.getPlaybackSeconds() - expected) < 0.000001, "playback cursor follows host PPQ at different sample rates");
        check(audio.getMagnitude(0, audio.getNumSamples()) > 0.01f, "loop produces audio");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
        const auto cursorImage = wave->createComponentSnapshot(wave->getLocalBounds());
        bool cursorInScrollbar = false;
        for (int y = 0; y < 14; ++y)
            for (int x = 0; x < wave->getWidth(); ++x)
                if (cursorImage.getPixelAt(x, y).getGreen() > 140) cursorInScrollbar = true;
        check(!cursorInScrollbar, "turquoise playback cursor and labels never enter scrollbar");
        midi.addEvent(juce::MidiMessage::noteOn(1, 72, 1.0f), 0); p.processBlock(audio, midi);
        check(std::abs(p.getPlaybackSeconds() - expected) < 0.000001, "active loop stays original on a different MIDI note by default");
        midi.clear();
        const float reference = audio.getSample(0, 128);
        host.ppq = 2.5; p.processBlock(audio, midi);
        juce::ignoreUnused(reference);
        check(std::abs(p.getPlaybackSeconds() - expected) < 0.000001, "host seek/loop repeat cursor is phase consistent");
        host.bpm = 90; host.ppq = 0.5; p.processBlock(audio, midi);
        const double slower = 0.25 + LoopMath::phase(0.5 + 255.0 * 90.0 / (60.0 * 48000.0), 2.0);
        check(std::abs(p.getPlaybackSeconds() - slower) < 0.000001, "tempo change adjusts playback increment");
        host.playing = false; p.processBlock(audio, midi); p.processBlock(audio, midi);
        check(audio.getMagnitude(0, audio.getNumSamples()) == 0 && p.getPlaybackSeconds() < 0, "DAW Stop silences loop and hides cursor"); host.playing = true;
        p.setLoopSelection(0.0, 0.5, 1); p.recallSlot(0);
        check(p.getViewState().loop.start == 0.25 && p.getViewState().loop.beats == 2, "slot recall preserves region and musical length");
        wave->scaleLength(0.5);
        check(std::abs(p.getViewState().loop.end - 0.75) < 0.000001 && p.getViewState().loop.start == 0.25, "divide loop by two preserves start");
        wave->scaleLength(2);
        check(std::abs(p.getViewState().loop.end - 1.25) < 0.000001, "multiply loop by two");
        wave->setMusicalLength(1);
        check(std::abs(p.getViewState().loop.beats - 1) < 0.000001, "Loop Length sets musical duration");

        // Exercise parameter automation and all exact velocity interval edges.
        check(p.getParameters().size() == 2 && p.slotParameter->getName(32) == "Slot"
              && p.positionParameter->getName(32) == "Loop Position", "real host automation parameters are exposed");
        p.setPlaybackSettings(0, 0); p.prepareToPlay(48000, 256); midi.clear(); p.processBlock(audio, midi);
        check(audio.getMagnitude(0, 256) == 0 && p.getPlaybackSeconds() < 0, "MIDI Trigger is silent with no held note");
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 32); p.processBlock(audio, midi);
        check(audio.getMagnitude(0, 32) == 0 && audio.getMagnitude(64, 160) > 0.01f, "Note On starts at its exact MIDI sample offset");
        const double triggered = p.getPlaybackSeconds(); midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 32); p.processBlock(audio, midi);
        check(std::abs(p.getPlaybackSeconds() - triggered) < 1e-8, "each Note On restarts the loop");
        midi.clear(); midi.addEvent(juce::MidiMessage::noteOff(1, 60), 48); p.processBlock(audio, midi); midi.clear(); p.processBlock(audio, midi);
        check(audio.getMagnitude(0, 256) == 0 && p.getPlaybackSeconds() < 0, "Note Off ends MIDI playback with short clickless release");

        const int lows[] {1,14,27,40,52,65,78,90,103,116}, highs[] {13,26,39,51,64,77,89,102,115,127};
        for (int slot = 0; slot < 10; ++slot)
            check(MiniSamplerAudioProcessor::velocitySlot(lows[slot]) == slot + 1 &&
                  MiniSamplerAudioProcessor::velocitySlot(highs[slot]) == slot + 1, "velocity maps to exact specified slot boundaries");
        p.setLoopSelection(0.2, 0.7, 1); p.recallSlot(0); const auto stored = p.getViewState().loop;
        p.slotParameter->setValueNotifyingHost(p.slotParameter->convertTo0to1(0));
        midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1,60,1.0f),0); p.processBlock(audio, midi);
        check(p.getPlaybackSeconds() >= 0.2 && p.getPlaybackSeconds() < 0.21, "host Slot automation wins over an older UI slot request in actual audio");
        check(std::abs(p.getViewState().loop.start - 0.2) < 1e-8, "Slot automation zero selects manual loop");
        p.slotParameter->setValueNotifyingHost(p.slotParameter->convertTo0to1(1)); p.processBlock(audio, midi);
        check(p.getViewState().loop.start == stored.start, "Slot automation one selects first stored loop");
        p.setPlaybackSettings(0, 1); p.prepareToPlay(48000, 256); midi.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(1)), 0);
        p.processBlock(audio, midi); check(p.getViewState().loop.start == stored.start, "velocity selects slot before triggering");
        midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(127)), 0); p.processBlock(audio, midi);
        p.processBlock(audio, midi); check(audio.getMagnitude(0, 256) == 0, "empty velocity slot is silent, never plays wrong material");
        p.setPlaybackSettings(0, 0); p.selectSlot(0); p.setLoopSelection(0.2, 0.7, 1);
        p.positionParameter->setValueNotifyingHost(0); midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1,60,1.0f),0);
        p.processBlock(audio,midi);
        check(p.getViewState().loop.start == 0, "writing Loop Position zero works even when parameter value was already zero");
        p.setLoopSelection(0.2,0.7,1); p.setLoopFades(0.02, 0.03);
        check(p.getViewState().loop.fadeIn == 0.02 && p.getViewState().loop.fadeOut == 0.03, "independent loop fades are applied to engine state");

        // Fade drag changes audio fades independently of Snap.
        p.setLoopSelection(0.2, 0.7, 1); wave->refreshFromProcessor();
        const float fadeX = 4 + float((0.2 + p.getViewState().loop.fadeIn) / 2 * (wave->getWidth() - 8));
        wave->mouseDown(eventFor(wave, fadeX, 36, juce::ModifierKeys::leftButtonModifier));
        wave->mouseDrag(eventFor(wave, fadeX + 30, 36, juce::ModifierKeys::leftButtonModifier, true));
        wave->mouseUp(eventFor(wave, fadeX + 30, 36, 0, true));
        check(p.getViewState().loop.fadeIn > 0.04, "fade handle drag changes independent fade-in duration");
        p.setUiSettings(3, 3, true, false, false); wave->refreshFromProcessor();
        wave->mouseDown(eventFor(wave, float(wave->getWidth()) * 0.75f, 110, juce::ModifierKeys::leftButtonModifier));
        check(p.getViewState().loop.start >= 1, "segment activates on mouse down without waiting for release");
        wave->mouseUp(eventFor(wave, float(wave->getWidth()) * 0.75f, 110, 0));
        p.setLoopSelection(0.2, 0.7, 1);
        p.setUiSettings(5, 1, true, false, false); p.prepareToPlay(48000, 256);
        p.positionParameter->setValueNotifyingHost(0.37f); midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0); p.processBlock(audio, midi);
        const double snapStep = 0.25 * 60 / host.bpm;
        const auto positionState = p.getViewState();
        check(std::abs(positionState.loop.start / snapStep - std::round(positionState.loop.start / snapStep)) < 1e-7,
              "Loop Position automation snaps to selected musical grid");
        p.setPlaybackSettings(0, 2); p.prepareToPlay(48000, 256); midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(80)), 0); p.processBlock(audio, midi);
        const auto velocityPosition = p.getViewState();
        check(std::abs(velocityPosition.loop.start / snapStep - std::round(velocityPosition.loop.start / snapStep)) < 1e-7,
              "velocity Loop Position obeys Snap grid");
        p.setUiSettings(5, 1, false, false, false); midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(80)), 0);
        p.processBlock(audio, midi);
        check(std::abs(p.getViewState().loop.start - 79.0 / 126 * 1.5) < 1e-6, "velocity position is continuous when Snap is off");

        // Exact length, pitch and stereo coherence of the actual Signalsmith render.
        auto rendered = renderBpmSample(*loaded, 0, 100.0 / 124, [] { return false; });
        check(rendered && rendered->audio.getNumSamples() == int(std::llround(88200.0 * 100 / 124)), "Signalsmith 100 to 124 BPM has exact target duration");
        const auto estimatePitch = [](const juce::AudioBuffer<float>& b, double rate)
        {
            int crossings = 0, first = -1, last = -1;
            const int a = int(rate * 0.15), end = juce::jmin(b.getNumSamples() - 1, int(rate * 0.9));
            for (int i = a + 1; i < end; ++i) if (b.getSample(0, i-1) <= 0 && b.getSample(0, i) > 0)
            { if (first < 0) first = i; last = i; ++crossings; }
            return crossings > 1 ? (crossings - 1) * rate / (last - first) : 0.0;
        };
        check(std::abs(estimatePitch(rendered->audio, rendered->rate) - 440) < 3, "Signalsmith preserves original 440 Hz pitch when BPM changes");
        double stereoError = 0;
        for (int i = 10000; i < 40000; ++i) stereoError += std::abs(rendered->audio.getSample(0,i) * 0.5 - rendered->audio.getSample(1,i));
        check(stereoError / 30000 < 0.015, "Signalsmith preserves stereo channel coherence");
        check(!renderBpmSample(*loaded, 0, 0.8, [] { return true; }), "background stretch cancellation is supported");
        p.setPlaybackSettings(1, 0); p.selectSlot(0); p.setLoopSelection(0, 2, 4); host.bpm = 124; host.ppq = 0; p.processBlock(audio, midi);
        check(p.matchBpm(100), "Original BPM accepts numeric sample tempo"); waitForLoad(p);
        check(std::abs(p.getViewState().sample->duration() - 2 * 100.0 / 124) < 1.0 / 44100, "processor matches sample BPM to host BPM");
        host.bpm = 140; p.processBlock(audio, midi);
        juce::Thread::sleep(80); waitForLoad(p);
        check(std::abs(p.getViewState().sample->duration() - 2 * 100.0 / 140) < 1.0 / 44100, "host tempo changes re-render original, never an already stretched buffer");
        check(!p.matchBpm(0) && !p.matchBpm(std::numeric_limits<double>::quiet_NaN()), "invalid Original BPM is rejected");
        p.setUiSettings(5, 1, true, false, true);
        p.setStartOffset(0.123456); waitForLoad(p);
        const double exactOffset = std::round(0.123456 * 44100) / 44100;
        check(std::abs(p.getViewState().startOffset - exactOffset) < 1e-10 &&
              p.getViewState().originalSample == loaded, "START has sample precision, ignores Snap/ZC, keeps original immutable");
        check(std::abs(p.getViewState().sample->duration() - (2 - exactOffset) * 100 / 140) < 1.0 / 44100, "START trims logical duration before stretch");
        p.setTheme(4);
        juce::MemoryBlock transformedState; p.getStateInformation(transformedState);
        MiniSamplerAudioProcessor transformedRestore(juce::File{});
        transformedRestore.setStateInformation(transformedState.getData(), int(transformedState.getSize())); waitForLoad(transformedRestore);
        check(transformedRestore.getViewState().theme == 4 && transformedRestore.getViewState().stretchApplied &&
              transformedRestore.getViewState().startOffset == exactOffset, "theme, original BPM, START and processed state restore without embedding audio");
        p.requestSampleLoad(file); waitForLoad(p);
        check(p.getViewState().startOffset == 0 && !p.getViewState().stretchApplied, "new sample resets START and BPM conversion");
        p.setTheme(0); p.setLoopSelection(0.25, 1.25, 2); p.deleteSlot(0); p.saveSlot();
        p.setUiSettings(3, 1, true, false, false); midi.clear();
        p.setDisplaySettings(false, true);
        editor.reset(); editor.reset(p.createEditor());
        for (auto* child : editor->getChildren()) if (auto* found = dynamic_cast<MiniSamplerWaveformView*>(child)) wave = found;
        check(!wave->brightGrid && wave->stereo, "editor reopen restores grid brightness and waveform mode");
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
        p.setUiSettings(5, 3, false, true, true); p.setMidiKeyTracking(true); p.setDisplaySettings(false, true);
        juce::MemoryBlock saved; p.getStateInformation(saved);
        MiniSamplerAudioProcessor restored(juce::File{}); restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize())); waitForLoad(restored);
        const auto restoredState = restored.getViewState();
        check(restoredState.slots.size() == 1 && restoredState.grid == 5 && restoredState.segments == 3 && restoredState.zeroCross
              && restoredState.triplet && !restoredState.snap && restoredState.midiKeyTracking && !restoredState.brightGrid && restoredState.stereoWaveform,
              "DAW project state restores sample path, slots, grid, Snap and ZC");
        p.deleteSlot(0); check(p.getViewState().slots.empty(), "slot deletion");
        p.setLoopSelection(0.25, 1.25, 2.0); p.saveSlot();
        p.requestSampleLoad(file); waitForLoad(p);
        const auto replacementState = p.getViewState();
        check(replacementState.slots.size() == 1 && std::abs(replacementState.loop.start - 0.25) < 0.000001
              && std::abs(replacementState.loop.end - 1.25) < 0.000001,
              "replacing a sample retains a valid loop and saved slots");
        check(std::abs(LoopMath::phase(-0.5, 2) - 0.75) < 0.000001, "negative project PPQ wraps correctly");
        check(LoopMath::divisionBeats(1, 7, 8) == 3.5, "bar division respects time signature");

        // START may clip a source region but must never alter its seconds/beat.
        p.setPlaybackSettings(0,0); p.setLoopSelection(0,2,4); p.setStartOffset(0.4); waitForLoad(p);
        auto beforeStart=p.getViewState().loop;
        check(std::abs((beforeStart.end-beforeStart.start)/beforeStart.beats-0.5)<1e-7,
              "START changes only source offset, keeps loop speed when a boundary clips");
        p.setStartOffset(0.8); waitForLoad(p);
        const auto afterStart=p.getViewState().loop;
        check(std::abs((afterStart.end-afterStart.start)/afterStart.beats-0.5)<1e-7,
              "repeated START edits do not accumulate playback speed changes");
        p.requestSampleLoad(file); waitForLoad(p); p.setLoopSelection(0.1,0.6,1);
        p.setLoopEnabled(false); p.prepareToPlay(48000,256); midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1,60,1.0f),0); p.processBlock(audio,midi);
        check(audio.getMagnitude(0,256)==0,"Loop toggle OFF is silent");
        p.setLoopEnabled(true); p.processBlock(audio,midi); check(audio.getMagnitude(0,256)>0.01,"Loop toggle ON retains existing region");

        p.deleteSlot(0); p.setLoopSelection(0.1,0.6,1); p.saveSlot(); p.setLoopSelection(0.8,1.3,1); p.saveSlot();
        p.setNoteSettings(1,60); p.setPlaybackSettings(0,0); p.prepareToPlay(48000,256);
        midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1,61,juce::uint8(80)),17); p.processBlock(audio,midi);
        check(p.getViewState().loop.start==0.8 && p.getPlaybackSeconds()>=0.8 && p.getPlaybackSeconds()<0.81,
              "MIDI Note -> Slot selects the correct stored region at Note On offset");
        check(p.getNameForMidiNoteNumber(60,1)==std::optional<juce::String>{"Slot 1"} &&
              p.getNameForMidiNoteNumber(61,2)==std::optional<juce::String>{"Slot 2"} && !p.getNameForMidiNoteNumber(-1,1),
              "JUCE note-name callback exports slot labels and validates note range");
        p.selectSlot(0); p.setLoopSelection(0.1,0.6,1); p.setUiSettings(5,1,true,false,false); p.setNoteSettings(2,60);
        host.bpm=120; host.ppq=0; p.prepareToPlay(48000,256);
        midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1,3,juce::uint8(90)),0); p.processBlock(audio,midi);
        const auto noteRegion=p.getViewState().loop;
        check(std::abs(noteRegion.start-0.375)<1e-7 && std::abs(noteRegion.end-noteRegion.start-0.5)<1e-7 && noteRegion.beats==1,
              "MIDI Note -> Position uses grid divisions and preserves length / speed without banks");
        check(p.getNameForMidiNoteNumber(127,1)==std::optional<juce::String>{"Grid 128"},"all 128 MIDI notes have position labels");
        midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1,127,1.0f),0); p.processBlock(audio,midi);
        check(std::abs(p.getViewState().loop.start-1.5)<1e-7,"position beyond sample clamps to last valid start");
        p.setNoteSettings(0,60); midi.clear(); p.positionParameter->setValueNotifyingHost(0.2f); p.processBlock(audio,midi);
        check(std::abs(p.getViewState().loop.start-0.25)<1e-7,"host position automation is visible after MIDI movement and follows Snap");
        p.setUiSettings(5,1,false,false,false); p.positionParameter->setValueNotifyingHost(0.2f); p.processBlock(audio,midi);
        check(std::abs(p.getViewState().loop.start-0.3)<1e-6,"host position automation is continuous with Snap OFF");

        auto palette=loopXPalette(2); palette.selection=juce::Colour(0x48112233); palette.loopFill=juce::Colour(0x80336699);
        palette.slot=juce::Colour(0xffee9955); palette.grid=juce::Colour(0xff574b67); palette.scrollThumb=juce::Colour(0xff807060);
        p.setCustomTheme(palette,"My Studio"); p.saveUserTheme("My Studio");
        const auto themeFile=p.exportTheme(); LoopXPalette parsed; juce::String parsedName;
        check(loopXReadTheme(juce::JSON::parse(themeFile),parsed,parsedName) && parsed==palette && parsedName=="My Studio",
              "theme JSON round-trips all 33 colours including independent alpha");
        check(!p.importTheme("{}") && p.getViewState().palette==palette,"invalid theme imports leave current palette untouched");
        check(p.renameUserTheme(0,"Renamed Studio") && p.loadUserTheme(0) && p.getViewState().themeName=="Renamed Studio",
              "user themes can be saved, renamed and reloaded without changing built-ins");
        p.setUiSettings(6,4,false,false,true); p.setDisplaySettings(true,true); p.setNoteSettings(1,48);
        juce::MemoryBlock completeState; p.getStateInformation(completeState);
        MiniSamplerAudioProcessor fresh(juce::File{}); fresh.setTheme(4); fresh.setUiSettings(1,1,true,false,false);
        StateQueryHost restoreHost; fresh.addListener(&restoreHost);
        fresh.setStateInformation(completeState.getData(),int(completeState.getSize()));
        check(restoreHost.changes==0,"state restore does not synchronously notify host under state lock");
        fresh.removeListener(&restoreHost); waitForLoad(fresh);
        const auto complete=fresh.getViewState();
        check(complete.palette==palette && complete.customTheme && complete.themeName=="Renamed Studio" && complete.grid==6 && complete.segments==4 &&
              !complete.snap && complete.zeroCross && complete.brightGrid && complete.stereoWaveform && complete.noteMode==1 && complete.rootNote==48,
              "fresh processor restores full theme + last grid settings; DAW state overrides defaults");
        auto* freshEditor=static_cast<MiniSamplerAudioProcessorEditor*>(fresh.createEditor());
        MiniSamplerWaveformView* freshWave=nullptr;
        for(auto* child:freshEditor->getChildren()) if(auto* w=dynamic_cast<MiniSamplerWaveformView*>(child)) freshWave=w;
        check(freshWave && freshWave->brightGrid && freshWave->stereo,"restored editor uses project settings, not constructor defaults");
        // Background/BPM panel and its timer are destroyed synchronously with editor.
        freshEditor->mouseDown(eventFor(freshEditor,float(freshEditor->getWidth()-320),12,juce::ModifierKeys::leftButtonModifier));
        delete freshEditor; juce::MessageManager::getInstance()->runDispatchLoopUntil(50);

        const auto prefs=file.getSiblingFile(file.getFileNameWithoutExtension()+"-settings.json");
        { MiniSamplerAudioProcessor first(prefs); first.setCustomTheme(palette,"Persistent"); first.saveUserTheme("Persistent"); first.setUiSettings(6,4,false,false,true); }
        { MiniSamplerAudioProcessor second(prefs); const auto s=second.getViewState();
          check(s.grid==6 && s.segments==4 && !s.snap && s.zeroCross && s.palette==palette && s.themeName=="Persistent",
                "plugin deletion / new instance restores last-used theme and grid from preferences"); }
        prefs.deleteFile();
        StateQueryHost queried; fresh.addListener(&queried);
        fresh.setTheme(1); fresh.setUiSettings(2,2,true,false,false); fresh.setLoopSelection(0,0.5,1);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
        check(queried.changes>0,"host state queries during notifications do not deadlock loader / UI"); fresh.removeListener(&queried);

        const auto shutdownStart=juce::Time::getMillisecondCounterHiRes();
        for(int n=0;n<12;++n)
        {
            auto closing=std::make_unique<MiniSamplerAudioProcessor>(juce::File{}); closing->requestSampleLoad(file); waitForLoad(*closing);
            closing->matchBpm(80); auto closingEditor=std::unique_ptr<juce::AudioProcessorEditor>(closing->createEditor());
            closingEditor->mouseDown(eventFor(closingEditor.get(),float(closingEditor->getWidth()-320),12,juce::ModifierKeys::leftButtonModifier));
            closingEditor.reset(); closing.reset();
            juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
        }
        check(juce::Time::getMillisecondCounterHiRes()-shutdownStart<10000,"repeated unload during rendering with an open BPM panel completes promptly");
        MiniSamplerSample cancelling; cancelling.audio.makeCopyOf(source);
        check(!cancelling.buildWaveform([]{return true;}),"peak building supports immediate cooperative cancellation");
        editor.reset(); p.setPlayHead(nullptr);
        file.deleteFile();
        std::cout << "All MiniSampler checks passed\n"; return 0;
    }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << std::endl; file.deleteFile(); return 1; }
}
