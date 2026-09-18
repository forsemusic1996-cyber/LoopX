#include "PluginEditor.h"

namespace
{
bool audioFile(const juce::File& file)
{
    return file.existsAsFile() && file.hasFileExtension("wav;aif;aiff;flac;mp3;ogg");
}
juce::File fileFromText(juce::String text)
{
    text = text.trim().unquoted();
    if (text.startsWithIgnoreCase("file://"))
    {
        text = juce::URL::removeEscapeChars(text.substring(7));
        if (text.startsWith("/") && text.length() > 2 && text[2] == ':') text = text.substring(1);
    }
    return juce::File::isAbsolutePath(text) ? juce::File(text) : juce::File{};
}
constexpr int load = 1, set = 2, add = 3, length = 4, twice = 5, half = 6,
              segments = 7, snap = 8, grid = 9, zc = 10, settings = 11, play = 12;
const juce::StringArray divisions { "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32" };
}

MiniSamplerWaveformView::MiniSamplerWaveformView(MiniSamplerAudioProcessor& p) : processor(p)
{
    setOpaque(true); refresh(); startTimerHz(30);
}
MiniSamplerWaveformView::~MiniSamplerWaveformView() { stopTimer(); }
double MiniSamplerWaveformView::duration() const { return state.sample ? state.sample->duration() : 0.0; }
double MiniSamplerWaveformView::visibleLength() const { return duration() / zoom; }
juce::Rectangle<float> MiniSamplerWaveformView::waveArea() const
{
    return getLocalBounds().toFloat().withTrimmedTop(20.0f).withTrimmedBottom(55.0f).reduced(4.0f, 0.0f);
}
float MiniSamplerWaveformView::xForTime(double time) const
{
    const auto area = waveArea();
    return area.getX() + static_cast<float>((time - viewStart) / juce::jmax(1.0e-9, visibleLength())) * area.getWidth();
}
double MiniSamplerWaveformView::timeForX(float x) const
{
    const auto area = waveArea();
    return juce::jlimit(0.0, duration(), viewStart + (x - area.getX()) / juce::jmax(1.0f, area.getWidth()) * visibleLength());
}
double MiniSamplerWaveformView::gridSeconds() const
{
    auto beats = LoopMath::divisionBeats(state.grid, processor.getProjectTimeSignatureNumerator(), processor.getProjectTimeSignatureDenominator());
    if (state.triplet) beats *= 2.0 / 3.0;
    return beats * 60.0 / processor.getProjectTempo();
}
double MiniSamplerWaveformView::snapTime(double time) const
{
    if (state.snap) return juce::jlimit(0.0, duration(), std::round(time / gridSeconds()) * gridSeconds());
    if (!state.zeroCross || !state.sample) return juce::jlimit(0.0, duration(), time);
    const auto& sample = *state.sample;
    const int count = sample.audio.getNumSamples();
    const int target = juce::jlimit(1, count - 1, static_cast<int>(time * sample.rate));
    const int radius = static_cast<int>(sample.rate * 0.005);
    int nearest = target, distance = radius + 1;
    const auto* samples = sample.audio.getReadPointer(0);
    for (int i = juce::jmax(1, target - radius); i < juce::jmin(count, target + radius + 1); ++i)
        if ((samples[i - 1] <= 0 && samples[i] >= 0) || (samples[i - 1] >= 0 && samples[i] <= 0))
            if (std::abs(i - target) < distance) { nearest = i; distance = std::abs(i - target); }
    return nearest / sample.rate;
}
bool MiniSamplerWaveformView::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& name : files) if (audioFile(fileFromText(name))) return true;
    return false;
}
void MiniSamplerWaveformView::filesDropped(const juce::StringArray& files, int, int)
{
    dragOver = false;
    for (const auto& name : files)
        if (const auto file = fileFromText(name); audioFile(file)) { processor.requestSampleLoad(file); break; }
    repaint();
}
void MiniSamplerWaveformView::fileDragEnter(const juce::StringArray&, int, int) { dragOver = true; repaint(); }
void MiniSamplerWaveformView::fileDragExit(const juce::StringArray&) { dragOver = false; repaint(); }
bool MiniSamplerWaveformView::isInterestedInTextDrag(const juce::String& text) { return audioFile(fileFromText(text)); }
void MiniSamplerWaveformView::textDropped(const juce::String& text, int, int)
{
    dragOver = false;
    const auto file = fileFromText(text); if (audioFile(file)) processor.requestSampleLoad(file);
    repaint();
}
void MiniSamplerWaveformView::refresh()
{
    auto next = processor.getViewState();
    const bool changed = next.sample != state.sample;
    bool overlaysChanged = next.loop.start != state.loop.start || next.loop.end != state.loop.end
        || next.grid != state.grid || next.segments != state.segments || next.snap != state.snap
        || next.triplet != state.triplet || next.zeroCross != state.zeroCross || next.slots.size() != state.slots.size();
    if (!overlaysChanged)
        for (size_t i = 0; i < next.slots.size(); ++i)
            if (next.slots[i].start != state.slots[i].start || next.slots[i].end != state.slots[i].end)
            { overlaysChanged = true; break; }
    state = std::move(next);
    if (changed) { dirtyCache = true; viewStart = 0; zoom = 1; pendingSelection = false; dragMode = 0; }
    if (!pendingSelection && dragMode == 0) { selectionStart = state.loop.start; selectionEnd = state.loop.end; }
    if (changed || overlaysChanged) repaint();
}
void MiniSamplerWaveformView::resized()
{
    // Live resize scales the previous image. Peak geometry is rebuilt after it settles.
    dirtyCache = true; lastResize = juce::Time::getMillisecondCounterHiRes();
}
void MiniSamplerWaveformView::rebuildWaveCache()
{
    dirtyCache = false;
    if (!state.sample || getWidth() < 2 || getHeight() < 60) { waveCache = {}; return; }
    const float density = juce::jlimit(1.0f, 3.0f, getDesktopScaleFactor() * juce::Component::getApproximateScaleFactorForComponent(this));
    const int width = juce::jlimit(1, 5400, static_cast<int>(std::ceil(waveArea().getWidth() * density)));
    const int height = juce::jlimit(1, 3000, static_cast<int>(std::ceil(waveArea().getHeight() * density)));
    waveCache = juce::Image(juce::Image::ARGB, width, juce::jmax(1, height), true);
    juce::Graphics g(waveCache);
    const int channels = stereo ? state.sample->audio.getNumChannels() : 1;
    for (int ch = 0; ch < channels; ++ch)
    {
        const float band = static_cast<float>(height) / channels;
        const float centre = band * (ch + 0.5f);
        const float scale = band * 0.46f;
        g.setColour(ch == 0 ? juce::Colour(0xff648b93) : juce::Colour(0xff93a9b3));
        const double firstSample = viewStart * state.sample->rate;
        const double samplesPerPixel = visibleLength() * state.sample->rate / width;
        if (samplesPerPixel < 1.0)
        {
            juce::Path path;
            const auto* data = state.sample->audio.getReadPointer(ch);
            const int count = state.sample->audio.getNumSamples();
            for (int x = 0; x < width; ++x)
            {
                const double position = juce::jlimit(0.0, static_cast<double>(count - 1), firstSample + x * samplesPerPixel);
                const int a = static_cast<int>(position), b = juce::jmin(a + 1, count - 1);
                const float value = data[a] + static_cast<float>(position - a) * (data[b] - data[a]);
                if (x == 0) path.startNewSubPath(0, centre - value * scale);
                else path.lineTo(static_cast<float>(x), centre - value * scale);
            }
            g.strokePath(path, juce::PathStrokeType(density));
            continue;
        }
        for (int x = 0; x < width; ++x)
        {
            const int a = static_cast<int>(firstSample + x * samplesPerPixel);
            const int b = static_cast<int>(std::ceil(firstSample + (x + 1) * samplesPerPixel));
            const auto range = state.sample->waveformRange(ch, a, b);
            g.drawVerticalLine(x, centre - range.second * scale, centre - range.first * scale + density * 0.5f);
        }
    }
    repaint();
}
void MiniSamplerWaveformView::timerCallback()
{
    const auto oldLoop = state.loop;
    const auto oldGrid = state.grid; const auto oldSegments = state.segments;
    refresh();
    if (dirtyCache && juce::Time::getMillisecondCounterHiRes() - lastResize > 90.0) rebuildWaveCache();
    if (state.loop.start != oldLoop.start || state.loop.end != oldLoop.end || state.grid != oldGrid || state.segments != oldSegments) repaint();
    const double nextCursor = processor.getPlaybackSeconds();
    if (nextCursor != cursor)
    {
        if (cursor >= 0) repaint(static_cast<int>(xForTime(cursor)) - 4, 0, 9, getHeight());
        cursor = nextCursor;
        if (cursor >= 0) repaint(static_cast<int>(xForTime(cursor)) - 4, 0, 9, getHeight());
    }
}
void MiniSamplerWaveformView::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff101315));
    const auto area = waveArea();
    g.setColour(juce::Colour(0xff21252a)); g.fillRect(0, 0, getWidth(), 23);
    if (!state.sample)
    {
        g.setColour(juce::Colour(0xffa6b1bc)); g.setFont(14.0f);
        g.drawText(dragOver ? "Release to load sample" : "Drop an audio file here or click Load", area, juce::Justification::centred);
        if (dragOver) { g.setColour(juce::Colour(0xff66ddc9)); g.drawRect(getLocalBounds(), 3); }
        return;
    }
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    if (waveCache.isValid()) g.drawImage(waveCache, area);
    const double beatSeconds = 60.0 / processor.getProjectTempo();
    const double barSeconds = beatSeconds * LoopMath::divisionBeats(1, processor.getProjectTimeSignatureNumerator(), processor.getProjectTimeSignatureDenominator());
    const double gridStep = gridSeconds();
    const double rawPixels = gridStep / juce::jmax(1.0e-9, visibleLength()) * area.getWidth();
    const int stride = juce::jmax(1, static_cast<int>(std::ceil(8.0 / juce::jmax(0.0001, rawPixels))));
    const double step = gridStep * stride;
    for (double t = std::floor(viewStart / step) * step; t <= viewStart + visibleLength() + step * 0.01; t += step)
    {
        const float x = xForTime(t);
        if (x < area.getX() || x > area.getRight()) continue;
        const bool bar = std::abs(t / barSeconds - std::round(t / barSeconds)) < 0.00001;
        g.setColour(bar ? juce::Colour(brightGrid ? 0xff519b9d : 0xff375b5e)
                        : juce::Colour(brightGrid ? 0xff343f42 : 0xff252d30));
        g.drawVerticalLine(static_cast<int>(x), area.getY(), area.getBottom());
        if (bar) { g.setFont(11.0f); g.drawText(juce::String(static_cast<int>(std::round(t / barSeconds)) + 1), static_cast<int>(x) + 3, 2, 48, 18, juce::Justification::centredLeft); }
    }
    if (state.segments > 1)
    {
        const double segment = barSeconds / std::pow(2.0, state.segments - 2);
        const int segmentStride = juce::jmax(1, static_cast<int>(std::ceil(8.0 / juce::jmax(0.0001, segment / visibleLength() * area.getWidth()))));
        const double drawStep = segment * segmentStride;
        for (double t = std::floor(viewStart / drawStep) * drawStep; t <= viewStart + visibleLength(); t += drawStep)
        {
            g.setColour(juce::Colour(0xff56999c));
            g.drawLine(xForTime(t), area.getY(), xForTime(t), area.getBottom(), 1.5f);
        }
    }
    const float startX = xForTime(state.loop.start), endX = xForTime(state.loop.end);
    if (state.loop.end > state.loop.start)
    {
        const auto selection = juce::Rectangle<float>(startX, area.getY(), endX - startX, area.getHeight()).getIntersection(area);
        g.setColour(juce::Colour(0x354cb8b7)); g.fillRect(selection);
        g.setColour(juce::Colour(0xff66cecd));
        for (const float x : { startX, endX })
            if (x >= area.getX() && x <= area.getRight()) g.drawLine(x, area.getY(), x, area.getBottom(), 1.0f);
        juce::Path flags;
        if (startX >= area.getX() && startX <= area.getRight()) flags.addTriangle(startX, area.getY(), startX + 10, area.getY(), startX, area.getY() + 10);
        if (endX >= area.getX() && endX <= area.getRight()) flags.addTriangle(endX, area.getY(), endX - 10, area.getY(), endX, area.getY() + 10);
        g.fillPath(flags);
        const auto loopBar = juce::Rectangle<float>(startX, getHeight() - 27.0f, endX - startX, 10.0f).getIntersection(getLocalBounds().toFloat());
        g.setColour(juce::Colour(0xff426f74)); g.fillRoundedRectangle(loopBar, 2.0f);
        g.setColour(juce::Colour(0xff66cecd)); g.drawRoundedRectangle(loopBar, 2.0f, 1.0f);
    }
    if (pendingSelection && selectionEnd != selectionStart)
    {
        const float a = xForTime(juce::jmin(selectionStart, selectionEnd)), b = xForTime(juce::jmax(selectionStart, selectionEnd));
        g.setColour(juce::Colour(0x60699fef));
        g.fillRect(juce::Rectangle<float>(a, area.getY(), b - a, area.getHeight()).getIntersection(area));
    }
    for (size_t i = 0; i < state.slots.size(); ++i)
    {
        const auto& slot = state.slots[i];
        const float a = xForTime(slot.start), b = xForTime(slot.end);
        const auto line = juce::Rectangle<float>(a, getHeight() - 35.0f, b - a, 3.0f).getIntersection(getLocalBounds().toFloat());
        g.setColour(juce::Colour(0xffcfbb80)); g.fillRect(line);
        if (line.getWidth() > 0)
        {
            g.setColour(juce::Colour(0xfff0dfa8)); g.setFont(16.0f);
            g.drawText(juce::String(i == 9 ? 0 : static_cast<int>(i) + 1), static_cast<int>(line.getX()) + 2, getHeight() - 54, 24, 18, juce::Justification::centredLeft);
        }
    }
    if (cursor >= viewStart && cursor <= viewStart + visibleLength())
    {
        g.setColour(juce::Colour(0xffffdf5d)); g.drawLine(xForTime(cursor), 0, xForTime(cursor), area.getBottom(), 2.0f);
    }
    const auto track = juce::Rectangle<float>(4.0f, getHeight() - 11.0f, getWidth() - 8.0f, 7.0f);
    const float thumbWidth = juce::jmax(18.0f, track.getWidth() / static_cast<float>(zoom));
    const float thumbX = track.getX() + static_cast<float>(viewStart / juce::jmax(1.0e-9, duration() - visibleLength())) * (track.getWidth() - thumbWidth);
    g.setColour(juce::Colour(0xff303741)); g.fillRoundedRectangle(track, 3.0f);
    g.setColour(juce::Colour(dragMode == 6 ? 0xffaac0d4 : 0xff64798c));
    g.fillRoundedRectangle({ thumbX, track.getY(), thumbWidth, track.getHeight() }, 3.0f);
    if (dragOver)
    {
        g.setColour(juce::Colour(0x503dc6b3)); g.fillRect(area);
        g.setColour(juce::Colour(0xff66ddc9)); g.drawRect(getLocalBounds(), 3);
        g.setColour(juce::Colours::white); g.setFont(18.0f);
        g.drawText("Release to load sample", area, juce::Justification::centred);
    }
}
void MiniSamplerWaveformView::commit(double start, double end, double beats)
{
    processor.setLoopSelection(start, end, beats); pendingSelection = false; refresh();
    selectionStart = state.loop.start; selectionEnd = state.loop.end;
    repaint(); if (onChanged) onChanged();
}
void MiniSamplerWaveformView::applySelection()
{
    if (selectionEnd > selectionStart) commit(selectionStart, selectionEnd);
}
void MiniSamplerWaveformView::setMusicalLength(double beats)
{
    refresh();
    const auto start = pendingSelection ? juce::jmin(selectionStart, selectionEnd) : state.loop.start;
    const auto end = juce::jmin(duration(), start + beats * 60.0 / processor.getProjectTempo());
    if (end > start) commit(start, end, (end - start) * processor.getProjectTempo() / 60.0);
}
void MiniSamplerWaveformView::scaleLength(double factor)
{
    refresh(); if (state.loop.end <= state.loop.start) return;
    const auto end = LoopMath::scaledEnd(state.loop.start, state.loop.end, duration(), factor);
    const auto beats = state.loop.beats * (end - state.loop.start) / (state.loop.end - state.loop.start);
    commit(state.loop.start, end, beats);
}
void MiniSamplerWaveformView::moveLoop(double seconds)
{
    refresh(); const double len = state.loop.end - state.loop.start;
    if (len <= 0) return;
    const auto start = juce::jlimit(0.0, juce::jmax(0.0, duration() - len), state.loop.start + seconds);
    commit(start, start + len, state.loop.beats);
}
void MiniSamplerWaveformView::mouseDown(const juce::MouseEvent& event)
{
    refresh(); if (!state.sample) return;
    if (event.mods.isRightButtonDown())
    {
        juce::PopupMenu menu; menu.addItem(1, "SET selection as loop", selectionEnd > selectionStart); menu.addItem(2, "Save loop to slot", state.loop.end > state.loop.start && state.slots.size() < 10);
        juce::Component::SafePointer<MiniSamplerWaveformView> safe(this);
        menu.showMenuAsync(miniSamplerMenuOptions(*getParentComponent(), event.getScreenPosition()), [safe](int result)
        { if (safe) { if (result == 1) safe->applySelection(); if (result == 2) { safe->processor.saveSlot(); if (safe->onChanged) safe->onChanged(); safe->refresh(); safe->repaint(); } } });
        return;
    }
    dragStart = event.x; dragLoopStart = state.loop.start; dragLoopEnd = state.loop.end; dragViewStart = viewStart;
    if (event.mods.isMiddleButtonDown()) { dragMode = 5; return; }
    if (!event.mods.isLeftButtonDown()) return;
    if (event.y >= getHeight() - 14)
    {
        const double trackWidth = juce::jmax(1, getWidth() - 8);
        const double thumbWidth = juce::jmax(18.0, trackWidth / zoom);
        const double travel = juce::jmax(1.0, trackWidth - thumbWidth);
        const double thumbX = 4.0 + viewStart / juce::jmax(1.0e-9, duration() - visibleLength()) * travel;
        if (event.x < thumbX || event.x > thumbX + thumbWidth)
        {
            viewStart = juce::jlimit(0.0, juce::jmax(0.0, duration() - visibleLength()), (event.x - 4.0 - thumbWidth * 0.5) / travel * (duration() - visibleLength()));
            dirtyCache = true;
        }
        dragViewStart = viewStart; dragMode = 6; repaint(); return;
    }
    const bool loopValid = state.loop.end > state.loop.start;
    if (event.y >= getHeight() - 31 && loopValid && event.x >= xForTime(state.loop.start) && event.x <= xForTime(state.loop.end)) dragMode = 4;
    else if (loopValid && std::abs(event.x - xForTime(state.loop.start)) <= 6) dragMode = 2;
    else if (loopValid && std::abs(event.x - xForTime(state.loop.end)) <= 6) dragMode = 3;
    else
    {
        dragMode = 1; pendingSelection = true; selectionStart = snapTime(timeForX(static_cast<float>(event.x))); selectionEnd = selectionStart;
    }
    repaint();
}
void MiniSamplerWaveformView::mouseDrag(const juce::MouseEvent& event)
{
    if (dragMode == 1) selectionEnd = snapTime(timeForX(static_cast<float>(event.x)));
    else if (dragMode == 2 || dragMode == 3)
    {
        const double delta = (event.x - dragStart) / juce::jmax(1.0f, waveArea().getWidth()) * visibleLength() / (event.mods.isShiftDown() ? 10.0 : 1.0);
        const double start = dragMode == 2 ? juce::jlimit(0.0, juce::jmax(0.0, dragLoopEnd - 0.001), snapTime(dragLoopStart + delta)) : dragLoopStart;
        const double end = dragMode == 3 ? juce::jlimit(start + 0.001, duration(), snapTime(dragLoopEnd + delta)) : dragLoopEnd;
        commit(start, end);
    }
    else if (dragMode == 4)
    {
        const double len = dragLoopEnd - dragLoopStart;
        const double delta = (event.x - dragStart) / juce::jmax(1.0f, waveArea().getWidth()) * visibleLength() / (event.mods.isShiftDown() ? 10.0 : 1.0);
        const double start = juce::jlimit(0.0, juce::jmax(0.0, duration() - len), snapTime(dragLoopStart + delta));
        commit(start, start + len, state.loop.beats);
    }
    else if (dragMode == 5)
    {
        viewStart = juce::jlimit(0.0, juce::jmax(0.0, duration() - visibleLength()), dragViewStart - (event.x - dragStart) / juce::jmax(1.0f, waveArea().getWidth()) * visibleLength());
        dirtyCache = true;
    }
    else if (dragMode == 6)
    {
        const double trackWidth = juce::jmax(1, getWidth() - 8);
        const double travel = juce::jmax(1.0, trackWidth - juce::jmax(18.0, trackWidth / zoom));
        viewStart = juce::jlimit(0.0, juce::jmax(0.0, duration() - visibleLength()), dragViewStart + (event.x - dragStart) / travel * (duration() - visibleLength()));
        dirtyCache = true;
    }
    repaint();
}
void MiniSamplerWaveformView::mouseUp(const juce::MouseEvent& event)
{
    if (dragMode == 1)
    {
        if (selectionEnd < selectionStart) std::swap(selectionStart, selectionEnd);
        if (std::abs(event.x - dragStart) < 4 && state.segments > 1)
        {
            const auto bar = LoopMath::divisionBeats(1, processor.getProjectTimeSignatureNumerator(), processor.getProjectTimeSignatureDenominator());
            const double beats = bar / std::pow(2.0, state.segments - 2);
            const double seconds = beats * 60.0 / processor.getProjectTempo();
            const double start = std::floor(timeForX(static_cast<float>(event.x)) / seconds) * seconds;
            const double end = juce::jmin(duration(), start + seconds);
            commit(start, end);
        }
        else if (selectionEnd - selectionStart < 0.001) pendingSelection = false;
    }
    dragMode = 0; refresh(); repaint();
}
void MiniSamplerWaveformView::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (!state.sample) return;
    const double anchor = timeForX(static_cast<float>(event.x));
    const double fraction = juce::jlimit(0.0, 1.0, static_cast<double>((event.x - waveArea().getX()) / juce::jmax(1.0f, waveArea().getWidth())));
    if (event.mods.isShiftDown()) viewStart += wheel.deltaY * visibleLength() * 0.4;
    else
    {
        const double maximumZoom = juce::jmax(128.0, duration() * state.sample->rate / juce::jmax(1.0f, waveArea().getWidth()) * 8.0);
        zoom = juce::jlimit(1.0, maximumZoom, zoom * (wheel.deltaY > 0 ? 1.25 : 0.8));
        viewStart = anchor - fraction * visibleLength();
    }
    viewStart = juce::jlimit(0.0, juce::jmax(0.0, duration() - visibleLength()), viewStart); dirtyCache = true; repaint();
}
void MiniSamplerWaveformView::resetZoom() { zoom = 1.0; viewStart = 0.0; dirtyCache = true; repaint(); }

