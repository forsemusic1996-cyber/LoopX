#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <limits>

MiniSamplerAudioProcessor::MiniSamplerAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      Thread("MiniSampler loader")
{
    state.status = "Drop audio here or click Load";
    startThread();
}
MiniSamplerAudioProcessor::~MiniSamplerAudioProcessor()
{
    signalThreadShouldExit(); notify(); stopThread(-1);
}
void MiniSamplerAudioProcessor::prepareToPlay(double rate, int)
{
    outputRate = juce::jmax(1.0, rate);
    fallbackBeat = 0; voices = {}; lastAudioSample = nullptr; loopMidiNote = 60;
}

bool MiniSamplerAudioProcessor::isBusesLayoutSupported(const BusesLayout& layout) const
{
    return layout.getMainInputChannelSet().isDisabled()
        && (layout.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
            || layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo());
}
void MiniSamplerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    // No mutex, file I/O, allocation, sample destruction or GUI access here.
    audioReaders.fetch_add(1, std::memory_order_seq_cst);
    const auto* sample = audioSample.load(std::memory_order_seq_cst);
    struct ReaderGuard { std::atomic<unsigned>& readers; ~ReaderGuard() { readers.fetch_sub(1, std::memory_order_seq_cst); } } guard { audioReaders };
    if (sample != lastAudioSample) { voices = {}; lastAudioSample = sample; loopMidiNote = 60; }
    bool connected = false, playing = false;
    double beat = fallbackBeat;
    if (auto* transport = getPlayHead())
        if (auto position = transport->getPosition())
        {
            connected = true; playing = position->getIsPlaying();
            if (auto bpm = position->getBpm(); bpm.hasValue() && std::isfinite(*bpm) && *bpm > 0)
                projectTempo.store(*bpm, std::memory_order_relaxed);
            if (auto signature = position->getTimeSignature())
            {
                numerator.store(juce::jmax(1, signature->numerator));
                denominator.store(juce::jmax(1, signature->denominator));
            }
            if (auto ppq = position->getPpqPosition(); ppq.hasValue() && std::isfinite(*ppq)) beat = *ppq;
            else if (auto seconds = position->getTimeInSeconds(); seconds.hasValue() && std::isfinite(*seconds))
                beat = *seconds * projectTempo.load() / 60.0;
        }
    hostConnected.store(connected); hostPlaying.store(playing);
    const double beatStep = projectTempo.load() / (60.0 * outputRate);
    if (!connected || playing) fallbackBeat = beat + buffer.getNumSamples() * beatStep;
    if (!sample || sample->audio.getNumSamples() < 2) { playbackSeconds.store(-1.0); return; }
    const auto sequence = loopSequence.load(std::memory_order_seq_cst);
    if ((sequence & 1u) == 0)
    {
        const Loop candidate { loopStart.load(), loopEnd.load(), loopBeats.load() };
        if (loopSequence.load(std::memory_order_seq_cst) == sequence) audioLoop = candidate;
    }
    const double start = juce::jlimit(0.0, sample->duration(), audioLoop.start) * sample->rate;
    const double end = juce::jlimit(0.0, sample->duration(), audioLoop.end) * sample->rate;
    const bool looping = loopEnabled.load() && end > start + 1.0 && audioLoop.beats > 0.0;
    if (looping && connected && !playing) { playbackSeconds.store(-1.0); return; }
    auto event = midi.cbegin();
    const auto eventEnd = midi.cend();
    double cursor = -1.0;
    const bool keyTracking = midiKeyTracking.load();
    double noteRatio = keyTracking ? std::pow(2.0, (loopMidiNote - 60) / 12.0) : 1.0;
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
            while (event != eventEnd && (*event).samplePosition <= i)
            {
                const auto message = (*event).getMessage();
                if (message.isNoteOn())
                {
                    loopMidiNote = message.getNoteNumber();
                    noteRatio = keyTracking ? std::pow(2.0, (loopMidiNote - 60) / 12.0) : 1.0;
                    if (!looping)
                    {
                    auto* voice = &voices.front();
                    for (auto& v : voices) if (v.note < 0) { voice = &v; break; }
                    *voice = { 0.0, sample->rate / outputRate * std::pow(2.0, (message.getNoteNumber() - 60) / 12.0),
                               message.getFloatVelocity(), message.getNoteNumber() };
                    }
                }
                if (message.isNoteOff()) for (auto& v : voices) if (v.note == message.getNoteNumber()) v.note = -1;
                if (message.isAllNotesOff() || message.isAllSoundOff()) { voices = {}; loopMidiNote = 60; noteRatio = 1.0; }
                ++event;
            }
        if (looping)
        {
            const auto position = start + LoopMath::phase((beat + i * beatStep) * noteRatio, audioLoop.beats) * (end - start);
            const int index = juce::jlimit(0, sample->audio.getNumSamples() - 1, static_cast<int>(position));
            const int next = index + 1 < static_cast<int>(std::ceil(end)) ? juce::jmin(index + 1, sample->audio.getNumSamples() - 1)
                                                                       : juce::jlimit(0, sample->audio.getNumSamples() - 1, static_cast<int>(start));
            const auto fraction = static_cast<float>(position - index);
            const double fadeSamples = juce::jmin(64.0, (end - start) * 0.05);
            const float gain = static_cast<float>(juce::jlimit(0.0, 1.0, juce::jmin(position - start, end - position) / juce::jmax(1.0, fadeSamples)));
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto* source = sample->audio.getReadPointer(juce::jmin(ch, sample->audio.getNumChannels() - 1));
                buffer.setSample(ch, i, (source[index] + fraction * (source[next] - source[index])) * gain);
            }
            cursor = position / sample->rate;
        }
        else
            for (auto& voice : voices)
                if (voice.note >= 0)
                {
                    const int index = static_cast<int>(voice.position);
                    if (index >= sample->audio.getNumSamples() - 1) { voice.note = -1; continue; }
                    const auto fraction = static_cast<float>(voice.position - index);
                    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    {
                        const auto* source = sample->audio.getReadPointer(juce::jmin(ch, sample->audio.getNumChannels() - 1));
                        buffer.addSample(ch, i, (source[index] + fraction * (source[index + 1] - source[index])) * voice.gain);
                    }
                    cursor = voice.position / sample->rate; voice.position += keyTracking ? voice.step : sample->rate / outputRate;
                }
    }
    playbackSeconds.store(cursor, std::memory_order_relaxed);
}
void MiniSamplerAudioProcessor::requestSampleLoad(const juce::File& file, bool restoring)
{
    if (!file.existsAsFile()) return;
    const juce::ScopedLock lock(stateLock);
    pendingFile = file; pendingRestore = restoring; ++requestVersion;
    loading.store(true); state.status = "Loading " + file.getFileName() + "..."; notify();
}
void MiniSamplerAudioProcessor::run()
{
    uint64_t handled = 0;
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    while (!threadShouldExit())
    {
        juce::File file; bool restoring = false; uint64_t version;
        {
            const juce::ScopedLock lock(stateLock);
            version = requestVersion;
            if (version != handled) { file = pendingFile; restoring = pendingRestore; }
        }
        if (version != handled)
        {
            std::shared_ptr<MiniSamplerSample> sample;
            juce::String error = "Cannot decode " + file.getFileName();
            try
            {
                std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
                if (reader && reader->lengthInSamples > 1 && reader->lengthInSamples <= std::numeric_limits<int>::max()
                    && reader->sampleRate > 0 && reader->numChannels > 0)
                {
                    sample = std::make_shared<MiniSamplerSample>(); sample->rate = reader->sampleRate; sample->file = file;
                    sample->audio.setSize(juce::jmin(2, static_cast<int>(reader->numChannels)), static_cast<int>(reader->lengthInSamples));
                    if (!reader->read(&sample->audio, 0, sample->audio.getNumSamples(), 0, true, true)) sample.reset();
                    if (sample) sample->buildWaveform();
                }
            }
            catch (const std::exception&) { sample.reset(); error = "Not enough memory to load " + file.getFileName(); }
            {
                const juce::ScopedLock lock(stateLock);
                if (version == requestVersion && !threadShouldExit())
                {
                    if (sample)
                    {
                        if (state.sample) retired.push_back(state.sample);
                        state.sample = sample; audioSample.store(sample.get(), std::memory_order_seq_cst);
                        if (!restoring) { state.slots.clear(); loopEnabled.store(false); state.loop = {}; }
                        else
                        {
                            state.loop.start = juce::jlimit(0.0, sample->duration(), state.loop.start);
                            state.loop.end = juce::jlimit(state.loop.start, sample->duration(), state.loop.end);
                        }
                        publishLoop(state.loop); state.status = file.getFileName();
                    }
                    else state.status = error;
                    loading.store(false);
                }
            }
            handled = version;
        }
        if (audioReaders.load(std::memory_order_seq_cst) == 0) retired.clear();
        wait(50);
    }
}
MiniSamplerAudioProcessor::ViewState MiniSamplerAudioProcessor::getViewState() const
{
    const juce::ScopedLock lock(stateLock); return state;
}
void MiniSamplerAudioProcessor::publishLoop(const Loop& loop)
{
    loopSequence.fetch_add(1, std::memory_order_seq_cst);
    loopStart.store(loop.start); loopEnd.store(loop.end); loopBeats.store(loop.beats);
    loopSequence.fetch_add(1, std::memory_order_seq_cst);
}
void MiniSamplerAudioProcessor::setLoopSelection(double start, double end, double beats)
{
    const juce::ScopedLock lock(stateLock);
    if (!state.sample) return;
    start = juce::jlimit(0.0, state.sample->duration(), start); end = juce::jlimit(start, state.sample->duration(), end);
    if (end - start < 0.001) return;
    state.loop = { start, end, beats > 0.0 ? beats : (end - start) * projectTempo.load() / 60.0 };
    publishLoop(state.loop); loopEnabled.store(true);
}
void MiniSamplerAudioProcessor::stopLoop() { loopEnabled.store(false); }
void MiniSamplerAudioProcessor::saveSlot()
{
    const juce::ScopedLock lock(stateLock);
    if (state.loop.end > state.loop.start && state.slots.size() < 10) state.slots.push_back(state.loop);
}
void MiniSamplerAudioProcessor::recallSlot(int index)
{
    const juce::ScopedLock lock(stateLock);
    if (juce::isPositiveAndBelow(index, static_cast<int>(state.slots.size())))
    {
        const auto slot = state.slots[static_cast<size_t>(index)]; setLoopSelection(slot.start, slot.end, slot.beats);
    }
}
void MiniSamplerAudioProcessor::deleteSlot(int index)
{
    const juce::ScopedLock lock(stateLock);
    if (juce::isPositiveAndBelow(index, static_cast<int>(state.slots.size()))) state.slots.erase(state.slots.begin() + index);
}
void MiniSamplerAudioProcessor::setUiSettings(int grid, int segments, bool snap, bool triplet, bool zeroCross)
{
    {
        const juce::ScopedLock lock(stateLock);
        state.grid = juce::jlimit(1, 6, grid); state.segments = juce::jlimit(1, 5, segments);
        state.snap = snap; state.triplet = triplet; state.zeroCross = zeroCross;
    }
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}
void MiniSamplerAudioProcessor::setDisplaySettings(bool brightGrid, bool stereoWaveform)
{
    {
        const juce::ScopedLock lock(stateLock);
        state.brightGrid = brightGrid; state.stereoWaveform = stereoWaveform;
    }
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}
void MiniSamplerAudioProcessor::setEditorSize(int width, int height)
{
    const juce::ScopedLock lock(stateLock); state.width = width; state.height = height;
}
void MiniSamplerAudioProcessor::getStateInformation(juce::MemoryBlock& block)
{
    const juce::ScopedLock lock(stateLock);
    juce::XmlElement xml("MiniSamplerLoopX");
    xml.setAttribute("version", 2);
    xml.setAttribute("file", state.sample ? state.sample->file.getFullPathName() : pendingFile.getFullPathName());
    xml.setAttribute("start", state.loop.start); xml.setAttribute("end", state.loop.end); xml.setAttribute("beats", state.loop.beats);
    xml.setAttribute("enabled", loopEnabled.load()); xml.setAttribute("grid", state.grid); xml.setAttribute("segments", state.segments);
    xml.setAttribute("snap", state.snap); xml.setAttribute("triplet", state.triplet); xml.setAttribute("zeroCross", state.zeroCross);
    xml.setAttribute("midiKeyTracking", state.midiKeyTracking);
    xml.setAttribute("brightGrid", state.brightGrid); xml.setAttribute("stereoWaveform", state.stereoWaveform);
    xml.setAttribute("width", state.width); xml.setAttribute("height", state.height);
    for (const auto& slot : state.slots)
    {
        auto* child = xml.createNewChildElement("Slot");
        child->setAttribute("start", slot.start); child->setAttribute("end", slot.end); child->setAttribute("beats", slot.beats);
    }
    copyXmlToBinary(xml, block);
}
void MiniSamplerAudioProcessor::setStateInformation(const void* data, int size)
{
    const auto xml = getXmlFromBinary(data, size);
    if (!xml || !xml->hasTagName("MiniSamplerLoopX")) return;
    const juce::ScopedLock lock(stateLock);
    const auto finite = [](double v) { return std::isfinite(v) && v >= 0 ? v : 0.0; };
    state.loop = { finite(xml->getDoubleAttribute("start")), finite(xml->getDoubleAttribute("end")), finite(xml->getDoubleAttribute("beats")) };
    state.slots.clear();
    for (const auto* child : xml->getChildIterator())
        if (child->hasTagName("Slot") && state.slots.size() < 10)
            state.slots.push_back({ finite(child->getDoubleAttribute("start")), finite(child->getDoubleAttribute("end")), finite(child->getDoubleAttribute("beats")) });
    state.grid = juce::jlimit(1, 6, xml->getIntAttribute("grid", 3)); state.segments = juce::jlimit(1, 5, xml->getIntAttribute("segments", 1));
    state.snap = xml->getBoolAttribute("snap", true); state.triplet = xml->getBoolAttribute("triplet"); state.zeroCross = xml->getBoolAttribute("zeroCross");
    state.midiKeyTracking = xml->getBoolAttribute("midiKeyTracking", false); midiKeyTracking.store(state.midiKeyTracking);
    state.brightGrid = xml->getBoolAttribute("brightGrid", true); state.stereoWaveform = xml->getBoolAttribute("stereoWaveform", false);
    state.width = juce::jlimit(720, 1800, xml->getIntAttribute("width", 1000)); state.height = juce::jlimit(260, 1100, xml->getIntAttribute("height", 390));
    publishLoop(state.loop); loopEnabled.store(xml->getBoolAttribute("enabled"));
    const juce::File file(xml->getStringAttribute("file"));
    if (file.existsAsFile()) requestSampleLoad(file, true);
    else state.status = "Sample file missing - click Load to relink";
}
juce::AudioProcessorEditor* MiniSamplerAudioProcessor::createEditor() { return new MiniSamplerAudioProcessorEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MiniSamplerAudioProcessor(); }
