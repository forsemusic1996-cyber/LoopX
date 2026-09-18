#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "StretchRender.h"
#include <algorithm>
#include <limits>

namespace {
double sane(double v, double fallback = 0) { return std::isfinite(v) && v >= 0 ? v : fallback; }
bool validBpm(double v) { return std::isfinite(v) && v >= 20 && v <= 400; }
}
MiniSamplerAudioProcessor::MiniSamplerAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      Thread("MiniSampler loader")
{
    addParameter(slotParameter = new juce::AudioParameterInt(juce::ParameterID{"slot", 1}, "Slot", 0, 10, 0));
    addParameter(positionParameter = new juce::AudioParameterFloat(juce::ParameterID{"loopPosition", 1},
        "Loop Position", juce::NormalisableRange<float>{0, 1}, 0));
    state.status = "Drop audio here"; startThread();
}
MiniSamplerAudioProcessor::~MiniSamplerAudioProcessor() { signalThreadShouldExit(); notify(); stopThread(-1); }
void MiniSamplerAudioProcessor::prepareToPlay(double rate, int)
{
    outputRate = juce::jmax(1.0, rate); fallbackBeat = 0; expectedBeat = 0;
    heldNotes = {}; noteOrder = 0; loopMidiNote = 60; midiPhase = 0; gateGain = 0;
    lastAudioSample = nullptr; lastParameterSlot = slotParameter->get(); selectedSlot = lastParameterSlot; lastMode = -1;
    lastSelectionVersion = slotSelectionVersion.load(); lastVelocity = velocityMode.load();
    lastPositionParameter = positionParameter->get(); engine.prepare(outputRate);
    outputTail = {}; switchTail = {}; switchFade = 0; previousValid = false;
}
bool MiniSamplerAudioProcessor::isBusesLayoutSupported(const BusesLayout& layout) const
{
    return layout.getMainInputChannelSet().isDisabled()
        && (layout.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
            || layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo());
}
void MiniSamplerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
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
    const MiniSamplerSample* sample = nullptr; double offset = 0, gridTempo = 0; int count = 0;
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
                           regions[r].fadeIn.load(), regions[r].fadeOut.load()};
        if (loopSequence.load(std::memory_order_seq_cst) == seq) { rtRegions = snapshot; coherent = true; }
    }
    if (!coherent || !sample || sample->audio.getNumSamples() < 2) { playbackSeconds.store(-1); return; }
    const bool sampleChanged = sample != lastAudioSample;
    if (sampleChanged) lastAudioSample = sample;
    const int mode = playbackMode.load(), velocity = velocityMode.load();
    if (mode != lastMode) { heldNotes = {}; gateGain = 0; engine.clear(); midiPhase = 0; lastMode = mode; }
    const int parameterSlot = slotParameter->get();
    if (velocity != lastVelocity) { selectedSlot = parameterSlot; lastVelocity = velocity; midiPhase = 0; }
    const auto selectionVersion = slotSelectionVersion.load();
    if (selectionVersion != lastSelectionVersion)
    { selectedSlot = parameterSlot; lastSelectionVersion = selectionVersion; midiPhase = 0; }
    if (parameterSlot != lastParameterSlot)
    { selectedSlot = parameterSlot; lastParameterSlot = parameterSlot; livePosition.store(-1); midiPhase = 0; }
    const float position = positionParameter->get();
    if (position != lastPositionParameter) { livePosition.store(position); lastPositionParameter = position; }
    const bool seek = std::abs(beat - expectedBeat) > juce::jmax(0.0001, beatStep * 2);
    expectedBeat = beat + buffer.getNumSamples() * beatStep;
    auto event = midi.cbegin(); const auto eventEnd = midi.cend();
    bool gate = std::any_of(heldNotes.begin(), heldNotes.end(), [](uint64_t order){ return order != 0; });
    double cursor = -1;
    const auto* const* input = sample->audio.getArrayOfReadPointers();
    const double logicalDuration = sample->duration() - offset;
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        bool restart = sampleChanged && i == 0;
        while (event != eventEnd && (*event).samplePosition <= i)
        {
            const auto m = (*event).getMessage();
            if (m.isNoteOn())
            {
                heldNotes[size_t((m.getChannel() - 1) * 128 + m.getNoteNumber())] = ++noteOrder;
                loopMidiNote = m.getNoteNumber(); gate = true; midiPhase = 0; restart = true;
                if (velocity == 1) { selectedSlot = velocitySlot(m.getVelocity()); livePosition.store(-1); }
                if (velocity == 2) livePosition.store(double(m.getVelocity() - 1) / 126.0);
            }
            if (m.isNoteOff())
            {
                heldNotes[size_t((m.getChannel() - 1) * 128 + m.getNoteNumber())] = 0;
                const auto latest = std::max_element(heldNotes.begin(), heldNotes.end());
                gate = *latest != 0; if (gate) loopMidiNote = int(std::distance(heldNotes.begin(), latest)) % 128;
            }
            if (m.isAllNotesOff() || m.isAllSoundOff()) { heldNotes = {}; gate = false; }
            ++event;
        }
        liveSlot.store(selectedSlot);
        Loop region = selectedSlot >= 0 && selectedSlot <= count ? rtRegions[size_t(selectedSlot)] : Loop{};
        const double len = region.end - region.start;
        const double automatedPosition = livePosition.load();
        if (automatedPosition >= 0 && len > 0)
        {
            region.start = automatedPosition * juce::jmax(0.0, logicalDuration - len);
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
        { audioLoop = region; midiPhase = 0; restart = true; }
        const double pitch = midiKeyTracking.load() ? std::pow(2.0, (loopMidiNote - 60) / 12.0) : 1;
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
            const double inGain = region.fadeIn > 0 ? phase * lengthSeconds / region.fadeIn : 1;
            const double outGain = region.fadeOut > 0 ? (1 - phase) * lengthSeconds / region.fadeOut : 1;
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
void MiniSamplerAudioProcessor::requestSampleLoad(const juce::File& file, bool restoring)
{
    if (!file.existsAsFile()) return;
    const juce::ScopedLock lock(stateLock);
    pendingFile = file; pendingRestore = restoring; ++requestVersion; ++transformVersion;
    loading.store(true); state.status = "Loading " + file.getFileName(); notify();
}
void MiniSamplerAudioProcessor::run()
{
    uint64_t handled = 0; juce::AudioFormatManager formats; formats.registerBasicFormats();
    while (!threadShouldExit())
    {
        juce::File file; uint64_t request; bool restore = false;
        { const juce::ScopedLock lock(stateLock); request = requestVersion; if (request != handled) { file = pendingFile; restore = pendingRestore; } }
        if (request != handled)
        {
            std::shared_ptr<MiniSamplerSample> original;
            try
            {
                std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
                if (reader && reader->lengthInSamples > 1 && reader->lengthInSamples <= std::numeric_limits<int>::max() && reader->sampleRate > 0 && reader->numChannels > 0)
                {
                    original = std::make_shared<MiniSamplerSample>(); original->rate = reader->sampleRate; original->file = file;
                    original->audio.setSize(juce::jmin(2, int(reader->numChannels)), int(reader->lengthInSamples));
                    if (!reader->read(&original->audio, 0, original->audio.getNumSamples(), 0, true, true)) original.reset();
                    if (original) original->buildWaveform();
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
            std::shared_ptr<const MiniSamplerSample> processed;
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
}
MiniSamplerAudioProcessor::ViewState MiniSamplerAudioProcessor::getViewState() const
{
    const juce::ScopedLock lock(stateLock); auto copy = state;
    const int slot = velocityMode.load() != 0 ? liveSlot.load() : slotParameter->get();
    if (slot > 0 && slot <= int(copy.slots.size())) copy.loop = copy.slots[size_t(slot - 1)];
    const double position = livePosition.load(), length = copy.loop.end - copy.loop.start;
    if (position >= 0 && copy.sample && length > 0)
    {
        double start = position * juce::jmax(0.0, copy.sample->duration() - copy.playbackOffset - length);
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
void MiniSamplerAudioProcessor::publishLoop(const Loop&)
{
    loopSequence.fetch_add(1, std::memory_order_seq_cst);
    for (int i = 0; i < 11; ++i)
    {
        const auto loop = i == 0 ? state.loop : i <= int(state.slots.size()) ? state.slots[size_t(i - 1)] : Loop{};
        regions[i].start.store(loop.start); regions[i].end.store(loop.end); regions[i].beats.store(loop.beats);
        regions[i].fadeIn.store(loop.fadeIn); regions[i].fadeOut.store(loop.fadeOut);
    }
    regionCount.store(int(state.slots.size())); sourceOffset.store(state.playbackOffset);
    timelineTempo.store(appliedTempo);
    audioSample.store(state.sample.get(), std::memory_order_seq_cst);
    loopSequence.fetch_add(1, std::memory_order_seq_cst);
}
void MiniSamplerAudioProcessor::setLoopSelection(double start, double end, double beats)
{
    const juce::ScopedLock lock(stateLock); if (!state.sample) return;
    const double duration = state.sample->duration() - state.playbackOffset;
    start = juce::jlimit(0.0, duration, start); end = juce::jlimit(start, duration, end);
    if (end - start < 0.001) return;
    const auto previous = getViewState().loop;
    const double tempo = timelineTempo.load() > 0 ? timelineTempo.load() : projectTempo.load();
    state.loop = {start, end, beats > 0 ? beats : (end - start) * tempo / 60,
                  juce::jmin(previous.fadeIn, (end - start) * 0.5), juce::jmin(previous.fadeOut, (end - start) * 0.5)};
    livePosition.store(-1); selectSlot(0); publishLoop(state.loop); loopEnabled.store(true);
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}
void MiniSamplerAudioProcessor::stopLoop() { loopEnabled.store(false); }
void MiniSamplerAudioProcessor::selectSlot(int slot)
{
    slotParameter->beginChangeGesture(); slotParameter->setValueNotifyingHost(slotParameter->convertTo0to1(juce::jlimit(0, 10, slot))); slotParameter->endChangeGesture();
    liveSlot.store(slot); livePosition.store(-1); requestedSlot.store(slot); ++slotSelectionVersion;
}
void MiniSamplerAudioProcessor::saveSlot()
{
    const juce::ScopedLock lock(stateLock); const auto selected = getViewState().loop;
    if (selected.end > selected.start && state.slots.size() < 10) { state.slots.push_back(selected); publishLoop(state.loop); }
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}
void MiniSamplerAudioProcessor::recallSlot(int index)
{
    const juce::ScopedLock lock(stateLock); if (juce::isPositiveAndBelow(index, int(state.slots.size()))) { selectSlot(index + 1); loopEnabled.store(true); }
}
void MiniSamplerAudioProcessor::deleteSlot(int index)
{
    const juce::ScopedLock lock(stateLock);
    if (juce::isPositiveAndBelow(index, int(state.slots.size())))
    {
        state.slots.erase(state.slots.begin() + index);
        const int selected = slotParameter->get(); selectSlot(selected == index + 1 ? 0 : selected > index + 1 ? selected - 1 : selected);
        publishLoop(state.loop); updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
    }
}
void MiniSamplerAudioProcessor::setUiSettings(int grid, int segments, bool snap, bool triplet, bool zeroCross)
{
    { const juce::ScopedLock lock(stateLock);
      state.grid = juce::jlimit(1, 6, grid); state.segments = juce::jlimit(1, 5, segments); state.snap = snap; state.triplet = triplet; state.zeroCross = zeroCross;
      liveGrid.store(state.grid); liveSnap.store(snap); liveTriplet.store(triplet); }
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}
void MiniSamplerAudioProcessor::setDisplaySettings(bool bright, bool stereo)
{
    { const juce::ScopedLock lock(stateLock); state.brightGrid = bright; state.stereoWaveform = stereo; }
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}
void MiniSamplerAudioProcessor::setPlaybackSettings(int mode, int velocity)
{
    { const juce::ScopedLock lock(stateLock);
      state.playbackMode = juce::jlimit(0, 1, mode); state.velocityMode = juce::jlimit(0, 2, velocity);
      playbackMode.store(state.playbackMode); velocityMode.store(state.velocityMode); livePosition.store(-1); }
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}
void MiniSamplerAudioProcessor::setTheme(int theme)
{
    { const juce::ScopedLock lock(stateLock); state.theme = juce::jlimit(0, 4, theme); }
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}
void MiniSamplerAudioProcessor::setLoopFades(double in, double out)
{
    const juce::ScopedLock lock(stateLock); const int slot = velocityMode.load() == 1 ? liveSlot.load() : slotParameter->get();
    auto& loop = slot > 0 && slot <= int(state.slots.size()) ? state.slots[size_t(slot - 1)] : state.loop;
    const double limit = (loop.end - loop.start) * 0.5;
    loop.fadeIn = juce::jlimit(0.0, limit, sane(in)); loop.fadeOut = juce::jlimit(0.0, limit, sane(out)); publishLoop(state.loop);
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}
void MiniSamplerAudioProcessor::setStartOffset(double value)
{
    const juce::ScopedLock lock(stateLock); if (!state.originalSample || !std::isfinite(value)) return;
    state.startOffset = juce::jlimit(0.0, (state.originalSample->audio.getNumSamples() - 2) / state.originalSample->rate, value);
    transformPending = true; ++transformVersion; loading.store(true); notify();
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}
bool MiniSamplerAudioProcessor::matchBpm(double original)
{
    const juce::ScopedLock lock(stateLock);
    if (!state.originalSample || !validBpm(original)) return false;
    state.originalBpm = original; state.targetBpm = projectTempo.load(); state.stretchApplied = true;
    transformPending = true; ++transformVersion; loading.store(true); notify();
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true)); return true;
}
void MiniSamplerAudioProcessor::setEditorSize(int width, int height) { const juce::ScopedLock lock(stateLock); state.width = width; state.height = height; }
void MiniSamplerAudioProcessor::getStateInformation(juce::MemoryBlock& block)
{
    const juce::ScopedLock lock(stateLock); juce::XmlElement xml("MiniSamplerLoopX");
    xml.setAttribute("version", 3); xml.setAttribute("file", state.originalSample ? state.originalSample->file.getFullPathName() : pendingFile.getFullPathName());
    const auto writeLoop = [](juce::XmlElement& x, const Loop& loop)
    { x.setAttribute("start", loop.start); x.setAttribute("end", loop.end); x.setAttribute("beats", loop.beats); x.setAttribute("fadeIn", loop.fadeIn); x.setAttribute("fadeOut", loop.fadeOut); };
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
void MiniSamplerAudioProcessor::setStateInformation(const void* data, int size)
{
    const auto xml = getXmlFromBinary(data, size); if (!xml || !xml->hasTagName("MiniSamplerLoopX")) return;
    const juce::ScopedLock lock(stateLock);
    const auto readLoop = [](const juce::XmlElement& x) { return Loop{sane(x.getDoubleAttribute("start")), sane(x.getDoubleAttribute("end")),
        sane(x.getDoubleAttribute("beats")), sane(x.getDoubleAttribute("fadeIn", 0.004)), sane(x.getDoubleAttribute("fadeOut", 0.004))}; };
    state.loop = readLoop(*xml); state.slots.clear();
    for (const auto* child : xml->getChildIterator()) if (child->hasTagName("Slot") && state.slots.size() < 10) state.slots.push_back(readLoop(*child));
    setUiSettings(xml->getIntAttribute("grid", 3), xml->getIntAttribute("segments", 1), xml->getBoolAttribute("snap", true), xml->getBoolAttribute("triplet", false), xml->getBoolAttribute("zeroCross"));
    state.midiKeyTracking = xml->getBoolAttribute("midiKeyTracking", false); midiKeyTracking.store(state.midiKeyTracking);
    setDisplaySettings(xml->getBoolAttribute("brightGrid", false), xml->getBoolAttribute("stereoWaveform", false));
    setPlaybackSettings(xml->getIntAttribute("playbackMode", 0), xml->getIntAttribute("velocityMode", 0)); setTheme(xml->getIntAttribute("theme", 0));
    state.startOffset = sane(xml->getDoubleAttribute("startOffset"));
    state.originalBpm = xml->getDoubleAttribute("originalBpm", 120); if (!validBpm(state.originalBpm)) state.originalBpm = 120;
    state.targetBpm = xml->getDoubleAttribute("targetBpm", 120); if (!validBpm(state.targetBpm)) state.targetBpm = 120;
    state.stretchApplied = xml->getBoolAttribute("stretchApplied", false);
    projectTempo.store(state.targetBpm);
    state.width = juce::jlimit(900, 1800, xml->getIntAttribute("width", 1000)); state.height = juce::jlimit(260, 1100, xml->getIntAttribute("height", 390));
    selectSlot(xml->getIntAttribute("slot", 0));
    positionParameter->setValueNotifyingHost(positionParameter->convertTo0to1(float(juce::jlimit(0.0, 1.0, sane(xml->getDoubleAttribute("loopPosition"))))));
    livePosition.store(xml->getDoubleAttribute("positionOverride", -1));
    loopEnabled.store(xml->getBoolAttribute("enabled")); restoredCoordinates = true;
    const juce::File file(xml->getStringAttribute("file")); if (file.existsAsFile()) requestSampleLoad(file, true); else state.status = "Sample missing - use Load audio file in Settings";
}
juce::AudioProcessorEditor* MiniSamplerAudioProcessor::createEditor() { return new MiniSamplerAudioProcessorEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MiniSamplerAudioProcessor(); }
