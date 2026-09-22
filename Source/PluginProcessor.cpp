#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "StretchRender.h"
#include <algorithm>
#include <limits>

namespace {
double sane(double v, double fallback = 0) { return std::isfinite(v) && v >= 0 ? v : fallback; }
bool validBpm(double v) { return std::isfinite(v) && v >= 20 && v <= 400; }
// valueChanged runs for EVERY host write, including writing zero again after
// manual edits or velocity selection. Callbacks do only lock-free atomic work.
class SlotControl final : public juce::AudioParameterInt
{
public:
    SlotControl(std::atomic<unsigned>& v, std::atomic<double>& p, std::atomic<double>& n)
        : AudioParameterInt(juce::ParameterID{"slot",1}, "Slot", 0,10,0), version(v), position(p), note(n) {}
private:
    void valueChanged(int) override { position.store(-1); note.store(-1); ++version; }
    std::atomic<unsigned>& version; std::atomic<double>& position; std::atomic<double>& note;
};
class PositionControl final : public juce::AudioParameterFloat
{
public:
    PositionControl(std::atomic<double>& p, std::atomic<double>& n)
        : AudioParameterFloat(juce::ParameterID{"loopPosition",1}, "Loop Position", juce::NormalisableRange<float>{0,1},0), position(p), note(n) {}
private:
    void valueChanged(float value) override { position.store(value); note.store(-1); }
    std::atomic<double>& position; std::atomic<double>& note;
};
}
LoopXAudioProcessor::LoopXAudioProcessor(juce::File preferences)
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      Thread("LoopX loader"), preferencesFile(std::move(preferences))
{
    addParameter(slotParameter = new SlotControl(slotSelectionVersion, livePosition, notePosition));
    addParameter(positionParameter = new PositionControl(livePosition, notePosition));
    loadPreferences(); state.status = "Drop audio here"; startThread();
}
LoopXAudioProcessor::~LoopXAudioProcessor()
{
    cancelPendingUpdate(); signalThreadShouldExit(); notify();
    // Every decode/peak/render loop cooperatively checks cancellation. Never
    // force-kill a worker while it owns buffers or locks, or pump host messages.
    stopThread(-1);
}
juce::File LoopXAudioProcessor::defaultPreferencesFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("LoopX/settings.json");
}
void LoopXAudioProcessor::uiChanged()
{
    preferencesDirty.store(true); notify(); triggerAsyncUpdate();
}
void LoopXAudioProcessor::handleAsyncUpdate()
{
    // No host callbacks under stateLock, during state restoration, or on loader.
    if (int division = pendingLengthDivision.load(); division != 0)
    {
        applyMidiChannelLength(division);
        pendingLengthDivision.compare_exchange_strong(division, 0);
    }
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}
juce::var LoopXAudioProcessor::preferencesJson() const
{
    const juce::ScopedLock lock(stateLock);
    auto* obj = new juce::DynamicObject;
    obj->setProperty("format", "LoopXPreferences"); obj->setProperty("version", 2);
    obj->setProperty("grid", state.grid); obj->setProperty("segments", state.segments);
    obj->setProperty("snap", state.snap); obj->setProperty("triplet", state.triplet); obj->setProperty("zeroCross", state.zeroCross);
    obj->setProperty("brightGrid", state.brightGrid); obj->setProperty("stereoWaveform", state.stereoWaveform);
    obj->setProperty("midiKeyTracking", state.midiKeyTracking); obj->setProperty("playbackMode", state.playbackMode);
    obj->setProperty("velocityMode", state.velocityMode); obj->setProperty("noteMode", state.noteMode);
    obj->setProperty("midiChannelLength", state.midiChannelLength);
    obj->setProperty("autoNoteNames", state.autoNoteNames);
    obj->setProperty("triggerMode", state.triggerMode); obj->setProperty("noteBehavior", state.noteBehavior);
    obj->setProperty("lengthControlMode", state.lengthControlMode);
    obj->setProperty("lengthChangeMode", state.lengthChangeMode); obj->setProperty("triggerTiming", state.triggerTiming);
    obj->setProperty("rootNote", state.rootNote);
    obj->setProperty("theme", state.theme); obj->setProperty("customTheme", state.customTheme);
    obj->setProperty("palette", loopXThemeJson(state.palette, state.themeName));
    juce::Array<juce::var> saved; for (const auto& theme : state.userThemes) saved.add(theme);
    obj->setProperty("userThemes", juce::var(saved)); return juce::var(obj);
}
void LoopXAudioProcessor::applyPreferences(const juce::var& json)
{
    const int version = int(json["version"]);
    if (json["format"].toString() != "LoopXPreferences" || (version != 1 && version != 2)) return;
    state.grid = juce::jlimit(1,6,int(json["grid"])); state.segments = juce::jlimit(1,5,int(json["segments"]));
    state.snap = bool(json["snap"]); state.triplet = bool(json["triplet"]); state.zeroCross = bool(json["zeroCross"]);
    state.brightGrid = bool(json["brightGrid"]); state.stereoWaveform = bool(json["stereoWaveform"]);
    state.midiKeyTracking = bool(json["midiKeyTracking"]); state.playbackMode = juce::jlimit(0,1,int(json["playbackMode"]));
    state.velocityMode = juce::jlimit(0,2,int(json["velocityMode"])); state.noteMode = juce::jlimit(0,2,int(json["noteMode"]));
    state.midiChannelLength = bool(json["midiChannelLength"]);
    state.autoNoteNames = !json.getDynamicObject()->hasProperty("autoNoteNames") || bool(json["autoNoteNames"]);
    if (version == 1)
    {
        const int legacyTrigger = juce::jlimit(0,2,int(json["triggerMode"]));
        state.triggerMode = legacyTrigger == 2 ? 1 : 0;
        state.noteBehavior = legacyTrigger == 1 ? 1 : 0;
        state.lengthControlMode = 1; state.triggerTiming = 0;
    }
    else
    {
        state.triggerMode = juce::jlimit(0,2,int(json["triggerMode"]));
        state.noteBehavior = juce::jlimit(0,3,int(json["noteBehavior"]));
        state.lengthControlMode = juce::jlimit(0,1,int(json["lengthControlMode"]));
        state.triggerTiming = juce::jlimit(0,4,int(json["triggerTiming"]));
    }
    state.lengthChangeMode = juce::jlimit(0,1,int(json["lengthChangeMode"]));
    state.rootNote = juce::jlimit(0,118,int(json["rootNote"]));
    state.theme = juce::jlimit(0,4,int(json["theme"])); state.customTheme = bool(json["customTheme"]);
    state.palette = loopXPalette(state.theme);
    const juce::StringArray names {"Studio Dark","Graphite","Slate","Warm Gray","Studio Light"}; state.themeName = names[state.theme];
    loopXReadTheme(json["palette"], state.palette, state.themeName);
    state.userThemes.clear();
    if (auto* array = json["userThemes"].getArray()) for (const auto& t : *array)
    {
        LoopXPalette palette; juce::String name;
        if (state.userThemes.size() < 64 && loopXReadTheme(t, palette, name)) state.userThemes.push_back(loopXThemeJson(palette, name));
    }
    liveGrid.store(state.grid); liveSnap.store(state.snap); liveTriplet.store(state.triplet);
    midiKeyTracking.store(state.midiKeyTracking); playbackMode.store(state.playbackMode); velocityMode.store(state.velocityMode);
    noteMode.store(state.noteMode); rootNote.store(state.rootNote); midiChannelLength.store(state.midiChannelLength); autoNoteNames.store(state.autoNoteNames);
    triggerMode.store(state.triggerMode); noteBehavior.store(state.noteBehavior); lengthControlMode.store(state.lengthControlMode);
    lengthChangeMode.store(state.lengthChangeMode); triggerTiming.store(state.triggerTiming);
}
void LoopXAudioProcessor::loadPreferences()
{
    if (preferencesFile.existsAsFile() && preferencesFile.getSize() < 262144) applyPreferences(juce::JSON::parse(preferencesFile.loadFileAsString()));
}
void LoopXAudioProcessor::flushPreferences()
{
    if (!preferencesDirty.exchange(false) || preferencesFile == juce::File{}) return;
    const auto text = juce::JSON::toString(preferencesJson());
    static juce::CriticalSection fileLock; const juce::ScopedLock lock(fileLock);
    if (preferencesFile.getParentDirectory().createDirectory().wasOk()) preferencesFile.replaceWithText(text);
}
void LoopXAudioProcessor::prepareToPlay(double rate, int)
{
    outputRate = juce::jmax(1.0, rate); fallbackBeat = 0; expectedBeat = 0;
    heldNotes = {}; heldLengthNotes = {}; noteOrder = 0; loopMidiNote = 60; midiPhase = 0; gateGain = 0;
    latchGate = false; oneShotGate = false; performanceStarted = false; lengthBaseValid = false;
    queuedLengthAction = 0; queuedLengthTargetBeat = 0;
    heldLengthMask.store(0); activeLengthDivision.store(0); lastMidiChannel.store(0);
    notePosition.store(-1);
    lastAudioSample = nullptr; lastParameterSlot = slotParameter->get(); selectedSlot = lastParameterSlot; lastMode = -1; lastTriggerMode = -1;
    lastSelectionVersion = slotSelectionVersion.load(); lastVelocity = velocityMode.load();
    engine.prepare(outputRate);
    outputTail = {}; switchTail = {}; switchFade = 0; previousValid = false;
}
bool LoopXAudioProcessor::isBusesLayoutSupported(const BusesLayout& layout) const
{
    return layout.getMainInputChannelSet().isDisabled()
        && (layout.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
            || layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo());
}
void LoopXAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals; buffer.clear();
    audioReaders.fetch_add(1, std::memory_order_seq_cst);
    struct Guard { std::atomic<unsigned>& n; ~Guard() { n.fetch_sub(1, std::memory_order_seq_cst); } } guard{audioReaders};
    double beat = fallbackBeat; bool connected = false, playing = false;
    if (auto* host = getPlayHead()) if (auto info = host->getPosition())
    {
        connected = true; playing = info->getIsPlaying();
        if (auto bpm = info->getBpm(); bpm.hasValue() && validBpm(*bpm)) projectTempo.store(*bpm);
        if (auto signature = info->getTimeSignature())
        { numerator.store(juce::jmax(1, signature->numerator)); denominator.store(juce::jmax(1, signature->denominator)); }
        if (auto ppq = info->getPpqPosition(); ppq.hasValue() && std::isfinite(*ppq)) beat = *ppq;
    }
    hostConnected.store(connected); hostPlaying.store(playing);
    const double beatStep = projectTempo.load() / (60.0 * outputRate);
    if (!connected || playing) fallbackBeat = beat + buffer.getNumSamples() * beatStep;
    const LoopXSample* sample = nullptr; double offset = 0, gridTempo = 0; int count = 0;
    bool coherent = false;
    // Bounded seqlock: never wait on the UI/loader. All associated buffer and
    // coordinate changes are one transaction. Old buffers die on loader only.
    for (int attempt = 0; attempt < 2 && !coherent; ++attempt)
    {
        const auto seq = loopSequence.load(std::memory_order_seq_cst); if (seq & 1u) continue;
        std::array<Loop, 11> snapshot {};
        sample = audioSample.load(std::memory_order_seq_cst); offset = sourceOffset.load(); gridTempo = timelineTempo.load();
        count = regionCount.load();
        for (int r = 0; r < 11; ++r)
            snapshot[r] = {regions[r].start.load(), regions[r].end.load(), regions[r].beats.load(),
                           regions[r].fadeIn.load(), regions[r].fadeOut.load(),
                           regions[r].fadeInCurve.load(), regions[r].fadeOutCurve.load()};
        if (loopSequence.load(std::memory_order_seq_cst) == seq) { rtRegions = snapshot; coherent = true; }
    }
    if (!coherent || !sample || sample->audio.getNumSamples() < 2) { playbackSeconds.store(-1); return; }
    const bool sampleChanged = sample != lastAudioSample;
    if (sampleChanged) lastAudioSample = sample;
    const int mode = playbackMode.load(), velocity = velocityMode.load();
    if (mode != lastMode) { heldNotes = {}; gateGain = 0; engine.clear(); midiPhase = 0; lastMode = mode; }
    const int trigger = triggerMode.load();
    if (trigger != lastTriggerMode) { heldNotes = {}; latchGate = false; oneShotGate = false; gateGain = 0; lastTriggerMode = trigger; }
    const int parameterSlot = slotParameter->get();
    if (velocity != lastVelocity) { selectedSlot = parameterSlot; lastVelocity = velocity; midiPhase = 0; }
    const auto selectionVersion = slotSelectionVersion.load();
    if (selectionVersion != lastSelectionVersion)
    {
        if (selectedSlot != parameterSlot) { selectedSlot = parameterSlot; midiPhase = 0; }
        lastSelectionVersion = selectionVersion;
    }
    if (parameterSlot != lastParameterSlot)
    { selectedSlot = parameterSlot; lastParameterSlot = parameterSlot; midiPhase = 0; }
    const bool seek = std::abs(beat - expectedBeat) > juce::jmax(0.0001, beatStep * 2);
    expectedBeat = beat + buffer.getNumSamples() * beatStep;
    auto event = midi.cbegin(); const auto eventEnd = midi.cend();
    bool gate = trigger == 2 ? latchGate : std::any_of(heldNotes.begin(), heldNotes.end(), [](uint64_t order){ return order != 0; });
    double cursor = -1;
    const auto* const* input = sample->audio.getArrayOfReadPointers();
    const double logicalDuration = sample->duration() - offset;
    const auto applyRealtimeLength = [&](int division)
    {
        const int regionIndex = juce::jlimit(0, count, selectedSlot);
        auto& target = rtRegions[size_t(regionIndex)];
        const double tempo = gridTempo > 0 ? gridTempo : projectTempo.load();
        const double beats = LoopMath::divisionBeats(division, numerator.load(), denominator.load());
        const double end = juce::jmin(logicalDuration, target.start + beats * 60.0 / tempo);
        if (end <= target.start + 0.001) return false;
        target.end = end; target.beats = (end - target.start) * tempo / 60.0;
        target.fadeIn = juce::jmin(target.fadeIn, (end - target.start) * 0.5);
        target.fadeOut = juce::jmin(target.fadeOut, (end - target.start) * 0.5);
        return true;
    };
    const int pendingLength = pendingLengthDivision.load();
    const bool pendingLengthAtBlockStart = pendingLength > 0 && applyRealtimeLength(pendingLength);
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        bool restart = sampleChanged && i == 0;
        bool lengthChanged = pendingLengthAtBlockStart && i == 0;
        while (event != eventEnd && (*event).samplePosition <= i)
        {
            const auto m = (*event).getMessage();
            if (m.isNoteOn())
            {
                const bool lengthCommand = midiChannelLength.load() && m.getChannel() <= 5;
                if (lengthCommand)
                {
                    const int division = m.getChannel();
                    applyRealtimeLength(division);
                    pendingLengthDivision.store(division);
                    triggerAsyncUpdate();
                    lengthChanged = true;
                    // With channel mapping enabled, channels 1-5 are control
                    // messages only. Trigger notes remain available on 6-16.
                    ++event; continue;
                }
                const bool wasGate = gate;
                heldNotes[size_t((m.getChannel() - 1) * 128 + m.getNoteNumber())] = ++noteOrder;
                loopMidiNote = m.getNoteNumber();
                if (trigger == 2)
                {
                    latchGate = !latchGate; gate = latchGate;
                    if (gate) { midiPhase = 0; restart = true; }
                }
                else
                {
                    gate = true;
                    if (trigger == 0 || !wasGate) { midiPhase = 0; restart = true; }
                }
                if (noteMode.load() == 1)
                {
                    selectedSlot = m.getNoteNumber() - rootNote.load() + 1;
                    if (selectedSlot < 1 || selectedSlot > 10) selectedSlot = 11;
                    livePosition.store(-1); notePosition.store(-1);
                }
                else if (noteMode.load() == 2)
                {
                    const int index = m.getNoteNumber();
                    const double gridStep = LoopMath::divisionBeats(liveGrid.load(), numerator.load(), denominator.load())
                        * (liveTriplet.load() ? 2.0/3 : 1) * 60.0 / (gridTempo > 0 ? gridTempo : projectTempo.load());
                    notePosition.store(index * gridStep); livePosition.store(-1);
                }
                else
                {
                    if (velocity == 1) { selectedSlot = velocitySlot(m.getVelocity()); livePosition.store(-1); notePosition.store(-1); }
                    if (velocity == 2) { livePosition.store(double(m.getVelocity() - 1) / 126.0); notePosition.store(-1); }
                }
            }
            if (m.isNoteOff())
            {
                heldNotes[size_t((m.getChannel() - 1) * 128 + m.getNoteNumber())] = 0;
                if (trigger != 2)
                {
                    const auto latest = std::max_element(heldNotes.begin(), heldNotes.end());
                    gate = *latest != 0; if (gate) loopMidiNote = int(std::distance(heldNotes.begin(), latest)) % 128;
                }
            }
            if (m.isAllNotesOff() || m.isAllSoundOff()) { heldNotes = {}; latchGate = false; gate = false; }
            ++event;
        }
        liveSlot.store(selectedSlot);
        Loop region = selectedSlot >= 0 && selectedSlot <= count ? rtRegions[size_t(selectedSlot)] : Loop{};
        const double len = region.end - region.start;
        const double automatedPosition = livePosition.load();
        const double noteStart = notePosition.load();
        if ((automatedPosition >= 0 || noteStart >= 0) && len > 0)
        {
            region.start = automatedPosition >= 0 ? automatedPosition * juce::jmax(0.0, logicalDuration - len) : noteStart;
            if (liveSnap.load())
            {
                const double step = LoopMath::divisionBeats(liveGrid.load(), numerator.load(), denominator.load())
                    * (liveTriplet.load() ? 2.0 / 3 : 1) * 60.0 / (gridTempo > 0 ? gridTempo : projectTempo.load());
                region.start = std::round(region.start / step) * step;
            }
            region.start = juce::jlimit(0.0, juce::jmax(0.0, logicalDuration - len), region.start); region.end = region.start + len;
        }
        const bool valid = loopEnabled.load() && region.end > region.start + 0.00001 && region.beats > 0;
        if (!valid)
        {
            if (previousValid) { switchTail = outputTail; switchFade = 96; }
            previousValid = false; gateGain = 0;
            for (int ch = 0; ch < 2; ++ch) outputTail[size_t(ch)] = switchFade > 0 ? switchTail[size_t(ch)] * float(switchFade - 1) / 96 : 0;
            if (switchFade > 0) --switchFade;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch) buffer.setSample(ch, i, outputTail[size_t(juce::jmin(ch,1))]);
            continue;
        }
        previousValid = true;
        if (region.start != audioLoop.start || region.end != audioLoop.end || region.beats != audioLoop.beats)
        {
            const bool lengthOnly = lengthChanged && region.start == audioLoop.start;
            audioLoop = region;
            if (!lengthOnly || lengthChangeMode.load() == 1) midiPhase = 0;
            restart = true;
        }
        const double pitch = midiKeyTracking.load() && noteMode.load() == 0 ? std::pow(2.0, (loopMidiNote - 60) / 12.0) : 1;
        const double phase = mode == 1 ? LoopMath::phase((beat + i * beatStep) * pitch, region.beats) : midiPhase;
        const double a = (offset + region.start) * sample->rate, b = (offset + region.end) * sample->rate;
        const double step = sample->rate / outputRate * pitch;
        if (restart || (seek && mode == 1 && i == 0))
        {
            engine.reset(a + phase * (b - a), step);
            switchTail = outputTail; switchFade = 96;
        }
        const bool audible = mode == 0 ? gate : (!connected || playing);
        gateGain += juce::jlimit(-1.0 / 96, 1.0 / 96, (audible ? 1.0 : 0.0) - gateGain);
        if (gateGain > 0 || audible)
        {
            auto value = engine.next(input, sample->audio.getNumChannels(), sample->audio.getNumSamples(), a, b, phase, step, sample->rate);
            const double lengthSeconds = region.end - region.start;
            const double linearIn = region.fadeIn > 0 ? phase * lengthSeconds / region.fadeIn : 1;
            const double linearOut = region.fadeOut > 0 ? (1 - phase) * lengthSeconds / region.fadeOut : 1;
            const double inGain = std::pow(juce::jlimit(0.0,1.0,linearIn), std::pow(4.0,-region.fadeInCurve));
            const double outGain = std::pow(juce::jlimit(0.0,1.0,linearOut), std::pow(4.0,-region.fadeOutCurve));
            const float gain = float(gateGain * juce::jlimit(0.0, 1.0, juce::jmin(inGain, outGain)));
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
                const auto index = size_t(juce::jmin(ch, 1));
                const float fresh = value[index] * gain;
                const float mix = switchFade > 0 ? float(97 - switchFade) / 96 : 1;
                const float result = switchTail[index] + mix * (fresh - switchTail[index]);
                buffer.setSample(ch, i, result); outputTail[index] = result;
            }
        }
        else outputTail = {};
        if (switchFade > 0) --switchFade;
        if (audible) cursor = region.start + phase * (region.end - region.start);
        if (mode == 0) { midiPhase += beatStep * pitch / region.beats; midiPhase -= std::floor(midiPhase); }
    }
    playbackSeconds.store(cursor);
}
void LoopXAudioProcessor::requestSampleLoad(const juce::File& file, bool restoring)
{
    if (!file.existsAsFile()) return;
    const juce::ScopedLock lock(stateLock);
    pendingFile = file; pendingRestore = restoring; ++requestVersion; ++transformVersion;
    loading.store(true); state.status = "Loading " + file.getFileName(); notify();
}
void LoopXAudioProcessor::run()
{
    uint64_t handled = 0; juce::AudioFormatManager formats; formats.registerBasicFormats();
    while (!threadShouldExit())
    {
        flushPreferences();
        juce::File file; uint64_t request; bool restore = false;
        { const juce::ScopedLock lock(stateLock); request = requestVersion; if (request != handled) { file = pendingFile; restore = pendingRestore; } }
        if (request != handled)
        {
            std::shared_ptr<LoopXSample> original;
            try
            {
                std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
                if (reader && reader->lengthInSamples > 1 && reader->lengthInSamples <= 20000000 && reader->sampleRate > 0 && reader->numChannels > 0 && !threadShouldExit())
                {
                    original = std::make_shared<LoopXSample>(); original->rate = reader->sampleRate; original->file = file;
                    original->audio.setSize(juce::jmin(2, int(reader->numChannels)), int(reader->lengthInSamples));
                    const auto cancelled = [this, request]
                    { const juce::ScopedLock lock(stateLock); return threadShouldExit() || requestVersion != request; };
                    for (int first = 0; original && first < original->audio.getNumSamples(); first += 65536)
                    {
                        const int count = juce::jmin(65536, original->audio.getNumSamples() - first);
                        if (cancelled() || !reader->read(&original->audio, first, count, first, true, true)) original.reset();
                    }
                    if (original && !original->buildWaveform(cancelled)) original.reset();
                }
            } catch (const std::exception&) { original.reset(); }
            {
                const juce::ScopedLock lock(stateLock);
                if (request == requestVersion)
                {
                    if (original)
                    {
                        state.originalSample = original;
                        if (!restore)
                        {
                            state.startOffset = 0; state.stretchApplied = false;
                            state.originalBpm = state.targetBpm = projectTempo.load(); appliedOffset = 0; appliedRatio = 1;
                            restoredCoordinates = false;
                            if (state.loop.end <= state.loop.start) { state.loop = {0, original->duration(), original->duration() * state.originalBpm / 60}; loopEnabled.store(true); }
                        }
                        else restoredCoordinates = true;
                        transformPending = true; ++transformVersion;
                    }
                    else { state.status = "Cannot decode " + file.getFileName(); loading.store(false); }
                }
            }
            handled = request;
        }
        ViewState job; uint64_t jobVersion = 0; bool doTransform = false;
        {
            const juce::ScopedLock lock(stateLock);
            if (state.originalSample && state.stretchApplied && std::abs(state.targetBpm - projectTempo.load()) > 0.00001)
            { state.targetBpm = projectTempo.load(); transformPending = true; ++transformVersion; loading.store(true); }
            if (transformPending && state.originalSample)
            { job = state; jobVersion = transformVersion; transformPending = false; doTransform = true; }
        }
        if (doTransform)
        {
            const double maxOffset = (job.originalSample->audio.getNumSamples() - 2) / job.originalSample->rate;
            const double offset = std::round(juce::jlimit(0.0, maxOffset, job.startOffset) * job.originalSample->rate) / job.originalSample->rate;
            const double ratio = job.stretchApplied ? job.originalBpm / job.targetBpm : 1;
            std::shared_ptr<const LoopXSample> processed;
            juce::String error;
            try
            {
                if (std::abs(ratio - 1) < 1.0e-9) processed = job.originalSample;
                else processed = renderBpmSample(*job.originalSample, offset, ratio, [this, jobVersion, bpm = job.targetBpm]
                {
                    const juce::ScopedLock lock(stateLock);
                    return threadShouldExit() || transformVersion != jobVersion || std::abs(projectTempo.load() - bpm) > 0.00001;
                });
            } catch (const std::exception& e) { error = e.what(); }
            {
                const juce::ScopedLock lock(stateLock);
                if (jobVersion == transformVersion)
                {
                    if (processed)
                    {
                        if (state.sample) retired.push_back(state.sample);
                        const double newDuration = processed->duration() - (processed == job.originalSample ? offset : 0);
                        const auto convert = [&](Loop& loop)
                        {
                            const double oldLength = loop.end - loop.start;
                            const double oldBeats = loop.beats;
                            if (!restoredCoordinates)
                            {
                                loop.start = ((appliedOffset + loop.start / appliedRatio) - offset) * ratio;
                                loop.end = ((appliedOffset + loop.end / appliedRatio) - offset) * ratio;
                                loop.fadeIn *= ratio / appliedRatio; loop.fadeOut *= ratio / appliedRatio;
                            }
                            loop.start = juce::jlimit(0.0, newDuration, loop.start);
                            loop.end = juce::jlimit(loop.start, newDuration, loop.end);
                            const double length = loop.end - loop.start;
                            loop.fadeIn = juce::jlimit(0.0, length * 0.5, loop.fadeIn); loop.fadeOut = juce::jlimit(0.0, length * 0.5, loop.fadeOut);
                            if (job.stretchApplied) loop.beats = length * job.targetBpm / 60;
                            else if (oldLength > 0) loop.beats = oldBeats * length / oldLength;
                        };
                        convert(state.loop); for (auto& slot : state.slots) convert(slot);
                        state.slots.erase(std::remove_if(state.slots.begin(), state.slots.end(), [](const Loop& r){ return r.end - r.start < 0.001; }), state.slots.end());
                        state.sample = processed; state.startOffset = offset; state.playbackOffset = processed == job.originalSample ? offset : 0;
                        appliedOffset = offset; appliedRatio = ratio; appliedTempo = job.stretchApplied ? job.targetBpm : 0; restoredCoordinates = false;
                        if (state.loop.end - state.loop.start < 0.001) loopEnabled.store(false);
                        publishLoop(state.loop); state.status = processed->file.getFileName(); loading.store(false);
                    }
                    else if (!error.isEmpty()) { state.status = error; loading.store(false); }
                    else { transformPending = true; }
                }
            }
        }
        if (audioReaders.load(std::memory_order_seq_cst) == 0) retired.clear();
        wait(25);
    }
    flushPreferences();
}
LoopXAudioProcessor::ViewState LoopXAudioProcessor::getViewState() const
{
    const juce::ScopedLock lock(stateLock); auto copy = state;
    const int slot = velocityMode.load() != 0 || noteMode.load() != 0 ? liveSlot.load() : slotParameter->get();
    if (slot > 0 && slot <= int(copy.slots.size())) copy.loop = copy.slots[size_t(slot - 1)];
    const double position = livePosition.load(), length = copy.loop.end - copy.loop.start;
    const double noteStart = notePosition.load();
    if ((position >= 0 || noteStart >= 0) && copy.sample && length > 0)
    {
        double start = position >= 0 ? position * juce::jmax(0.0, copy.sample->duration() - copy.playbackOffset - length) : noteStart;
        if (copy.snap)
        {
            const double tempo = timelineTempo.load() > 0 ? timelineTempo.load() : projectTempo.load();
            const double step = LoopMath::divisionBeats(copy.grid, numerator.load(), denominator.load()) * (copy.triplet ? 2.0 / 3 : 1) * 60 / tempo;
            start = std::round(start / step) * step;
        }
        start = juce::jlimit(0.0, juce::jmax(0.0, copy.sample->duration() - copy.playbackOffset - length), start);
        copy.loop.start = start; copy.loop.end = start + length;
    }
    return copy;
}
void LoopXAudioProcessor::publishLoop(const Loop&)
{
    loopSequence.fetch_add(1, std::memory_order_seq_cst);
    for (int i = 0; i < 11; ++i)
    {
        const auto loop = i == 0 ? state.loop : i <= int(state.slots.size()) ? state.slots[size_t(i - 1)] : Loop{};
        regions[i].start.store(loop.start); regions[i].end.store(loop.end); regions[i].beats.store(loop.beats);
        regions[i].fadeIn.store(loop.fadeIn); regions[i].fadeOut.store(loop.fadeOut);
        regions[i].fadeInCurve.store(loop.fadeInCurve); regions[i].fadeOutCurve.store(loop.fadeOutCurve);
    }
    regionCount.store(int(state.slots.size())); sourceOffset.store(state.playbackOffset);
    timelineTempo.store(appliedTempo);
    audioSample.store(state.sample.get(), std::memory_order_seq_cst);
    loopSequence.fetch_add(1, std::memory_order_seq_cst);
}
void LoopXAudioProcessor::setLoopSelection(double start, double end, double beats)
{
    { const juce::ScopedLock lock(stateLock); if (!state.sample) return;
    const double duration = state.sample->duration() - state.playbackOffset;
    start = juce::jlimit(0.0, duration, start); end = juce::jlimit(start, duration, end);
    if (end - start < 0.001) return;
    const auto previous = getViewState().loop;
    const double tempo = timelineTempo.load() > 0 ? timelineTempo.load() : projectTempo.load();
    state.loop = {start, end, beats > 0 ? beats : (end - start) * tempo / 60,
                  juce::jmin(previous.fadeIn, (end - start) * 0.5), juce::jmin(previous.fadeOut, (end - start) * 0.5),
                  previous.fadeInCurve, previous.fadeOutCurve};
    livePosition.store(-1); notePosition.store(-1); publishLoop(state.loop); loopEnabled.store(true); }
    selectSlot(0); uiChanged();
}
void LoopXAudioProcessor::stopLoop() { setLoopEnabled(false); }
void LoopXAudioProcessor::setLoopEnabled(bool enabled) { loopEnabled.store(enabled); triggerAsyncUpdate(); }
void LoopXAudioProcessor::selectSlot(int slot)
{
    slotParameter->beginChangeGesture(); slotParameter->setValueNotifyingHost(slotParameter->convertTo0to1(juce::jlimit(0, 10, slot))); slotParameter->endChangeGesture();
    liveSlot.store(slot); livePosition.store(-1); notePosition.store(-1);
}
void LoopXAudioProcessor::saveSlot()
{
    const juce::ScopedLock lock(stateLock); const auto selected = getViewState().loop;
    if (selected.end > selected.start && state.slots.size() < 10) { state.slots.push_back(selected); publishLoop(state.loop); }
    uiChanged();
}
void LoopXAudioProcessor::recallSlot(int index)
{
    { const juce::ScopedLock lock(stateLock); if (!juce::isPositiveAndBelow(index, int(state.slots.size()))) return; }
    selectSlot(index + 1); setLoopEnabled(true);
}
void LoopXAudioProcessor::deleteSlot(int index)
{
    int selected = -1;
    { const juce::ScopedLock lock(stateLock);
    if (juce::isPositiveAndBelow(index, int(state.slots.size())))
    {
        state.slots.erase(state.slots.begin() + index);
        selected = slotParameter->get(); selected = selected == index + 1 ? 0 : selected > index + 1 ? selected - 1 : selected;
        publishLoop(state.loop);
    }
    }
    if (selected >= 0) { selectSlot(selected); uiChanged(); }
}
void LoopXAudioProcessor::setUiSettings(int grid, int segments, bool snap, bool triplet, bool zeroCross)
{
    { const juce::ScopedLock lock(stateLock);
      state.grid = juce::jlimit(1, 6, grid); state.segments = juce::jlimit(1, 5, segments); state.snap = snap; state.triplet = triplet; state.zeroCross = zeroCross;
      liveGrid.store(state.grid); liveSnap.store(snap); liveTriplet.store(triplet); }
    uiChanged();
}
void LoopXAudioProcessor::setDisplaySettings(bool bright, bool stereo)
{
    { const juce::ScopedLock lock(stateLock); state.brightGrid = bright; state.stereoWaveform = stereo; }
    uiChanged();
}
void LoopXAudioProcessor::setPlaybackSettings(int mode, int velocity)
{
    { const juce::ScopedLock lock(stateLock);
      state.playbackMode = juce::jlimit(0, 1, mode); state.velocityMode = juce::jlimit(0, 2, velocity);
      playbackMode.store(state.playbackMode); velocityMode.store(state.velocityMode); livePosition.store(-1); }
    uiChanged();
}
void LoopXAudioProcessor::setTheme(int theme)
{
    { const juce::ScopedLock lock(stateLock);
      state.theme = juce::jlimit(0, 4, theme); state.palette = loopXPalette(state.theme); state.customTheme = false;
      const juce::StringArray names {"Studio Dark","Graphite","Slate","Warm Gray","Studio Light"}; state.themeName = names[state.theme]; }
    uiChanged();
}
void LoopXAudioProcessor::setCustomTheme(const LoopXPalette& palette, juce::String name)
{
    { const juce::ScopedLock lock(stateLock); state.palette = palette; state.customTheme = true; state.themeName = name.trim().substring(0,80); }
    uiChanged();
}
void LoopXAudioProcessor::saveUserTheme(juce::String name)
{
    name = name.trim().substring(0,80); if (name.isEmpty()) name = "Custom";
    { const juce::ScopedLock lock(stateLock); state.themeName = name; state.customTheme = true;
      const auto doc = loopXThemeJson(state.palette,name); bool replaced = false;
      for (auto& saved : state.userThemes) if (saved["name"].toString() == name) { saved = doc; replaced = true; break; }
      if (!replaced && state.userThemes.size() < 64) state.userThemes.push_back(doc); }
    uiChanged();
}
bool LoopXAudioProcessor::loadUserTheme(int index)
{
    LoopXPalette palette; juce::String name;
    { const juce::ScopedLock lock(stateLock);
      if (!juce::isPositiveAndBelow(index,int(state.userThemes.size())) || !loopXReadTheme(state.userThemes[size_t(index)],palette,name)) return false; }
    setCustomTheme(palette,name); return true;
}
bool LoopXAudioProcessor::renameUserTheme(int index, juce::String name)
{
    name = name.trim().substring(0,80); if (name.isEmpty()) return false;
    { const juce::ScopedLock lock(stateLock);
      if (!juce::isPositiveAndBelow(index,int(state.userThemes.size()))) return false;
      for (size_t i=0; i<state.userThemes.size(); ++i) if (int(i)!=index && state.userThemes[i]["name"].toString()==name) return false;
      LoopXPalette palette; juce::String old;
      if (!loopXReadTheme(state.userThemes[size_t(index)],palette,old)) return false;
      state.userThemes[size_t(index)] = loopXThemeJson(palette,name); state.themeName=name; }
    uiChanged(); return true;
}
bool LoopXAudioProcessor::importTheme(const juce::String& text)
{
    if (text.length() > 65536) return false;
    LoopXPalette palette; juce::String name;
    if (!loopXReadTheme(juce::JSON::parse(text),palette,name)) return false;
    setCustomTheme(palette,name); saveUserTheme(name); return true;
}
juce::String LoopXAudioProcessor::exportTheme() const
{
    const juce::ScopedLock lock(stateLock); return juce::JSON::toString(loopXThemeJson(state.palette,state.themeName));
}
void LoopXAudioProcessor::setNoteSettings(int mode, int root)
{
    { const juce::ScopedLock lock(stateLock); state.noteMode = juce::jlimit(0,2,mode);
      state.rootNote = juce::jlimit(0,118,root);
      noteMode.store(state.noteMode); rootNote.store(state.rootNote);
      livePosition.store(-1); notePosition.store(-1); liveSlot.store(slotParameter->get()); }
    ++slotSelectionVersion;
    uiChanged();
}
void LoopXAudioProcessor::setMidiChannelLengthEnabled(bool enabled)
{
    { const juce::ScopedLock lock(stateLock); state.midiChannelLength = enabled; midiChannelLength.store(enabled); }
    if (!enabled) pendingLengthDivision.store(0);
    uiChanged();
}
void LoopXAudioProcessor::setAutoNoteNamesEnabled(bool enabled)
{
    { const juce::ScopedLock lock(stateLock); state.autoNoteNames = enabled; autoNoteNames.store(enabled); }
    uiChanged();
}
void LoopXAudioProcessor::setMidiTriggerMode(int mode)
{
    { const juce::ScopedLock lock(stateLock); state.triggerMode = juce::jlimit(0,2,mode); triggerMode.store(state.triggerMode); }
    uiChanged();
}
void LoopXAudioProcessor::setLengthChangeMode(int mode)
{
    { const juce::ScopedLock lock(stateLock); state.lengthChangeMode = juce::jlimit(0,1,mode); lengthChangeMode.store(state.lengthChangeMode); }
    uiChanged();
}
void LoopXAudioProcessor::applyMidiChannelLength(int division)
{
    if (!midiChannelLength.load() || division < 1 || division > 5) return;
    const juce::ScopedLock lock(stateLock);
    if (!state.sample) return;
    const int slot = velocityMode.load() == 1 || noteMode.load() == 1 ? liveSlot.load() : slotParameter->get();
    auto& loop = slot > 0 && slot <= int(state.slots.size()) ? state.slots[size_t(slot - 1)] : state.loop;
    const double tempo = timelineTempo.load() > 0 ? timelineTempo.load() : projectTempo.load();
    const double duration = state.sample->duration() - state.playbackOffset;
    const double beats = LoopMath::divisionBeats(division, numerator.load(), denominator.load());
    const double end = juce::jmin(duration, loop.start + beats * 60.0 / tempo);
    if (end - loop.start < 0.001) return;
    loop.end = end;
    loop.beats = (loop.end - loop.start) * tempo / 60.0;
    loop.fadeIn = juce::jmin(loop.fadeIn, (loop.end - loop.start) * 0.5);
    loop.fadeOut = juce::jmin(loop.fadeOut, (loop.end - loop.start) * 0.5);
    publishLoop(state.loop);
}
std::optional<juce::String> LoopXAudioProcessor::getNameForMidiNoteNumber(int note, int)
{
    if (!autoNoteNames.load() || note < 0 || note > 127) return {};
    const int index = note - rootNote.load();
    if (noteMode.load() == 1 && index >= 0 && index < 10) return "Slot " + juce::String(index + 1);
    if (noteMode.load() == 2) return "Grid " + juce::String(note + 1);
    return {};
}
void LoopXAudioProcessor::setLoopFades(double in, double out)
{
    const juce::ScopedLock lock(stateLock); const int slot = velocityMode.load() == 1 || noteMode.load()==1 ? liveSlot.load() : slotParameter->get();
    auto& loop = slot > 0 && slot <= int(state.slots.size()) ? state.slots[size_t(slot - 1)] : state.loop;
    const double limit = (loop.end - loop.start) * 0.5;
    loop.fadeIn = juce::jlimit(0.0, limit, sane(in)); loop.fadeOut = juce::jlimit(0.0, limit, sane(out)); publishLoop(state.loop);
    uiChanged();
}
void LoopXAudioProcessor::setLoopFadeCurves(double in, double out)
{
    const juce::ScopedLock lock(stateLock); const int slot = velocityMode.load() == 1 || noteMode.load()==1 ? liveSlot.load() : slotParameter->get();
    auto& loop = slot > 0 && slot <= int(state.slots.size()) ? state.slots[size_t(slot - 1)] : state.loop;
    loop.fadeInCurve = juce::jlimit(-1.0,1.0,std::isfinite(in) ? in : 0.0);
    loop.fadeOutCurve = juce::jlimit(-1.0,1.0,std::isfinite(out) ? out : 0.0);
    publishLoop(state.loop); uiChanged();
}
void LoopXAudioProcessor::setStartOffset(double value)
{
    const juce::ScopedLock lock(stateLock); if (!state.originalSample || !std::isfinite(value)) return;
    state.startOffset = juce::jlimit(0.0, (state.originalSample->audio.getNumSamples() - 2) / state.originalSample->rate, value);
    transformPending = true; ++transformVersion; loading.store(true); notify();
    uiChanged();
}
bool LoopXAudioProcessor::matchBpm(double original)
{
    const juce::ScopedLock lock(stateLock);
    if (!state.originalSample || !validBpm(original)) return false;
    state.originalBpm = original; state.targetBpm = projectTempo.load(); state.stretchApplied = true;
    transformPending = true; ++transformVersion; loading.store(true); notify();
    uiChanged(); return true;
}
void LoopXAudioProcessor::setEditorSize(int width, int height) { const juce::ScopedLock lock(stateLock); state.width = width; state.height = height; }
void LoopXAudioProcessor::getStateInformation(juce::MemoryBlock& block)
{
    const juce::ScopedLock lock(stateLock); juce::XmlElement xml("LoopX");
    xml.setAttribute("version", 4); xml.setAttribute("file", state.originalSample ? state.originalSample->file.getFullPathName() : pendingFile.getFullPathName());
    xml.createNewChildElement("SettingsJson")->addTextElement(juce::JSON::toString(preferencesJson()));
    const auto writeLoop = [](juce::XmlElement& x, const Loop& loop)
    {
        x.setAttribute("start", loop.start); x.setAttribute("end", loop.end); x.setAttribute("beats", loop.beats);
        x.setAttribute("fadeIn", loop.fadeIn); x.setAttribute("fadeOut", loop.fadeOut);
        x.setAttribute("fadeInCurve", loop.fadeInCurve); x.setAttribute("fadeOutCurve", loop.fadeOutCurve);
    };
    writeLoop(xml, state.loop);
    xml.setAttribute("enabled", loopEnabled.load()); xml.setAttribute("grid", state.grid); xml.setAttribute("segments", state.segments);
    xml.setAttribute("snap", state.snap); xml.setAttribute("triplet", state.triplet); xml.setAttribute("zeroCross", state.zeroCross);
    xml.setAttribute("midiKeyTracking", state.midiKeyTracking); xml.setAttribute("brightGrid", state.brightGrid); xml.setAttribute("stereoWaveform", state.stereoWaveform);
    xml.setAttribute("playbackMode", state.playbackMode); xml.setAttribute("velocityMode", state.velocityMode); xml.setAttribute("theme", state.theme);
    xml.setAttribute("startOffset", state.startOffset); xml.setAttribute("originalBpm", state.originalBpm); xml.setAttribute("targetBpm", state.targetBpm);
    xml.setAttribute("stretchApplied", state.stretchApplied); xml.setAttribute("slot", slotParameter->get()); xml.setAttribute("loopPosition", double(positionParameter->get()));
    xml.setAttribute("positionOverride", livePosition.load()); xml.setAttribute("width", state.width); xml.setAttribute("height", state.height);
    for (const auto& slot : state.slots) writeLoop(*xml.createNewChildElement("Slot"), slot);
    copyXmlToBinary(xml, block);
}
void LoopXAudioProcessor::setStateInformation(const void* data, int size)
{
    const auto xml = getXmlFromBinary(data, size);
    // Accept the previous state tag so existing projects retain their settings.
    if (!xml || (!xml->hasTagName("LoopX") && !xml->hasTagName("MiniSamplerLoopX"))) return;
    const juce::ScopedLock lock(stateLock);
    const auto readLoop = [](const juce::XmlElement& x) { return Loop{sane(x.getDoubleAttribute("start")), sane(x.getDoubleAttribute("end")),
        sane(x.getDoubleAttribute("beats")), sane(x.getDoubleAttribute("fadeIn", 0.004)), sane(x.getDoubleAttribute("fadeOut", 0.004)),
        juce::jlimit(-1.0,1.0,x.getDoubleAttribute("fadeInCurve")), juce::jlimit(-1.0,1.0,x.getDoubleAttribute("fadeOutCurve"))}; };
    state.loop = readLoop(*xml); state.slots.clear();
    for (const auto* child : xml->getChildIterator()) if (child->hasTagName("Slot") && state.slots.size() < 10) state.slots.push_back(readLoop(*child));
    state.grid = juce::jlimit(1,6,xml->getIntAttribute("grid",3)); state.segments = juce::jlimit(1,5,xml->getIntAttribute("segments",1));
    state.snap = xml->getBoolAttribute("snap",true); state.triplet = xml->getBoolAttribute("triplet",false); state.zeroCross = xml->getBoolAttribute("zeroCross");
    liveGrid.store(state.grid); liveSnap.store(state.snap); liveTriplet.store(state.triplet);
    state.midiKeyTracking = xml->getBoolAttribute("midiKeyTracking", false); midiKeyTracking.store(state.midiKeyTracking);
    state.brightGrid = xml->getBoolAttribute("brightGrid",false); state.stereoWaveform = xml->getBoolAttribute("stereoWaveform",false);
    state.playbackMode = juce::jlimit(0,1,xml->getIntAttribute("playbackMode",0)); playbackMode.store(state.playbackMode);
    state.velocityMode = juce::jlimit(0,2,xml->getIntAttribute("velocityMode",0)); velocityMode.store(state.velocityMode);
    state.theme = juce::jlimit(0,4,xml->getIntAttribute("theme",0)); state.palette = loopXPalette(state.theme); state.customTheme = false;
    const juce::StringArray names {"Studio Dark","Graphite","Slate","Warm Gray","Studio Light"}; state.themeName = names[state.theme];
    state.noteMode = 0; state.rootNote = 60; state.midiChannelLength = false; state.autoNoteNames = true; state.triggerMode = 0; state.lengthChangeMode = 0;
    noteMode.store(0); rootNote.store(60); midiChannelLength.store(false); autoNoteNames.store(true); triggerMode.store(0); lengthChangeMode.store(0); notePosition.store(-1);
    if (auto* settings = xml->getChildByName("SettingsJson")) applyPreferences(juce::JSON::parse(settings->getAllSubText()));
    state.startOffset = sane(xml->getDoubleAttribute("startOffset"));
    state.originalBpm = xml->getDoubleAttribute("originalBpm", 120); if (!validBpm(state.originalBpm)) state.originalBpm = 120;
    state.targetBpm = xml->getDoubleAttribute("targetBpm", 120); if (!validBpm(state.targetBpm)) state.targetBpm = 120;
    state.stretchApplied = xml->getBoolAttribute("stretchApplied", false);
    projectTempo.store(state.targetBpm);
    state.width = juce::jlimit(900, 1800, xml->getIntAttribute("width", 1000)); state.height = juce::jlimit(260, 1100, xml->getIntAttribute("height", 390));
    const int savedSlot = juce::jlimit(0,10,xml->getIntAttribute("slot",0));
    // AudioParameterInt/Float make their overrides private; the normalized
    // setter is public on the base parameter. Restore without host callbacks.
    static_cast<juce::AudioProcessorParameter*>(slotParameter)->setValue(slotParameter->convertTo0to1(savedSlot)); liveSlot.store(savedSlot);
    static_cast<juce::AudioProcessorParameter*>(positionParameter)->setValue(positionParameter->convertTo0to1(float(juce::jlimit(0.0, 1.0, sane(xml->getDoubleAttribute("loopPosition"))))));
    livePosition.store(xml->getDoubleAttribute("positionOverride", -1));
    loopEnabled.store(xml->getBoolAttribute("enabled")); restoredCoordinates = true;
    publishLoop(state.loop);
    const juce::File file(xml->getStringAttribute("file")); if (file.existsAsFile()) requestSampleLoad(file, true); else state.status = "Sample missing - use Load audio file in Settings";
}
juce::AudioProcessorEditor* LoopXAudioProcessor::createEditor() { return new LoopXAudioProcessorEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new LoopXAudioProcessor(); }