MiniSamplerAudioProcessorEditor::MiniSamplerAudioProcessorEditor(MiniSamplerAudioProcessor& p) : AudioProcessorEditor(&p), processor(p), waveform(p)
{
    state = processor.getViewState();
    setOpaque(true); setWantsKeyboardFocus(true); setResizable(true, true); setResizeLimits(800, 260, 1800, 1100);
    addAndMakeVisible(waveform);
    waveform.onChanged = [this] { state = processor.getViewState(); layoutTools(); repaint(); };
    setSize(juce::jmax(800, state.width), state.height); startTimerHz(15);
}
MiniSamplerAudioProcessorEditor::~MiniSamplerAudioProcessorEditor() { stopTimer(); }
void MiniSamplerAudioProcessorEditor::layoutTools()
{
    tools.clear();
    const auto put = [this](int id, juce::String text, int x, int width, bool active = false)
    { tools.push_back({ id, std::move(text), { x, 5, width, 18 }, active }); };
    put(load, "Load", 6, 45); put(set, "SET", 55, 34);
    int x = 94;
    for (size_t i = 0; i < state.slots.size(); ++i) { put(100 + static_cast<int>(i), juce::String(i == 9 ? 0 : static_cast<int>(i) + 1), x, 16, true); x += 19; }
    put(add, "+", x, 22, state.slots.size() < 10);
    const int centre = getWidth() / 2 - 93;
    put(length, "LOOP LENGTH", centre, 112, true); put(twice, juce::String::charToString(0x00d7) + "2", centre + 116, 31); put(half, juce::String::charToString(0x00f7) + "2", centre + 151, 31);
    x = getWidth() - 286;
    put(segments, state.segments == 1 ? "Segments" : "Seg " + juce::StringArray({ "Off", "1/1", "1/2", "1/4", "1/8" })[state.segments - 1], x, 72, state.segments > 1); x += 76;
    put(snap, "Snap", x, 42, state.snap); x += 46;
    put(grid, divisions[state.grid - 1] + (state.triplet ? "T" : ""), x, 49); x += 53;
    put(zc, "ZC", x, 28, state.zeroCross); x += 32;
    put(play, processor.isLooping() ? "Stop" : "Loop", x, 40, processor.isLooping()); x += 44;
    put(settings, juce::String::charToString(0x2699), x, 27);
}
void MiniSamplerAudioProcessorEditor::resized()
{
    waveform.setBounds(4, 28, getWidth() - 8, juce::jmax(10, getHeight() - 54));
    layoutTools(); processor.setEditorSize(getWidth(), getHeight());
}
void MiniSamplerAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff202426)); g.setFont(12.0f);
    for (const auto& tool : tools)
    {
        g.setFont(tool.id >= 100 ? 15.0f : 12.0f);
        g.setColour(tool.active ? juce::Colour(0xff23474a) : juce::Colour(0xff292e30)); g.fillRoundedRectangle(tool.rect.toFloat(), 2.0f);
        g.setColour(tool.active ? juce::Colour(0xff66cecd) : juce::Colour(0xff657577)); g.drawRoundedRectangle(tool.rect.toFloat(), 2.0f, 1.0f);
        g.setColour(juce::Colour(0xffe1e7ed)); g.drawText(tool.text, tool.rect, juce::Justification::centred);
    }
    g.setColour(juce::Colour(0xffa8b4c2));
    g.setFont(11.0f);
    const int bottom = getHeight() - 23;
    g.drawText("Mini Sampler / LoopX v0.2  |  " + state.status, 10, bottom, getWidth() - 260, 19, juce::Justification::centredLeft);
    const auto tempo = processor.getProjectTempo();
    const auto host = processor.hasHostPosition() ? (processor.isHostPlaying() ? "PLAY" : "STOP") : "PREVIEW";
    g.drawText(juce::String(state.loop.beats, 2) + " beats  |  " + juce::String(tempo, 1) + " BPM  " + host,
               getWidth() - 250, bottom, 240, 19, juce::Justification::centredRight);
}
void MiniSamplerAudioProcessorEditor::timerCallback()
{
    const auto previous = state;
    state = processor.getViewState();
    if (previous.slots.size() != state.slots.size() || previous.loop.start != state.loop.start || previous.loop.end != state.loop.end
        || previous.grid != state.grid || previous.segments != state.segments || previous.snap != state.snap || previous.zeroCross != state.zeroCross
        || previous.triplet != state.triplet || previous.status != state.status || lastPlaying != processor.isLooping())
    { layoutTools(); repaint(0, 0, getWidth(), 38); repaint(0, getHeight() - 25, getWidth(), 25); }
    if (lastTempo != processor.getProjectTempo()) { waveform.repaint(); repaint(0, getHeight() - 25, getWidth(), 25); }
    lastTempo = processor.getProjectTempo(); lastPlaying = processor.isLooping();
}
void MiniSamplerAudioProcessorEditor::chooseFile()
{
    fileChooser = std::make_unique<juce::FileChooser>("Load sample", juce::File{}, "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    juce::Component::SafePointer<MiniSamplerAudioProcessorEditor> safe(this);
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& chooser) { if (safe && chooser.getResult().existsAsFile()) safe->processor.requestSampleLoad(chooser.getResult()); });
}
void MiniSamplerAudioProcessorEditor::mouseDown(const juce::MouseEvent& event)
{
    menuPosition = event.getScreenPosition();
    for (const auto& tool : tools) if (tool.rect.contains(event.getPosition())) { invoke(tool.id, event.mods.isRightButtonDown()); break; }
}
void MiniSamplerAudioProcessorEditor::invoke(int id, bool rightClick)
{
    state = processor.getViewState();
    if (id >= 100) { if (rightClick) processor.deleteSlot(id - 100); else processor.recallSlot(id - 100); }
    else if (id == load) chooseFile();
    else if (id == set) waveform.applySelection();
    else if (id == add) processor.saveSlot();
    else if (id == twice) waveform.scaleLength(2.0);
    else if (id == half) waveform.scaleLength(0.5);
    else if (id == snap || id == zc)
        processor.setUiSettings(state.grid, state.segments, id == snap ? !state.snap : state.snap, state.triplet, id == zc ? !state.zeroCross : state.zeroCross);
    else if (id == play) { if (processor.isLooping()) processor.stopLoop(); else waveform.applySelection(); }
    else menuFor(id);
    state = processor.getViewState(); layoutTools(); waveform.refreshFromProcessor(); repaint();
}
void MiniSamplerAudioProcessorEditor::menuFor(int id)
{
    juce::PopupMenu menu;
    if (id == length || id == grid)
        for (int i = 0; i < divisions.size(); ++i) menu.addItem(i + 1, divisions[i], true, id == grid && state.grid == i + 1);
    if (id == segments)
    {
        const juce::StringArray labels { "Off", "1/1", "1/2", "1/4", "1/8" };
        for (int i = 0; i < labels.size(); ++i) menu.addItem(i + 1, labels[i], true, state.segments == i + 1);
    }
    if (id == settings)
    {
        menu.addItem(1, "Triplet grid", true, state.triplet);
        menu.addItem(2, "Stereo waveform", true, waveform.stereo);
        menu.addItem(3, "Bright grid", true, waveform.brightGrid);
        menu.addItem(4, "Show full sample / reset zoom");
        menu.addSeparator();
        menu.addItem(5, "Help: mouse and keyboard");
        menu.addItem(6, "MIDI notes change speed / pitch", true, state.midiKeyTracking);
    }
    juce::Component::SafePointer<MiniSamplerAudioProcessorEditor> safe(this);
    menu.showMenuAsync(miniSamplerMenuOptions(*this, menuPosition), [safe, id](int result) { if (safe && result > 0) safe->handleMenu(id, result); });
}
void MiniSamplerAudioProcessorEditor::handleMenu(int id, int result)
{
    state = processor.getViewState();
    if (id == length) waveform.setMusicalLength(LoopMath::divisionBeats(result, processor.getProjectTimeSignatureNumerator(), processor.getProjectTimeSignatureDenominator()));
    else if (id == grid || id == segments || (id == settings && result == 1))
        processor.setUiSettings(id == grid ? result : state.grid, id == segments ? result : state.segments,
                                state.snap, id == settings ? !state.triplet : state.triplet, state.zeroCross);
    else if (id == settings)
    {
        if (result == 2) { waveform.stereo = !waveform.stereo; waveform.resetZoom(); }
        if (result == 3) { waveform.brightGrid = !waveform.brightGrid; waveform.repaint(); }
        if (result == 4) waveform.resetZoom();
        if (result == 6) processor.setMidiKeyTracking(!state.midiKeyTracking);
        if (result == 5) juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Mini Sampler / LoopX",
            "Drag a local audio file onto the waveform.\nDrag to select; SET or right-click applies the loop.\nDrag orange markers to resize; drag the bottom loop bar to move.\nSegments: click a whole segment. + saves a slot (up to 10).\nLeft-click slot recalls; right-click deletes. Keys 1-9/0 recall.\nLeft/Right nudge by grid; Up/Down move one loop length.\nMouse wheel zooms; Shift+wheel or middle-drag pans.\nSnap has priority over ZC (nearest crossing within 5 ms).\nLoop follows DAW Play/Stop/seek and BPM; tempo changes use varispeed.\nSamples are linked by file path, not embedded in the project.");
    }
    state = processor.getViewState(); layoutTools(); waveform.refreshFromProcessor(); repaint();
}
bool MiniSamplerAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (key.getModifiers().isCtrlDown() || key.getModifiers().isAltDown()) return false;
    const auto code = key.getKeyCode();
    if (code >= '1' && code <= '9') { invoke(100 + code - '1', false); return true; }
    if (code == '0') { invoke(109, false); return true; }
    const auto loop = processor.getViewState().loop;
    double step = LoopMath::divisionBeats(processor.getViewState().grid, processor.getProjectTimeSignatureNumerator(), processor.getProjectTimeSignatureDenominator()) * 60.0 / processor.getProjectTempo();
    if (processor.getViewState().triplet) step *= 2.0 / 3.0;
    if (code == juce::KeyPress::leftKey) waveform.moveLoop(-step);
    else if (code == juce::KeyPress::rightKey) waveform.moveLoop(step);
    else if (code == juce::KeyPress::upKey) waveform.moveLoop(loop.end - loop.start);
    else if (code == juce::KeyPress::downKey) waveform.moveLoop(loop.start - loop.end);
    else return false;
    return true;
}
