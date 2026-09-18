#include "PluginEditor.h"
#include "ThemeColours.h"
#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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
    // Text drops can contain local paths or file:// URIs, not just CF_HDROP.
    return juce::File::isAbsolutePath(text) ? juce::File(text) : juce::File{};
}
constexpr int load = 1, set = 2, add = 3, length = 4, twice = 5, half = 6,
              segments = 7, snap = 8, grid = 9, zc = 10, settings = 11, play = 12, bpmTool = 13;
const juce::StringArray divisions { "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32" };
}

MiniSamplerWaveformView::MiniSamplerWaveformView(MiniSamplerAudioProcessor& p) : processor(p)
{
    setOpaque(true); refresh(); startTimerHz(30);
}
MiniSamplerWaveformView::~MiniSamplerWaveformView() { stopTimer(); }
const MiniSamplerSample* MiniSamplerWaveformView::drawingSample() const
{
    return editingStart ? state.originalSample.get() : state.sample.get();
}
double MiniSamplerWaveformView::duration() const
{
    const auto* sample = drawingSample();
    return sample ? sample->duration() - (editingStart ? 0 : state.playbackOffset) : 0;
}
double MiniSamplerWaveformView::visibleLength() const { return duration() / zoom; }
juce::Rectangle<float> MiniSamplerWaveformView::waveArea() const
{
    return getLocalBounds().toFloat().withTrimmedTop(14.0f).withTrimmedBottom(18.0f).reduced(4.0f, 0.0f);
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
    return beats * 60.0 / processor.getTimelineTempo();
}
double MiniSamplerWaveformView::snapTime(double time) const
{
    if (state.snap) return juce::jlimit(0.0, duration(), std::round(time / gridSeconds()) * gridSeconds());
    if (!state.zeroCross || !state.sample) return juce::jlimit(0.0, duration(), time);
    const auto& sample = *state.sample;
    const int count = sample.audio.getNumSamples();
    const int target = juce::jlimit(1, count - 1, static_cast<int>((time + state.playbackOffset) * sample.rate));
    const int radius = static_cast<int>(sample.rate * 0.005);
    int nearest = target, distance = radius + 1;
    const auto* samples = sample.audio.getReadPointer(0);
    for (int i = juce::jmax(1, target - radius); i < juce::jmin(count, target + radius + 1); ++i)
        if ((samples[i - 1] <= 0 && samples[i] >= 0) || (samples[i - 1] >= 0 && samples[i] <= 0))
            if (std::abs(i - target) < distance) { nearest = i; distance = std::abs(i - target); }
    return juce::jlimit(0.0, duration(), nearest / sample.rate - state.playbackOffset);
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
    if (state.originalSample && next.originalSample != state.originalSample) editingStart = false;
    const bool displayChanged = brightGrid != next.brightGrid || stereo != next.stereoWaveform || state.theme != next.theme;
    if (state.theme != next.theme) dirtyCache = true;
    if (stereo != next.stereoWaveform) dirtyCache = true;
    brightGrid = next.brightGrid; stereo = next.stereoWaveform;
    bool overlaysChanged = next.loop.start != state.loop.start || next.loop.end != state.loop.end
        || next.grid != state.grid || next.segments != state.segments || next.snap != state.snap
        || next.loop.fadeIn != state.loop.fadeIn || next.loop.fadeOut != state.loop.fadeOut
        || next.triplet != state.triplet || next.zeroCross != state.zeroCross || next.slots.size() != state.slots.size();
    if (!overlaysChanged)
        for (size_t i = 0; i < next.slots.size(); ++i)
            if (next.slots[i].start != state.slots[i].start || next.slots[i].end != state.slots[i].end)
            { overlaysChanged = true; break; }
    state = std::move(next);
    if (changed) { dirtyCache = true; viewStart = 0; zoom = 1; pendingSelection = false; dragMode = 0; }
    if (!pendingSelection && dragMode == 0) { selectionStart = state.loop.start; selectionEnd = state.loop.end; }
    if (changed || overlaysChanged || displayChanged) repaint();
}
void MiniSamplerWaveformView::resized()
{
    // During live resize paint simply scales the previous cached image.
    // Rebuild peaks geometry only after the resize has settled.
    dirtyCache = true; lastResize = juce::Time::getMillisecondCounterHiRes();
}
void MiniSamplerWaveformView::rebuildWaveCache()
{
    dirtyCache = false;
    const auto* sample = drawingSample();
    if (!sample || getWidth() < 2 || getHeight() < 60) { waveCache = {}; return; }
    const float density = juce::jlimit(1.0f, 3.0f, getDesktopScaleFactor() * juce::Component::getApproximateScaleFactorForComponent(this));
    const int width = juce::jlimit(1, 5400, static_cast<int>(std::ceil(waveArea().getWidth() * density)));
    const int height = juce::jlimit(1, 3000, static_cast<int>(std::ceil(waveArea().getHeight() * density)));
    waveCache = juce::Image(juce::Image::ARGB, width, juce::jmax(1, height), true);
    juce::Graphics g(waveCache);
    const int channels = stereo ? sample->audio.getNumChannels() : 1;
    for (int ch = 0; ch < channels; ++ch)
    {
        const float band = static_cast<float>(height) / channels;
        const float centre = band * (ch + 0.5f);
        const float scale = band * 0.46f;
        const auto palette = loopXPalette(state.theme);
        g.setColour(ch == 0 ? palette.wave : palette.wave2);
        const double firstSample = (viewStart + (editingStart ? 0 : state.playbackOffset)) * sample->rate;
        const double samplesPerPixel = visibleLength() * sample->rate / width;
        if (samplesPerPixel < 1.0)
        {
            juce::Path path;
            const auto* data = sample->audio.getReadPointer(ch);
            const int count = sample->audio.getNumSamples();
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
            const auto range = sample->waveformRange(ch, a, b);
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
    const double nextCursor = editingStart ? -1 : processor.getPlaybackSeconds();
    if (nextCursor != cursor)
    {
        if (cursor >= 0) repaint(static_cast<int>(xForTime(cursor)) - 4, 0, 9, getHeight());
        cursor = nextCursor;
        if (cursor >= 0) repaint(static_cast<int>(xForTime(cursor)) - 4, 0, 9, getHeight());
    }
}
void MiniSamplerWaveformView::paint(juce::Graphics& g)
{
    const auto palette = loopXPalette(state.theme);
    g.fillAll(palette.background);
    const auto area = waveArea();
    g.setColour(juce::Colour(0xff303234)); g.fillRect(0, 0, getWidth(), 14);
    if (!state.sample)
    {
        g.setColour(juce::Colour(0xffa6b1bc)); g.setFont(14.0f);
        g.drawText(dragOver ? "Release to load sample" : "Drop audio here or use Settings > Load audio file", area, juce::Justification::centred);
        if (dragOver) { g.setColour(juce::Colour(0xff66ddc9)); g.drawRect(getLocalBounds(), 3); }
        return;
    }
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    if (waveCache.isValid()) g.drawImage(waveCache, area);
    // Grid work is bounded by pixels, not sample duration or source sample count.
    const double beatSeconds = 60.0 / processor.getTimelineTempo();
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
    }
    if (!editingStart && state.segments > 1)
    {
        const double segment = barSeconds / std::pow(2.0, state.segments - 2);
        const int segmentStride = juce::jmax(1, static_cast<int>(std::ceil(8.0 / juce::jmax(0.0001, segment / visibleLength() * area.getWidth()))));
        const double drawStep = segment * segmentStride;
        for (double t = std::floor(viewStart / drawStep) * drawStep; t <= viewStart + visibleLength(); t += drawStep)
        {
            g.setColour(palette.segment);
            g.drawLine(xForTime(t), area.getY(), xForTime(t), area.getBottom(), 2.0f);
        }
    }
    const float startX = xForTime(state.loop.start), endX = xForTime(state.loop.end);
    if (!editingStart && state.loop.end > state.loop.start)
    {
        const auto selection = juce::Rectangle<float>(startX, area.getY(), endX - startX, area.getHeight()).getIntersection(area);
        g.setColour(juce::Colour(0x604b8585)); g.fillRect(selection);
        g.setColour(palette.accent);
        for (const float x : { startX, endX })
            if (x >= area.getX() && x <= area.getRight()) g.drawLine(x, area.getY(), x, area.getBottom(), 1.0f);
        juce::Path flags;
        if (startX >= area.getX() && startX <= area.getRight()) flags.addTriangle(startX, area.getY(), startX + 10, area.getY(), startX, area.getY() + 10);
        if (endX >= area.getX() && endX <= area.getRight()) flags.addTriangle(endX, area.getY(), endX - 10, area.getY(), endX, area.getY() + 10);
        g.fillPath(flags);
        const auto loopBar = juce::Rectangle<float>(startX, area.getBottom(), endX - startX, 14.0f).getIntersection(getLocalBounds().toFloat());
        g.setColour(juce::Colour(0xff426f74)); g.fillRoundedRectangle(loopBar, 2.0f);
        g.setColour(juce::Colour(0xff66cecd)); g.drawRoundedRectangle(loopBar, 2.0f, 1.0f);
        // Fade handles are separate from the boundary flags.
        const float inX = xForTime(state.loop.start + state.loop.fadeIn);
        const float outX = xForTime(state.loop.end - state.loop.fadeOut);
        g.saveState(); g.reduceClipRegion(area.toNearestInt());
        g.setColour(palette.accent.withAlpha(0.8f));
        g.drawLine(startX, area.getBottom(), inX, area.getY() + 22, 1.2f);
        g.drawLine(outX, area.getY() + 22, endX, area.getBottom(), 1.2f);
        g.fillRect(inX - 4, area.getY() + 18, 8.0f, 8.0f);
        g.fillRect(outX - 4, area.getY() + 18, 8.0f, 8.0f);
        g.restoreState();
    }
    if (!editingStart && pendingSelection && selectionEnd != selectionStart)
    {
        const float a = xForTime(juce::jmin(selectionStart, selectionEnd)), b = xForTime(juce::jmax(selectionStart, selectionEnd));
        g.setColour(juce::Colour(0x90548787));
        g.fillRect(juce::Rectangle<float>(a, area.getY(), b - a, area.getHeight()).getIntersection(area));
    }
    // Musical labels overlay the waveform, never the scrollbar strip.
    const double signatureBeat = beatSeconds * 4.0 / processor.getProjectTimeSignatureDenominator();
    const int labelStride = juce::jmax(1, static_cast<int>(std::ceil(48.0 / juce::jmax(0.0001, signatureBeat / visibleLength() * area.getWidth()))));
    const double labelStep = signatureBeat * labelStride;
    g.setColour(juce::Colour(0xff8b989a)); g.setFont(11.0f);
    for (double t = std::floor(viewStart / labelStep) * labelStep; t <= viewStart + visibleLength(); t += labelStep)
    {
        const float x = xForTime(t);
        if (x < area.getX() || x > area.getRight() - 8.0f) continue;
        const auto beatIndex = static_cast<juce::int64>(std::llround(t / signatureBeat));
        const int numerator = processor.getProjectTimeSignatureNumerator();
        const auto barIndex = beatIndex / numerator + 1;
        const int beatInBar = static_cast<int>(beatIndex % numerator) + 1;
        const auto label = juce::String(barIndex) + (beatInBar == 1 ? juce::String{} : "." + juce::String(beatInBar));
        g.drawText(label, static_cast<int>(x) + 3, static_cast<int>(area.getY()) + 2,
                   juce::jmin(46, static_cast<int>(area.getRight() - x - 3)), 14, juce::Justification::centredLeft);
    }
    for (size_t i = 0; !editingStart && i < state.slots.size(); ++i)
    {
        const auto& slot = state.slots[i];
        const float a = xForTime(slot.start), b = xForTime(slot.end);
        const auto line = juce::Rectangle<float>(a, area.getBottom() - 4.0f, b - a, 3.0f).getIntersection(area);
        g.setColour(juce::Colour(0xffe07a5f)); g.fillRect(line);
        if (line.getWidth() > 0)
        {
            g.setColour(juce::Colour(0xffe07a5f)); g.setFont(16.0f);
            g.drawText(juce::String(i == 9 ? 0 : static_cast<int>(i) + 1), static_cast<int>(line.getX()) + 2, getHeight() - 39, 24, 18, juce::Justification::centredLeft);
        }
    }
    if (!editingStart && cursor >= viewStart && cursor <= viewStart + visibleLength())
    {
        g.setColour(juce::Colour(0xff66cecd)); g.drawLine(xForTime(cursor), area.getY(), xForTime(cursor), area.getBottom(), 2.0f);
    }
    if (editingStart)
    {
        const float x = xForTime(draftStart);
        g.setColour(palette.accent); g.drawLine(x, area.getY(), x, area.getBottom(), 2.0f);
        juce::Path flag; flag.addTriangle(x, area.getY(), x + 11, area.getY(), x, area.getY() + 11); g.fillPath(flag);
    }
    const auto track = juce::Rectangle<float>(4.0f, 0.0f, getWidth() - 8.0f, area.getY());
    const float thumbWidth = juce::jmax(18.0f, track.getWidth() / static_cast<float>(zoom));
    const float thumbX = track.getX() + static_cast<float>(viewStart / juce::jmax(1.0e-9, duration() - visibleLength())) * (track.getWidth() - thumbWidth);
    g.setColour(juce::Colour(0xff303234)); g.fillRoundedRectangle(track, 3.0f);
    g.setColour(juce::Colour(dragMode == 6 ? 0xff686a6c : 0xff484a4c));
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
    const auto end = juce::jmin(duration(), start + beats * 60.0 / processor.getTimelineTempo());
    if (end > start) commit(start, end, (end - start) * processor.getTimelineTempo() / 60.0);
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
    if (getParentComponent()) getParentComponent()->grabKeyboardFocus();
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
    if (event.y >= 0 && event.y < waveArea().getY() && event.x >= 4 && event.x <= getWidth() - 4)
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
    if (editingStart)
    {
        dragMode = 10; draftStart = std::round(timeForX(float(event.x)) * state.originalSample->rate) / state.originalSample->rate;
        repaint(); return;
    }
    const int hit = hitTestTool(float(event.x), float(event.y));
    if (hit == 8 || hit == 9) { dragMode = hit; return; }
    const bool loopValid = state.loop.end > state.loop.start;
    if (event.y >= waveArea().getBottom() && event.y < getHeight() - 4 && loopValid && event.x >= xForTime(state.loop.start) && event.x <= xForTime(state.loop.end)) dragMode = 4;
    else if (loopValid && std::abs(event.x - xForTime(state.loop.start)) <= 6) dragMode = 2;
    else if (loopValid && std::abs(event.x - xForTime(state.loop.end)) <= 6) dragMode = 3;
    else
    {
        dragMode = 1; pendingSelection = true; selectionStart = snapTime(timeForX(static_cast<float>(event.x))); selectionEnd = selectionStart;
        if (state.segments > 1)
        {
            const double seconds = LoopMath::divisionBeats(1, processor.getProjectTimeSignatureNumerator(), processor.getProjectTimeSignatureDenominator())
                / std::pow(2.0, state.segments - 2) * 60 / processor.getTimelineTempo();
            const double start = std::floor(timeForX(float(event.x)) / seconds) * seconds;
            commit(start, juce::jmin(duration(), start + seconds));
            dragMode = 7;
        }
    }
    repaint();
}
void MiniSamplerWaveformView::mouseDrag(const juce::MouseEvent& event)
{
    if (dragMode == 10)
    {
        draftStart = std::round(timeForX(float(event.x)) * state.originalSample->rate) / state.originalSample->rate;
    }
    else if (dragMode == 8 || dragMode == 9)
    {
        const double limit = (state.loop.end - state.loop.start) * 0.5;
        const double time = timeForX(float(event.x));
        processor.setLoopFades(dragMode == 8 ? juce::jlimit(0.0, limit, time - state.loop.start) : state.loop.fadeIn,
                               dragMode == 9 ? juce::jlimit(0.0, limit, state.loop.end - time) : state.loop.fadeOut);
        refresh();
    }
    else if (dragMode == 7 && std::abs(event.x - dragStart) >= 4)
    {
        dragMode = 1; pendingSelection = true; selectionStart = snapTime(timeForX(float(dragStart)));
        selectionEnd = snapTime(timeForX(float(event.x)));
    }
    else if (dragMode == 1) selectionEnd = snapTime(timeForX(static_cast<float>(event.x)));
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
    juce::ignoreUnused(event);
    if (dragMode == 1)
    {
        if (selectionEnd < selectionStart) std::swap(selectionStart, selectionEnd);
        if (selectionEnd - selectionStart < 0.001) pendingSelection = false;
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
int MiniSamplerWaveformView::hitTestTool(float x, float y) const
{
    const auto area = waveArea();
    if (y < area.getY()) return 6;
    if (editingStart) return 10;
    if (state.loop.end <= state.loop.start) return 0;
    if (std::abs(y - (area.getY() + 22)) <= 7)
    {
        if (std::abs(x - xForTime(state.loop.start + state.loop.fadeIn)) <= 7) return 8;
        if (std::abs(x - xForTime(state.loop.end - state.loop.fadeOut)) <= 7) return 9;
    }
    if (y >= area.getBottom() && x >= xForTime(state.loop.start) && x <= xForTime(state.loop.end)) return 4;
    if (std::abs(x - xForTime(state.loop.start)) <= 6 || std::abs(x - xForTime(state.loop.end)) <= 6) return 2;
    return 0;
}
void MiniSamplerWaveformView::mouseMove(const juce::MouseEvent& e)
{
    const int hit = hitTestTool(float(e.x), float(e.y));
    static const juce::MouseCursor moveCursor = []
    {
        juce::Image image(juce::Image::ARGB, 24, 24, true); juce::Graphics g(image);
        juce::Path path; path.startNewSubPath(3, 12); path.lineTo(21, 12);
        path.startNewSubPath(12, 3); path.lineTo(12, 21);
        path.addTriangle(0, 12, 5, 8, 5, 16); path.addTriangle(24, 12, 19, 8, 19, 16);
        path.addTriangle(12, 0, 8, 5, 16, 5); path.addTriangle(12, 24, 8, 19, 16, 19);
        g.setColour(juce::Colours::black); g.strokePath(path, juce::PathStrokeType(3));
        g.setColour(juce::Colours::white); g.strokePath(path, juce::PathStrokeType(1)); g.fillPath(path);
        return juce::MouseCursor(image, 12, 12);
    }();
    setMouseCursor(hit == 4 ? moveCursor :
        (hit != 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor));
}
void MiniSamplerWaveformView::mouseExit(const juce::MouseEvent&) { setMouseCursor(juce::MouseCursor::NormalCursor); }
void MiniSamplerWaveformView::toggleStart()
{
    refresh();
    if (!state.originalSample) return;
    if (editingStart) processor.setStartOffset(draftStart);
    else draftStart = state.startOffset;
    editingStart = !editingStart; pendingSelection = false; resetZoom();
}
void MiniSamplerWaveformView::resetZoom() { zoom = 1.0; viewStart = 0.0; dirtyCache = true; repaint(); }

MiniSamplerAudioProcessorEditor::MiniSamplerAudioProcessorEditor(MiniSamplerAudioProcessor& p) : AudioProcessorEditor(&p), processor(p), waveform(p)
{
    state = processor.getViewState();
    setOpaque(true); setWantsKeyboardFocus(true); setResizable(true, true); setResizeLimits(900, 260, 1800, 1100);
    addAndMakeVisible(waveform);
    waveform.onChanged = [this] { state = processor.getViewState(); layoutTools(); repaint(); };
    setSize(juce::jmax(900, state.width), state.height); startTimerHz(15);
}
MiniSamplerAudioProcessorEditor::~MiniSamplerAudioProcessorEditor() { stopTimer(); }
void MiniSamplerAudioProcessorEditor::layoutTools()
{
    tools.clear();
    const auto put = [this](int id, juce::String text, int x, int width, bool active = false)
    { tools.push_back({ id, std::move(text), { x, 5, width, 18 }, active }); };
    put(load, waveform.isEditingStart() ? "OK" : "START", 6, 45, waveform.isEditingStart()); put(set, "SET", 55, 34);
    int x = 94;
    for (size_t i = 0; i < state.slots.size(); ++i) { put(100 + static_cast<int>(i), juce::String(i == 9 ? 0 : static_cast<int>(i) + 1), x, 22, true); x += 25; }
    put(add, "+", x, 22, state.slots.size() < 10);
    // Central musical length group stays centred, independently of slot count.
    const int centre = juce::jlimit(x + 26, getWidth() - 524, getWidth() / 2 - 93);
    put(length, "LOOP LENGTH", centre, 112, true); put(twice, juce::String::charToString(0x00d7) + "2", centre + 116, 31); put(half, juce::String::charToString(0x00f7) + "2", centre + 151, 31);
    x = getWidth() - 332;
    put(bpmTool, "BPM", x, 42, state.stretchApplied); x += 46;
    put(segments, state.segments == 1 ? "Segments" : "Seg " + juce::StringArray({ "Off", "1/1", "1/2", "1/4", "1/8" })[state.segments - 1], x, 72, state.segments > 1); x += 76;
    put(snap, "Snap", x, 42, state.snap); x += 46;
    put(grid, divisions[state.grid - 1] + (state.triplet ? "T" : ""), x, 49); x += 53;
    put(zc, "ZC", x, 28, state.zeroCross); x += 32;
    put(play, processor.isLooping() ? "Stop" : "Loop", x, 40, processor.isLooping()); x += 44;
    put(settings, juce::String::charToString(0x2699), x, 27);
}
void MiniSamplerAudioProcessorEditor::resized()
{
    waveform.setBounds(4, 28, getWidth() - 8, juce::jmax(10, getHeight() - 32));
    layoutTools(); processor.setEditorSize(getWidth(), getHeight());
}
void MiniSamplerAudioProcessorEditor::paint(juce::Graphics& g)
{
    const auto palette = loopXPalette(state.theme);
    g.fillAll(palette.toolbar); g.setFont(12.0f);
    for (const auto& tool : tools)
    {
        g.setFont(tool.id >= 100 ? 15.0f : 12.0f);
        g.setColour(tool.active ? palette.active : palette.button); g.fillRoundedRectangle(tool.rect.toFloat(), 2.0f);
        g.setColour(tool.active ? palette.accent : palette.text.withAlpha(0.4f)); g.drawRoundedRectangle(tool.rect.toFloat(), 2.0f, 1.0f);
        g.setColour(palette.text); g.drawText(tool.text, tool.rect, juce::Justification::centred);
    }
}
void MiniSamplerAudioProcessorEditor::timerCallback()
{
    const auto previous = state;
    state = processor.getViewState();
    if (previous.slots.size() != state.slots.size() || previous.loop.start != state.loop.start || previous.loop.end != state.loop.end
        || previous.grid != state.grid || previous.segments != state.segments || previous.snap != state.snap || previous.zeroCross != state.zeroCross
        || previous.theme != state.theme || previous.stretchApplied != state.stretchApplied || previous.triplet != state.triplet || lastPlaying != processor.isLooping())
    { layoutTools(); repaint(0, 0, getWidth(), 28); }
    if (lastTempo != processor.getTimelineTempo()) waveform.repaint();
    lastTempo = processor.getTimelineTempo(); lastPlaying = processor.isLooping();
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
    grabKeyboardFocus(); menuPosition = event.getScreenPosition();
    for (const auto& tool : tools) if (tool.rect.contains(event.getPosition())) { invoke(tool.id, event.mods.isRightButtonDown()); break; }
}
void MiniSamplerAudioProcessorEditor::invoke(int id, bool rightClick)
{
    state = processor.getViewState();
    if (id >= 100) { if (rightClick) processor.deleteSlot(id - 100); else processor.recallSlot(id - 100); }
    else if (id == load) waveform.toggleStart();
    else if (id == bpmTool) showBpm();
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
        juce::PopupMenu playback, midi, display, themes;
        playback.addItem(20, "MIDI Trigger", true, state.playbackMode == 0);
        playback.addItem(21, "Continuous / Host Sync", true, state.playbackMode == 1);
        midi.addItem(6, "MIDI key tracking", true, state.midiKeyTracking);
        midi.addSeparator();
        midi.addItem(30, "Velocity: Off", true, state.velocityMode == 0);
        midi.addItem(31, "Velocity -> Slot", true, state.velocityMode == 1);
        midi.addItem(32, "Velocity -> Loop Position", true, state.velocityMode == 2);
        display.addItem(2, "Stereo waveform", true, state.stereoWaveform);
        display.addItem(3, "Bright Grid", true, state.brightGrid);
        const juce::StringArray names {"Default", "Graphite", "Coral", "Violet", "Amber"};
        for (int i = 0; i < names.size(); ++i) themes.addItem(40 + i, names[i], true, state.theme == i);
        menu.addSubMenu("Playback", playback); menu.addSubMenu("MIDI", midi);
        menu.addSubMenu("Display", display); menu.addSubMenu("Themes", themes);
        menu.addSeparator(); menu.addItem(50, "Load audio file...");
    }
    juce::Component::SafePointer<MiniSamplerAudioProcessorEditor> safe(this);
    menu.showMenuAsync(miniSamplerMenuOptions(*this, menuPosition), [safe, id](int result) { if (safe && result > 0) safe->handleMenu(id, result); });
}
void MiniSamplerAudioProcessorEditor::handleMenu(int id, int result)
{
    state = processor.getViewState();
    if (id == length) waveform.setMusicalLength(LoopMath::divisionBeats(result, processor.getProjectTimeSignatureNumerator(), processor.getProjectTimeSignatureDenominator()));
    else if (id == grid || id == segments)
        processor.setUiSettings(id == grid ? result : state.grid, id == segments ? result : state.segments,
                                state.snap, state.triplet, state.zeroCross);
    else if (id == settings)
    {
        if (result == 2) { processor.setDisplaySettings(state.brightGrid, !state.stereoWaveform); waveform.resetZoom(); }
        if (result == 3) processor.setDisplaySettings(!state.brightGrid, state.stereoWaveform);
        if (result == 6) processor.setMidiKeyTracking(!state.midiKeyTracking);
        if (result == 20 || result == 21) processor.setPlaybackSettings(result - 20, state.velocityMode);
        if (result >= 30 && result <= 32) processor.setPlaybackSettings(state.playbackMode, result - 30);
        if (result >= 40 && result <= 44) processor.setTheme(result - 40);
        if (result == 50) chooseFile();
    }
    state = processor.getViewState(); layoutTools(); waveform.refreshFromProcessor(); repaint();
}
bool MiniSamplerAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (key.getModifiers().isCtrlDown() || key.getModifiers().isAltDown()) return false;
    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::spaceKey)
    {
        if (auto* host = processor.getPlayHead(); host && host->canControlTransport())
        { host->transportPlay(!processor.isHostPlaying()); return true; }
#if JUCE_WINDOWS
        if (auto* peer = getPeer())
        {
            const auto hwnd = static_cast<HWND>(peer->getNativeHandle());
            const auto parent = GetAncestor(hwnd, GA_ROOTOWNER);
            if (parent && parent != hwnd)
            {
                PostMessageW(parent, WM_KEYDOWN, VK_SPACE, 1);
                PostMessageW(parent, WM_KEYUP, VK_SPACE, (LPARAM(1) << 31) | (LPARAM(1) << 30) | 1);
                return true;
            }
        }
#endif
        return false;
    }
    if (code >= '1' && code <= '9') { invoke(100 + code - '1', false); return true; }
    if (code == '0') { invoke(109, false); return true; }
    const auto loop = processor.getViewState().loop;
    double step = LoopMath::divisionBeats(processor.getViewState().grid, processor.getProjectTimeSignatureNumerator(), processor.getProjectTimeSignatureDenominator()) * 60.0 / processor.getTimelineTempo();
    if (processor.getViewState().triplet) step *= 2.0 / 3.0;
    if (code == juce::KeyPress::leftKey) waveform.moveLoop(-step);
    else if (code == juce::KeyPress::rightKey) waveform.moveLoop(step);
    else if (code == juce::KeyPress::upKey) waveform.moveLoop(loop.end - loop.start);
    else if (code == juce::KeyPress::downKey) waveform.moveLoop(loop.start - loop.end);
    else return false;
    return true;
}
void MiniSamplerAudioProcessorEditor::mouseMove(const juce::MouseEvent& e)
{
    for (const auto& t : tools) if (t.rect.contains(e.getPosition()))
    { setMouseCursor(juce::MouseCursor::PointingHandCursor); return; }
    setMouseCursor(juce::MouseCursor::NormalCursor);
}
void MiniSamplerAudioProcessorEditor::mouseExit(const juce::MouseEvent&) { setMouseCursor(juce::MouseCursor::NormalCursor); }

namespace {
class BpmPanel final : public juce::Component, private juce::Timer
{
public:
    explicit BpmPanel(MiniSamplerAudioProcessor& p) : processor(p)
    {
        addAndMakeVisible(original); addAndMakeVisible(target); addAndMakeVisible(match); addAndMakeVisible(title);
        title.setText("Original BPM (sample)", juce::dontSendNotification);
        original.setInputRestrictions(8, "0123456789."); original.setText(juce::String(p.getViewState().originalBpm, 2));
        match.setButtonText("Match BPM");
        match.onClick = [this]
        {
            if (!processor.matchBpm(original.getText().getDoubleValue()))
                target.setText("Load audio / enter BPM 20-400", juce::dontSendNotification);
        };
        setSize(256, 138); startTimerHz(10); timerCallback();
    }
    void resized() override
    {
        title.setBounds(12, 8, 232, 22); original.setBounds(12, 32, 232, 24);
        target.setBounds(12, 60, 232, 24); match.setBounds(12, 94, 232, 28);
    }
private:
    void timerCallback() override
    {
        target.setText("Project: " + juce::String(processor.getProjectTempo(), 2) + " BPM" +
            (processor.isLoading() ? " (rendering)" : ""), juce::dontSendNotification);
    }
    MiniSamplerAudioProcessor& processor;
    juce::TextEditor original; juce::Label target, title; juce::TextButton match;
};
}
void MiniSamplerAudioProcessorEditor::showBpm()
{
    const auto local = getLocalPoint(nullptr, menuPosition);
    juce::CallOutBox::launchAsynchronously(std::make_unique<BpmPanel>(processor), {local.x, local.y, 1, 1}, this);
}
