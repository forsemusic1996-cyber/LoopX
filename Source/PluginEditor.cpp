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
              segments = 7, snap = 8, grid = 9, zc = 10, settings = 11, play = 12, bpmTool = 13, midiTool = 14;
const juce::StringArray divisions { "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32" };
}

LoopXWaveformView::LoopXWaveformView(LoopXAudioProcessor& p) : processor(p)
{
    setOpaque(true); refresh(); startTimerHz(30);
}
LoopXWaveformView::~LoopXWaveformView()
{
    stopTimer();
    if (loopPositionGesture) processor.positionParameter->endChangeGesture();
}
const LoopXSample* LoopXWaveformView::drawingSample() const
{
    return editingStart ? state.originalSample.get() : state.sample.get();
}
double LoopXWaveformView::duration() const
{
    const auto* sample = drawingSample();
    return sample ? sample->duration() - (editingStart ? 0 : state.playbackOffset) : 0;
}
double LoopXWaveformView::visibleLength() const { return duration() / zoom; }
juce::Rectangle<float> LoopXWaveformView::waveArea() const
{
    return getLocalBounds().toFloat().withTrimmedTop(14.0f).withTrimmedBottom(18.0f).reduced(4.0f, 0.0f);
}
float LoopXWaveformView::xForTime(double time) const
{
    const auto area = waveArea();
    return area.getX() + static_cast<float>((time - viewStart) / juce::jmax(1.0e-9, visibleLength())) * area.getWidth();
}
double LoopXWaveformView::timeForX(float x) const
{
    const auto area = waveArea();
    return juce::jlimit(0.0, duration(), viewStart + (x - area.getX()) / juce::jmax(1.0f, area.getWidth()) * visibleLength());
}
double LoopXWaveformView::gridSeconds() const
{
    auto beats = LoopMath::divisionBeats(state.grid, processor.getProjectTimeSignatureNumerator(), processor.getProjectTimeSignatureDenominator());
    if (state.triplet) beats *= 2.0 / 3.0;
    return beats * 60.0 / processor.getTimelineTempo();
}
double LoopXWaveformView::snapTime(double time) const
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
bool LoopXWaveformView::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& name : files) if (audioFile(fileFromText(name))) return true;
    return false;
}
void LoopXWaveformView::filesDropped(const juce::StringArray& files, int, int)
{
    dragOver = false;
    for (const auto& name : files)
        if (const auto file = fileFromText(name); audioFile(file)) { processor.requestSampleLoad(file); break; }
    repaint();
}
void LoopXWaveformView::fileDragEnter(const juce::StringArray&, int, int) { dragOver = true; repaint(); }
void LoopXWaveformView::fileDragExit(const juce::StringArray&) { dragOver = false; repaint(); }
bool LoopXWaveformView::isInterestedInTextDrag(const juce::String& text) { return audioFile(fileFromText(text)); }
void LoopXWaveformView::textDropped(const juce::String& text, int, int)
{
    dragOver = false;
    const auto file = fileFromText(text); if (audioFile(file)) processor.requestSampleLoad(file);
    repaint();
}
void LoopXWaveformView::refresh()
{
    auto next = processor.getViewState();
    const bool changed = next.sample != state.sample;
    if (state.originalSample && next.originalSample != state.originalSample) editingStart = false;
    const bool displayChanged = brightGrid != next.brightGrid || stereo != next.stereoWaveform || state.palette != next.palette;
    if (state.palette != next.palette) dirtyCache = true;
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
    if (state.segments > 1) pendingSelection = false;
    if (changed) { dirtyCache = true; viewStart = 0; zoom = 1; pendingSelection = false; dragMode = 0; }
    if (!pendingSelection && dragMode == 0) { selectionStart = state.loop.start; selectionEnd = state.loop.end; }
    if (changed || overlaysChanged || displayChanged) repaint();
}
void LoopXWaveformView::resized()
{
    // During live resize paint simply scales the previous cached image.
    // Rebuild peaks geometry only after the resize has settled.
    dirtyCache = true; lastResize = juce::Time::getMillisecondCounterHiRes();
}
void LoopXWaveformView::rebuildWaveCache()
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
        const auto palette = state.palette;
        const double firstSample = (viewStart + (editingStart ? 0 : state.playbackOffset)) * sample->rate;
        const double samplesPerPixel = visibleLength() * sample->rate / width;
        const float distant = float(juce::jlimit(0.0,1.0,std::log2(juce::jmax(1.0,samplesPerPixel/64.0))/5.0));
        g.setColour((ch == 0 ? palette.wave : palette.wave2).withMultipliedAlpha(1.0f-0.25f*distant));
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
        std::vector<std::pair<float,float>> ranges; ranges.reserve(size_t(width));
        for (int x = 0; x < width; ++x)
        {
            const int a = static_cast<int>(firstSample + x * samplesPerPixel);
            const int b = static_cast<int>(std::ceil(firstSample + (x + 1) * samplesPerPixel));
            const auto range = sample->waveformRange(ch, a, b);
            ranges.push_back(range);
            if (distant <= 0) g.drawVerticalLine(x, centre - range.second * scale, centre - range.first * scale + density * 0.5f);
        }
        if (distant > 0)
        {
            // Anti-aliased continuous envelope, still passing through the
            // original min/max of every pixel. No averaging away transients.
            juce::Path envelope; envelope.startNewSubPath(0,centre-ranges.front().second*scale);
            for (int x=1; x<width; ++x) envelope.lineTo(float(x),centre-ranges[size_t(x)].second*scale);
            for (int x=width-1; x>=0; --x) envelope.lineTo(float(x),centre-ranges[size_t(x)].first*scale);
            envelope.closeSubPath(); g.fillPath(envelope); g.strokePath(envelope,juce::PathStrokeType(0.5f));
        }
    }
    repaint();
}
void LoopXWaveformView::timerCallback()
{
    const auto oldLoop = state.loop;
    const auto oldGrid = state.grid; const auto oldSegments = state.segments;
    refresh();
    if (dirtyCache && juce::Time::getMillisecondCounterHiRes() - lastResize > 90.0) rebuildWaveCache();
    if (state.loop.start != oldLoop.start || state.loop.end != oldLoop.end
        || state.loop.fadeIn != oldLoop.fadeIn || state.loop.fadeOut != oldLoop.fadeOut
        || state.loop.fadeInCurve != oldLoop.fadeInCurve || state.loop.fadeOutCurve != oldLoop.fadeOutCurve
        || state.grid != oldGrid || state.segments != oldSegments) repaint();
    const double nextCursor = editingStart ? -1 : processor.getPlaybackSeconds();
    if (nextCursor != cursor)
    {
        if (cursor >= 0) repaint(static_cast<int>(xForTime(cursor)) - 4, 0, 9, getHeight());
        cursor = nextCursor;
        if (cursor >= 0) repaint(static_cast<int>(xForTime(cursor)) - 4, 0, 9, getHeight());
    }
}
void LoopXWaveformView::paint(juce::Graphics& g)
{
    const auto palette = state.palette;
    g.fillAll(palette.background);
    const auto area = waveArea();
    g.setColour(palette.scrollTrack); g.fillRect(0, 0, getWidth(), 14);
    if (!state.sample)
    {
        g.setColour(palette.text); g.setFont(14.0f);
        g.drawText(dragOver ? "Release to load sample" : "Drop audio here or use Settings > Load audio file", area, juce::Justification::centred);
        if (dragOver) { g.setColour(palette.dropBorder); g.drawRect(getLocalBounds(), 3); }
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
        g.setColour(bar ? (brightGrid ? palette.gridBarBright : palette.gridBar)
                        : (brightGrid ? palette.gridBright : palette.grid));
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
            g.drawLine(xForTime(t), area.getY(), xForTime(t), area.getBottom(), 1.0f);
        }
    }
    const float startX = xForTime(state.loop.start), endX = xForTime(state.loop.end);
    if (!editingStart && state.loop.end > state.loop.start)
    {
        const auto selection = juce::Rectangle<float>(startX, area.getY(), endX - startX, area.getHeight()).getIntersection(area);
        g.setColour(palette.loopFill); g.fillRect(selection);
        g.setColour(palette.loopBorder);
        for (const float x : { startX, endX })
            if (x >= area.getX() && x <= area.getRight()) g.drawLine(x, area.getY(), x, area.getBottom(), 1.0f);
        juce::Path flags;
        if (startX >= area.getX() && startX <= area.getRight()) flags.addTriangle(startX, area.getY(), startX + 10, area.getY(), startX, area.getY() + 10);
        if (endX >= area.getX() && endX <= area.getRight()) flags.addTriangle(endX, area.getY(), endX - 10, area.getY(), endX, area.getY() + 10);
        g.fillPath(flags);
        const auto loopBar = juce::Rectangle<float>(startX, area.getBottom(), endX - startX, 14.0f).getIntersection(getLocalBounds().toFloat());
        g.setColour(palette.loopMarker); g.fillRoundedRectangle(loopBar, 2.0f);
        g.setColour(palette.loopMarkerBorder); g.drawRoundedRectangle(loopBar, 2.0f, 1.0f);
        // Fade handles are separate from the boundary flags.
        const float inX = xForTime(state.loop.start + state.loop.fadeIn);
        const float outX = xForTime(state.loop.end - state.loop.fadeOut);
        g.saveState(); g.reduceClipRegion(area.toNearestInt());
        const auto drawFade = [&](bool fadeIn)
        {
            const double curve = fadeIn ? state.loop.fadeInCurve : state.loop.fadeOutCurve;
            const double exponent = std::pow(4.0,-curve);
            const float x0 = fadeIn ? startX : outX, x1 = fadeIn ? inX : endX;
            juce::Path path; path.startNewSubPath(x0, fadeIn ? area.getBottom() : area.getY()+22);
            for (int n=1;n<=32;++n)
            {
                const double t=double(n)/32.0, gain=std::pow(fadeIn?t:1.0-t,exponent);
                path.lineTo(juce::jmap(float(t),x0,x1),area.getBottom()-float(gain)*(area.getHeight()-22.0f));
            }
            const bool active = fadeIn ? hoverFade==8 || dragMode==11 : hoverFade==9 || dragMode==12;
            g.setColour(active ? palette.active : palette.fade); g.strokePath(path,juce::PathStrokeType(active?2.2f:1.2f));
        };
        drawFade(true); drawFade(false);
        g.setColour(hoverFade != 0 || dragMode == 11 || dragMode == 12 ? palette.active : palette.fade);
        g.fillRect(inX - 4, area.getY() + 18, 8.0f, 8.0f);
        g.fillRect(outX - 4, area.getY() + 18, 8.0f, 8.0f);
        g.restoreState();
    }
    if (!editingStart && pendingSelection && selectionEnd != selectionStart)
    {
        const float a = xForTime(juce::jmin(selectionStart, selectionEnd)), b = xForTime(juce::jmax(selectionStart, selectionEnd));
        g.setColour(palette.selection);
        g.fillRect(juce::Rectangle<float>(a, area.getY(), b - a, area.getHeight()).getIntersection(area));
    }
    // Musical labels overlay the waveform, never the scrollbar strip.
    const double signatureBeat = beatSeconds * 4.0 / processor.getProjectTimeSignatureDenominator();
    const int labelStride = juce::jmax(1, static_cast<int>(std::ceil(48.0 / juce::jmax(0.0001, signatureBeat / visibleLength() * area.getWidth()))));
    const double labelStep = signatureBeat * labelStride;
    g.setColour(palette.gridText); g.setFont(11.0f);
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
        g.setColour(palette.slot); g.fillRect(line);
        if (line.getWidth() > 0)
        {
            g.setColour(palette.slotText); g.setFont(16.0f);
            const auto label = juce::String(int(i)+1) + " " + LoopXAudioProcessor::noteLabel(juce::jmin(127,state.rootNote+int(i)));
            g.drawText(label, static_cast<int>(line.getX()) + 2, getHeight() - 39, juce::jmin(90,int(line.getWidth())-2), 18, juce::Justification::centredLeft);
        }
    }
    if (!editingStart && cursor >= viewStart && cursor <= viewStart + visibleLength())
    {
        g.setColour(palette.cursor); g.drawLine(xForTime(cursor), area.getY(), xForTime(cursor), area.getBottom(), 2.0f);
    }
    if (editingStart)
    {
        const float x = xForTime(draftStart);
        g.setColour(palette.start); g.drawLine(x, area.getY(), x, area.getBottom(), 2.0f);
        juce::Path flag; flag.addTriangle(x, area.getY(), x + 11, area.getY(), x, area.getY() + 11); g.fillPath(flag);
    }
    const auto track = juce::Rectangle<float>(4.0f, 0.0f, getWidth() - 8.0f, area.getY());
    const float thumbWidth = juce::jmax(18.0f, track.getWidth() / static_cast<float>(zoom));
    const float thumbX = track.getX() + static_cast<float>(viewStart / juce::jmax(1.0e-9, duration() - visibleLength())) * (track.getWidth() - thumbWidth);
    g.setColour(palette.scrollTrack); g.fillRoundedRectangle(track, 3.0f);
    g.setColour(dragMode == 6 ? palette.scrollActive : palette.scrollThumb);
    g.fillRoundedRectangle({ thumbX, track.getY(), thumbWidth, track.getHeight() }, 3.0f);
    if (dragOver)
    {
        g.setColour(palette.dropFill); g.fillRect(area);
        g.setColour(palette.dropBorder); g.drawRect(getLocalBounds(), 3);
        g.setColour(palette.text); g.setFont(18.0f);
        g.drawText("Release to load sample", area, juce::Justification::centred);
    }
}
void LoopXWaveformView::commit(double start, double end, double beats)
{
    processor.setLoopSelection(start, end, beats); pendingSelection = false; refresh();
    selectionStart = state.loop.start; selectionEnd = state.loop.end;
    repaint(); if (onChanged) onChanged();
}
void LoopXWaveformView::applySelection()
{
    if (state.segments == 1 && pendingSelection && selectionEnd > selectionStart) commit(selectionStart, selectionEnd);
}
void LoopXWaveformView::activateSegmentAt(float x)
{
    if (state.segments <= 1) return;
    const double seconds = LoopMath::divisionBeats(1, processor.getProjectTimeSignatureNumerator(), processor.getProjectTimeSignatureDenominator())
        / std::pow(2.0, state.segments - 2) * 60.0 / processor.getTimelineTempo();
    const double start = std::floor(timeForX(x) / seconds) * seconds;
    pendingSelection = false;
    commit(start, juce::jmin(duration(), start + seconds));
}
void LoopXWaveformView::setLoopPositionParameter(double start)
{
    const double length = dragLoopEnd - dragLoopStart;
    const double lastStart = juce::jmax(0.0, duration() - length);
    const float normalised = lastStart > 0 ? static_cast<float>(juce::jlimit(0.0, lastStart, start) / lastStart) : 0.0f;
    processor.positionParameter->setValueNotifyingHost(normalised);
    refresh();
    if (onChanged) onChanged();
}
void LoopXWaveformView::setMusicalLength(double beats)
{
    refresh();
    const auto start = pendingSelection ? juce::jmin(selectionStart, selectionEnd) : state.loop.start;
    const auto end = juce::jmin(duration(), start + beats * 60.0 / processor.getTimelineTempo());
    if (end > start) commit(start, end, (end - start) * processor.getTimelineTempo() / 60.0);
}
void LoopXWaveformView::scaleLength(double factor)
{
    refresh(); if (state.loop.end <= state.loop.start) return;
    const auto end = LoopMath::scaledEnd(state.loop.start, state.loop.end, duration(), factor);
    const auto beats = state.loop.beats * (end - state.loop.start) / (state.loop.end - state.loop.start);
    commit(state.loop.start, end, beats);
}
void LoopXWaveformView::moveLoop(double seconds)
{
    refresh(); const double len = state.loop.end - state.loop.start;
    if (len <= 0) return;
    const auto start = juce::jlimit(0.0, juce::jmax(0.0, duration() - len), state.loop.start + seconds);
    commit(start, start + len, state.loop.beats);
}
void LoopXWaveformView::mouseDown(const juce::MouseEvent& event)
{
    refresh(); if (!state.sample) return;
    if (getParentComponent()) getParentComponent()->grabKeyboardFocus();
    if (event.mods.isRightButtonDown())
    {
        juce::PopupMenu menu;
        menu.addItem(1, "SET selection as loop", state.segments == 1 && pendingSelection && selectionEnd > selectionStart);
        menu.addItem(2, "Save loop to slot", state.loop.end > state.loop.start && state.slots.size() < 10);
        menu.addSeparator();
        menu.addItem(3, "MIDI Trigger", true, state.playbackMode == 0);
        menu.addItem(4, "Continuous Scrolling / Host Sync", true, state.playbackMode == 1);
        juce::Component::SafePointer<LoopXWaveformView> safe(this);
        menu.showMenuAsync(loopXMenuOptions(*getParentComponent(), event.getScreenPosition()), [safe](int result)
        {
            if (!safe) return;
            if (result == 1) safe->applySelection();
            if (result == 2) safe->processor.saveSlot();
            if (result == 3 || result == 4)
            {
                const auto current = safe->processor.getViewState();
                safe->processor.setPlaybackSettings(result == 3 ? 0 : 1, current.velocityMode);
            }
            if (result > 0)
            {
                if (safe->onChanged) safe->onChanged();
                safe->refresh();
                safe->repaint();
            }
        });
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
    if (event.mods.isCtrlDown() && (hit == 8 || hit == 9))
    {
        dragMode = hit == 8 ? 11 : 12; dragStart = event.y;
        dragCurve = hit == 8 ? state.loop.fadeInCurve : state.loop.fadeOutCurve;
        return;
    }
    if (hit == 8 || hit == 9) { dragMode = hit; return; }
    if (hit == 4)
    {
        dragMode = 4; loopPositionGesture = true;
        processor.positionParameter->beginChangeGesture();
        setLoopPositionParameter(state.loop.start);
        return;
    }
    const bool loopValid = state.loop.end > state.loop.start;
    if (loopValid && std::abs(event.x - xForTime(state.loop.start)) <= 6) dragMode = 2;
    else if (loopValid && std::abs(event.x - xForTime(state.loop.end)) <= 6) dragMode = 3;
    else if (state.segments > 1)
    {
        dragMode = 7;
        activateSegmentAt(float(event.x));
    }
    else
    {
        dragMode = 1; pendingSelection = true; selectionStart = snapTime(timeForX(static_cast<float>(event.x))); selectionEnd = selectionStart;
    }
    repaint();
}
void LoopXWaveformView::mouseDrag(const juce::MouseEvent& event)
{
    if (dragMode == 10)
    {
        draftStart = std::round(timeForX(float(event.x)) * state.originalSample->rate) / state.originalSample->rate;
    }
    else if (dragMode == 11 || dragMode == 12)
    {
        const double curve = juce::jlimit(-1.0,1.0,dragCurve + (dragStart-event.y)/80.0);
        processor.setLoopFadeCurves(dragMode==11 ? curve : state.loop.fadeInCurve,
                                    dragMode==12 ? curve : state.loop.fadeOutCurve);
        refresh(); hoverFade = dragMode==11 ? 8 : 9;
    }
    else if (dragMode == 8 || dragMode == 9)
    {
        const double limit = (state.loop.end - state.loop.start) * 0.5;
        const double time = timeForX(float(event.x));
        processor.setLoopFades(dragMode == 8 ? juce::jlimit(0.0, limit, time - state.loop.start) : state.loop.fadeIn,
                               dragMode == 9 ? juce::jlimit(0.0, limit, state.loop.end - time) : state.loop.fadeOut);
        refresh();
    }
    else if (dragMode == 7) activateSegmentAt(float(event.x));
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
        setMouseCursor(juce::MouseCursor::UpDownLeftRightResizeCursor);
        const double len = dragLoopEnd - dragLoopStart;
        const double delta = (event.x - dragStart) / juce::jmax(1.0f, waveArea().getWidth()) * visibleLength() / (event.mods.isShiftDown() ? 10.0 : 1.0);
        const double start = juce::jlimit(0.0, juce::jmax(0.0, duration() - len), snapTime(dragLoopStart + delta));
        setLoopPositionParameter(start);
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
void LoopXWaveformView::mouseUp(const juce::MouseEvent& event)
{
    juce::ignoreUnused(event);
    const int completedMode = dragMode;
    if (dragMode == 1)
    {
        if (selectionEnd < selectionStart) std::swap(selectionStart, selectionEnd);
        if (selectionEnd - selectionStart < 0.001) pendingSelection = false;
    }
    dragMode = 0;
    if (completedMode == 4 && loopPositionGesture)
    {
        processor.positionParameter->endChangeGesture();
        loopPositionGesture = false;
    }
    refresh(); mouseMove(event); repaint();
}
void LoopXWaveformView::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
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
int LoopXWaveformView::hitTestTool(float x, float y) const
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
    const auto loopBar = juce::Rectangle<float>(xForTime(state.loop.start), area.getBottom(),
                                                xForTime(state.loop.end) - xForTime(state.loop.start), 14.0f)
                             .getIntersection(getLocalBounds().toFloat());
    if (loopBar.contains(x, y)) return 4;
    if (std::abs(x - xForTime(state.loop.start)) <= 6 || std::abs(x - xForTime(state.loop.end)) <= 6) return 2;
    return 0;
}
void LoopXWaveformView::mouseMove(const juce::MouseEvent& e)
{
    const int hit = hitTestTool(float(e.x), float(e.y));
    const int nextHover = e.mods.isCtrlDown() && (hit==8 || hit==9) ? hit : 0;
    if (nextHover != hoverFade) { hoverFade=nextHover; repaint(); }
    // JUCE maps this standard cursor directly to IDC_SIZEALL on Windows.
    setMouseCursor(nextHover ? juce::MouseCursor::UpDownLeftRightResizeCursor : hit == 4 ? juce::MouseCursor::UpDownLeftRightResizeCursor :
        (hit != 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor));
}
void LoopXWaveformView::mouseExit(const juce::MouseEvent&) { if (dragMode == 0) { hoverFade=0; setMouseCursor(juce::MouseCursor::NormalCursor); repaint(); } }
void LoopXWaveformView::toggleStart()
{
    refresh();
    if (!state.originalSample) return;
    if (editingStart) processor.setStartOffset(draftStart);
    else draftStart = state.startOffset;
    editingStart = !editingStart; pendingSelection = false; resetZoom();
}
void LoopXWaveformView::resetZoom() { zoom = 1.0; viewStart = 0.0; dirtyCache = true; repaint(); }

LoopXAudioProcessorEditor::LoopXAudioProcessorEditor(LoopXAudioProcessor& p) : AudioProcessorEditor(&p), processor(p), waveform(p)
{
    state = processor.getViewState();
    lookAndFeel.apply(state.palette); setLookAndFeel(&lookAndFeel);
    setOpaque(true); setWantsKeyboardFocus(true); setResizable(true, true); setResizeLimits(1000, 260, 1800, 1100);
    addAndMakeVisible(waveform);
    waveform.onChanged = [this] { state = processor.getViewState(); layoutTools(); repaint(); };
    setSize(juce::jmax(1000, state.width), state.height); startTimerHz(15);
}
LoopXAudioProcessorEditor::~LoopXAudioProcessorEditor()
{
    stopTimer(); waveform.onChanged = {}; controlPanel.reset(); fileChooser.reset(); setLookAndFeel(nullptr);
}
void LoopXAudioProcessorEditor::layoutTools()
{
    tools.clear();
    const auto put = [this](int id, juce::String text, int x, int width, bool active = false)
    { tools.push_back({ id, std::move(text), { x, 5, width, 18 }, active }); };
    put(load, waveform.isEditingStart() ? "OK" : "START", 6, 45, waveform.isEditingStart()); put(set, "SET", 55, 34);
    int x = 94;
    for (size_t i = 0; i < state.slots.size(); ++i) { put(100 + static_cast<int>(i), juce::String(i == 9 ? 0 : static_cast<int>(i) + 1), x, 22, true); x += 25; }
    put(add, "+", x, 22, state.slots.size() < 10);
    // Central musical length group stays centred, independently of slot count.
    const int centre = juce::jlimit(x + 26, getWidth() - 578, getWidth() / 2 - 93);
    put(length, "LOOP LENGTH", centre, 112, true); put(twice, "x2", centre + 116, 31); put(half, "/2", centre + 151, 31);
    x = getWidth() - 382;
    put(midiTool, "MIDI", x, 46, controlPanelType == 66); x += 50;
    put(bpmTool, "BPM", x, 42, state.stretchApplied); x += 46;
    put(segments, state.segments == 1 ? "Segments" : "Seg " + juce::StringArray({ "Off", "1/1", "1/2", "1/4", "1/8" })[state.segments - 1], x, 72, state.segments > 1); x += 76;
    put(snap, "Snap", x, 42, state.snap); x += 46;
    put(grid, divisions[state.grid - 1] + (state.triplet ? "T" : ""), x, 49); x += 53;
    put(zc, "ZC", x, 28, state.zeroCross); x += 32;
    put(play, "Loop", x, 40, processor.isLooping()); x += 44;
    put(settings, "SET", x, 27);
}
void LoopXAudioProcessorEditor::resized()
{
    waveform.setBounds(4, 28, getWidth() - 8, juce::jmax(10, getHeight() - 32));
    layoutTools(); processor.setEditorSize(getWidth(), getHeight());
    if (controlPanel)
    {
        if (compactControlPanel)
            controlPanel->setBounds(juce::jlimit(8, getWidth() - 264, getWidth() - 332), 28,
                                    juce::jmin(256, getWidth() - 16), juce::jmin(138, getHeight() - 32));
        else
            controlPanel->setBounds(getWidth()-juce::jmin(580,getWidth()-16)-8,32,
                                    juce::jmin(580,getWidth()-16),juce::jmin(420,getHeight()-40));
    }
}
void LoopXAudioProcessorEditor::paint(juce::Graphics& g)
{
    const auto palette = state.palette;
    g.fillAll(palette.toolbar); g.setFont(12.0f);
    for (const auto& tool : tools)
    {
        g.setFont(tool.id >= 100 ? 15.0f : 12.0f);
        g.setColour(tool.active ? palette.active : tool.id == hoveredTool ? palette.buttonHover : palette.button); g.fillRoundedRectangle(tool.rect.toFloat(), 2.0f);
        g.setColour(tool.active ? palette.activeBorder : palette.buttonBorder); g.drawRoundedRectangle(tool.rect.toFloat(), 2.0f, 1.0f);
        g.setColour(tool.active ? palette.activeText : palette.text); g.drawText(tool.text, tool.rect, juce::Justification::centred);
    }
}
void LoopXAudioProcessorEditor::timerCallback()
{
    const auto previous = state;
    state = processor.getViewState();
    if (previous.slots.size() != state.slots.size() || previous.loop.start != state.loop.start || previous.loop.end != state.loop.end
        || previous.grid != state.grid || previous.segments != state.segments || previous.snap != state.snap || previous.zeroCross != state.zeroCross
        || previous.palette != state.palette || previous.stretchApplied != state.stretchApplied || previous.triplet != state.triplet || lastPlaying != processor.isLooping())
    { lookAndFeel.apply(state.palette); layoutTools(); repaint(); }
    if (lastTempo != processor.getTimelineTempo()) waveform.repaint();
    lastTempo = processor.getTimelineTempo(); lastPlaying = processor.isLooping();
}
void LoopXAudioProcessorEditor::chooseFile()
{
    fileChooser = std::make_unique<juce::FileChooser>("Load sample", juce::File{}, "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    juce::Component::SafePointer<LoopXAudioProcessorEditor> safe(this);
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& chooser) { if (safe && chooser.getResult().existsAsFile()) safe->processor.requestSampleLoad(chooser.getResult()); });
}
void LoopXAudioProcessorEditor::mouseDown(const juce::MouseEvent& event)
{
    grabKeyboardFocus(); menuPosition = event.getScreenPosition();
    for (const auto& tool : tools) if (tool.rect.contains(event.getPosition())) { invoke(tool.id, event.mods.isRightButtonDown()); break; }
}
void LoopXAudioProcessorEditor::invoke(int id, bool rightClick)
{
    state = processor.getViewState();
    if (id >= 100) { if (rightClick) processor.deleteSlot(id - 100); else processor.recallSlot(id - 100); }
    else if (id == load) waveform.toggleStart();
    else if (id == bpmTool) showBpm();
    else if (id == midiTool) { if (controlPanel && controlPanelType == 66) { controlPanel.reset(); controlPanelType = 0; } else showControlPanel(66); }
    else if (id == set) waveform.applySelection();
    else if (id == add) processor.saveSlot();
    else if (id == twice) waveform.scaleLength(2.0);
    else if (id == half) waveform.scaleLength(0.5);
    else if (id == snap || id == zc)
        processor.setUiSettings(state.grid, state.segments, id == snap ? !state.snap : state.snap, state.triplet, id == zc ? !state.zeroCross : state.zeroCross);
    else if (id == play) processor.setLoopEnabled(!processor.isLooping());
    else menuFor(id);
    state = processor.getViewState(); layoutTools(); waveform.refreshFromProcessor(); repaint();
}
void LoopXAudioProcessorEditor::menuFor(int id)
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
        juce::PopupMenu playback, midi, channelLength, triggerModes, noteBehaviors, lengthControlModes, lengthChangeModes, timingModes, display, themes;
        playback.addItem(20, "MIDI Trigger", true, state.playbackMode == 0);
        playback.addItem(21, "Continuous / Host Sync", true, state.playbackMode == 1);
        midi.addItem(6, "MIDI key tracking", true, state.midiKeyTracking);
        midi.addSeparator();
        midi.addItem(30, "Velocity: Off", true, state.velocityMode == 0);
        midi.addItem(31, "Velocity -> Slot", true, state.velocityMode == 1);
        midi.addItem(32, "Velocity -> Loop Position", true, state.velocityMode == 2);
        midi.addSeparator();
        midi.addItem(60,"MIDI Note: Off",true,state.noteMode==0);
        midi.addItem(61,"MIDI Note -> Slot",true,state.noteMode==1);
        midi.addItem(62,"MIDI Note -> Loop Position",true,state.noteMode==2);
        midi.addItem(63,"Note mapping... (Slot 1: " + LoopXAudioProcessor::noteLabel(state.rootNote) + ")");
        midi.addItem(65,"Automatic MIDI note names",true,state.autoNoteNames);
        triggerModes.addItem(80,"Gate / Hold",true,state.triggerMode==0);
        triggerModes.addItem(81,"Latch / Toggle",true,state.triggerMode==1);
        triggerModes.addItem(82,"One Shot",true,state.triggerMode==2);
        midi.addSubMenu("Trigger mode",triggerModes);
        noteBehaviors.addItem(83,"Retrigger",true,state.noteBehavior==0);
        noteBehaviors.addItem(84,"Legato / No Retrigger",true,state.noteBehavior==1);
        noteBehaviors.addItem(85,"Resume",true,state.noteBehavior==2);
        noteBehaviors.addItem(86,"Restart after release",true,state.noteBehavior==3);
        midi.addSubMenu("Note behavior",noteBehaviors);
        channelLength.addItem(70, "Enabled - Note On changes Loop Length", true, state.midiChannelLength);
        channelLength.addSeparator();
        channelLength.addItem(71, "Channel 1 -> Main notes (protected)", false);
        channelLength.addItem(72, "Channel 2 -> 1 Bar", false);
        channelLength.addItem(73, "Channel 3 -> 1/2", false);
        channelLength.addItem(74, "Channel 4 -> 1/4", false);
        channelLength.addItem(75, "Channel 5 -> 1/8", false);
        channelLength.addItem(78, "Channel 6 -> 1/16", false);
        lengthControlModes.addItem(87,"Hold On / Momentary",true,state.lengthControlMode==0);
        lengthControlModes.addItem(88,"Latch",true,state.lengthControlMode==1);
        channelLength.addSubMenu("Control mode",lengthControlModes);
        lengthChangeModes.addItem(76,"Keep playback position",true,state.lengthChangeMode==0);
        lengthChangeModes.addItem(77,"Restart from loop start",true,state.lengthChangeMode==1);
        channelLength.addSubMenu("Playback cursor",lengthChangeModes);
        timingModes.addItem(92,"Immediate",true,state.triggerTiming==0);
        timingModes.addItem(93,"Next Grid",true,state.triggerTiming==1);
        timingModes.addItem(94,"Next Beat",true,state.triggerTiming==2);
        timingModes.addItem(95,"Next Bar",true,state.triggerTiming==3);
        timingModes.addItem(96,"End of Loop",true,state.triggerTiming==4);
        channelLength.addSubMenu("Trigger timing",timingModes);
        midi.addSeparator();
        midi.addSubMenu("Channel -> Loop Length" + juce::String(state.midiChannelLength ? " (On)" : " (Off)"), channelLength);
        midi.addItem(66,"Open MIDI Matrix...");
        display.addItem(2, "Stereo waveform", true, state.stereoWaveform);
        display.addItem(3, "Bright Grid", true, state.brightGrid);
        const juce::StringArray names {"Studio Dark", "Graphite", "Slate", "Warm Gray", "Studio Light"};
        for (int i = 0; i < names.size(); ++i) themes.addItem(40 + i, names[i], true, !state.customTheme && state.theme == i);
        themes.addSeparator(); int themeIndex=0;
        for (const auto& t : state.userThemes) themes.addItem(200+themeIndex++,t["name"].toString(),true,state.customTheme && state.themeName==t["name"].toString());
        themes.addItem(51,"Theme editor / Import / Export...");
        menu.addSubMenu("Playback", playback); menu.addSubMenu("MIDI", midi);
        menu.addSubMenu("Display", display); menu.addSubMenu("Themes", themes);
        menu.addSeparator(); menu.addItem(50, "Load audio file...");
        menu.addItem(64,"Loop Position (automation)...");
        menu.addSeparator();
        menu.addItem(90,"Made by Andrew Dihtiaruk",false);
        menu.addItem(91,"Support Ko-Fi");
    }
    juce::Component::SafePointer<LoopXAudioProcessorEditor> safe(this);
    menu.showMenuAsync(loopXMenuOptions(*this, menuPosition), [safe, id](int result) { if (safe && result > 0) safe->handleMenu(id, result); });
}
void LoopXAudioProcessorEditor::handleMenu(int id, int result)
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
        if (result == 51 || result == 63 || result == 64 || result == 66) showControlPanel(result);
        if (result >= 60 && result <= 62) processor.setNoteSettings(result-60,state.rootNote);
        if (result == 65) processor.setAutoNoteNamesEnabled(!state.autoNoteNames);
        if (result == 70) processor.setMidiChannelLengthEnabled(!state.midiChannelLength);
        if (result == 76 || result == 77) processor.setLengthChangeMode(result-76);
        if (result >= 80 && result <= 82) processor.setMidiTriggerMode(result-80);
        if (result >= 83 && result <= 86) processor.setMidiNoteBehavior(result-83);
        if (result == 87 || result == 88) processor.setLengthControlMode(result-87);
        if (result >= 92 && result <= 96) processor.setTriggerTiming(result-92);
        if (result == 91) juce::URL("https://ko-fi.com/pianohousestudio/shop").launchInDefaultBrowser();
        if (result >= 200 && result < 264) processor.loadUserTheme(result-200);
    }
    state = processor.getViewState(); layoutTools(); waveform.refreshFromProcessor(); repaint();
}
bool LoopXAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
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
void LoopXAudioProcessorEditor::mouseMove(const juce::MouseEvent& e)
{
    for (const auto& t : tools) if (t.rect.contains(e.getPosition()))
    { if (hoveredTool != t.id) { hoveredTool=t.id; repaint(0,0,getWidth(),28); } setMouseCursor(juce::MouseCursor::PointingHandCursor); return; }
    if (hoveredTool!=0) { hoveredTool=0; repaint(0,0,getWidth(),28); } setMouseCursor(juce::MouseCursor::NormalCursor);
}
void LoopXAudioProcessorEditor::mouseExit(const juce::MouseEvent&) { hoveredTool=0; repaint(0,0,getWidth(),28); setMouseCursor(juce::MouseCursor::NormalCursor); }

namespace {
class BpmPanel final : public juce::Component, private juce::Timer
{
public:
    explicit BpmPanel(LoopXAudioProcessor& p) : processor(p)
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
        addAndMakeVisible(done); done.setButtonText("Done"); done.onClick=[this]{setVisible(false);};
        setSize(256, 138); startTimerHz(10); timerCallback();
    }
    void resized() override
    {
        title.setBounds(12, 8, 232, 22); original.setBounds(12, 32, 232, 24);
        target.setBounds(12, 60, getWidth()-24, 24); match.setBounds(12, 94, getWidth()-108, 28); done.setBounds(getWidth()-84,94,72,28);
    }
    void paint(juce::Graphics& g) override { g.fillAll(processor.getViewState().palette.toolbar); }
private:
    void timerCallback() override
    {
        target.setText("Project: " + juce::String(processor.getProjectTempo(), 2) + " BPM" +
            (processor.isLoading() ? " (rendering)" : ""), juce::dontSendNotification);
    }
    LoopXAudioProcessor& processor;
    juce::TextEditor original; juce::Label target, title; juce::TextButton match,done;
};

class NoteMappingPanel final : public juce::Component
{
public:
    explicit NoteMappingPanel(LoopXAudioProcessor& p) : processor(p)
    {
        for (auto* c : std::initializer_list<juce::Component*>{&mode,&root,&description,&done}) addAndMakeVisible(c);
        mode.addItem("Notes: Off",1); mode.addItem("Note -> Slot",2); mode.addItem("Note -> Loop Position",3);
        const auto s=p.getViewState(); mode.setSelectedId(s.noteMode+1,juce::dontSendNotification);
        root.setRange(0,118,1); root.setValue(s.rootNote,juce::dontSendNotification); root.setTextBoxStyle(juce::Slider::TextBoxRight,false,60,22);
        root.setTooltip("Root MIDI note for Slot 1. Next nine semitones = Slots 2-10. C3 = MIDI 60 in LoopX labels.");
        mode.onChange=[this] { apply(); };
        root.onValueChange=[this]{apply();};
        done.setButtonText("Done"); done.onClick=[this]{setVisible(false);}; updateDescription();
    }
    void paint(juce::Graphics& g) override
    {
        const auto p=processor.getViewState().palette; g.fillAll(p.toolbar); g.setColour(p.text); g.setFont(12);
        g.drawText("Slot 1 note",12,42,90,24,juce::Justification::centredLeft);
    }
    void resized() override
    {
        mode.setBounds(12,10,getWidth()-100,24); done.setBounds(getWidth()-80,10,68,24);
        root.setBounds(110,42,getWidth()-122,24);
        description.setBounds(12,76,getWidth()-24,getHeight()-86);
    }
private:
    void apply() { processor.setNoteSettings(mode.getSelectedId()-1,int(root.getValue())); updateDescription(); repaint(); }
    void updateDescription()
    {
        const auto s=processor.getViewState(); juce::String text;
        root.setEnabled(s.noteMode!=2);
        if (s.noteMode==2) text="128 notes = 128 divisions. MIDI 0 -> Grid 1, MIDI 1 -> Grid 2, ... MIDI 127 -> Grid 128.\nLoop length and pitch stay unchanged. No banks. Position is clamped at the sample end.\nNote names are exported to supported DAWs; octave labels differ between hosts.";
        else
        {
            const int low[] {1,14,27,40,52,65,78,90,103,116}, high[] {13,26,39,51,64,77,89,102,115,127};
            for (int i=0;i<10;++i) text+="Slot "+juce::String(i+1)+" : "+(s.rootNote+i<=127 ? LoopXAudioProcessor::noteLabel(s.rootNote+i) : "unmapped")+
                "    velocity "+juce::String(low[i])+"-"+juce::String(high[i])+"\n";
        }
        description.setText(text,juce::dontSendNotification);
    }
    LoopXAudioProcessor& processor; juce::ComboBox mode; juce::Slider root; juce::Label description; juce::TextButton done;
};
class MidiPerformancePanel final : public juce::Component, private juce::Timer
{
public:
    explicit MidiPerformancePanel(LoopXAudioProcessor& p) : processor(p)
    {
        for (auto* c : std::initializer_list<juce::Component*>{&title,&trigger,&behavior,&lengthMode,&cursorMode,&timing,&preset,&lengthEnabled,&autoNames,&mapping,&activity,&reset,&done}) addAndMakeVisible(c);
        title.setText("MIDI PERFORMANCE MATRIX",juce::dontSendNotification);
        trigger.addItemList({"Gate / Hold","Latch / Toggle","One Shot"},1);
        behavior.addItemList({"Retrigger","Legato / No Retrigger","Resume","Restart after release"},1);
        lengthMode.addItemList({"Hold On / Momentary","Latch"},1);
        cursorMode.addItemList({"Keep playback position","Restart from loop start"},1);
        timing.addItemList({"Immediate","Next Grid","Next Beat","Next Bar","End of Loop"},1);
        preset.addItemList({"Preset: Default","Preset: Live Play","Preset: DAW Quantized"},1);
        lengthEnabled.setButtonText("Enable Channel 2-6 Loop Length control"); autoNames.setButtonText("Automatic MIDI note names");
        mapping.setJustificationType(juce::Justification::topLeft);
        mapping.setText(R"(ROUTING - Conflict check: OK
Main Notes: Ch 1 (protected)
Loop Length: Ch 2 = 1 Bar, Ch 3 = 1/2, Ch 4 = 1/4, Ch 5 = 1/8, Ch 6 = 1/16
Ch 7-16 remain available for normal notes and future mapping.
Hold On restores the previous length; overlapping controls use the last held note.)",juce::dontSendNotification);
        reset.setButtonText("Reset Mapping"); done.setButtonText("Done");
        trigger.onChange=[this]{if(!syncing)processor.setMidiTriggerMode(trigger.getSelectedId()-1);};
        behavior.onChange=[this]{if(!syncing)processor.setMidiNoteBehavior(behavior.getSelectedId()-1);};
        lengthMode.onChange=[this]{if(!syncing)processor.setLengthControlMode(lengthMode.getSelectedId()-1);};
        cursorMode.onChange=[this]{if(!syncing)processor.setLengthChangeMode(cursorMode.getSelectedId()-1);};
        timing.onChange=[this]{if(!syncing)processor.setTriggerTiming(timing.getSelectedId()-1);};
        lengthEnabled.onClick=[this]{if(!syncing)processor.setMidiChannelLengthEnabled(lengthEnabled.getToggleState());};
        autoNames.onClick=[this]{if(!syncing)processor.setAutoNoteNamesEnabled(autoNames.getToggleState());};
        reset.onClick=[this]{processor.resetMidiMapping();sync();}; done.onClick=[this]{setVisible(false);};
        preset.onChange=[this]{if(syncing)return; if(preset.getSelectedId()==1)processor.resetMidiMapping(); if(preset.getSelectedId()==2){processor.setMidiChannelLengthEnabled(true);processor.setMidiNoteBehavior(1);processor.setLengthControlMode(0);processor.setLengthChangeMode(0);processor.setTriggerTiming(0);} if(preset.getSelectedId()==3){processor.setMidiChannelLengthEnabled(true);processor.setMidiNoteBehavior(0);processor.setLengthControlMode(1);processor.setLengthChangeMode(1);processor.setTriggerTiming(3);} sync();};
        sync(); startTimerHz(20);
    }
    void paint(juce::Graphics& g) override { const auto p=processor.getViewState().palette;g.fillAll(p.toolbar);g.setColour(p.buttonBorder);g.drawRect(getLocalBounds());g.setColour(flash>0?p.active:p.buttonBorder);g.fillEllipse(float(getWidth()-28),12,10,10); }
    void resized() override { title.setBounds(12,8,300,24);done.setBounds(getWidth()-80,8,68,24);juce::ComboBox* b[]{&trigger,&behavior,&lengthMode,&cursorMode,&timing,&preset};for(int i=0;i<6;++i)b[i]->setBounds(154,42+i*31,getWidth()-166,24);lengthEnabled.setBounds(12,232,280,24);autoNames.setBounds(300,232,250,24);mapping.setBounds(12,264,getWidth()-24,105);activity.setBounds(12,374,330,22);reset.setBounds(getWidth()-120,370,108,28); }
private:
    void sync(){const auto s=processor.getViewState();syncing=true;trigger.setSelectedId(s.triggerMode+1,juce::dontSendNotification);behavior.setSelectedId(s.noteBehavior+1,juce::dontSendNotification);lengthMode.setSelectedId(s.lengthControlMode+1,juce::dontSendNotification);cursorMode.setSelectedId(s.lengthChangeMode+1,juce::dontSendNotification);timing.setSelectedId(s.triggerTiming+1,juce::dontSendNotification);lengthEnabled.setToggleState(s.midiChannelLength,juce::dontSendNotification);autoNames.setToggleState(s.autoNoteNames,juce::dontSendNotification);preset.setSelectedId(0,juce::dontSendNotification);syncing=false;}
    void timerCallback() override {const auto c=processor.getMidiActivityCounter();if(c!=counter){counter=c;flash=5;}if(flash>0)--flash;juce::String s=processor.getLastMidiChannel()>0?"MIDI Ch "+juce::String(processor.getLastMidiChannel()):"MIDI idle";const int d=processor.getActiveLengthDivision();if(d>0)s+=" | "+juce::StringArray({"1 Bar","1/2","1/4","1/8","1/16"})[d-1];activity.setText(s,juce::dontSendNotification);sync();repaint();}
    LoopXAudioProcessor& processor;juce::Label title,mapping,activity;juce::ComboBox trigger,behavior,lengthMode,cursorMode,timing,preset;juce::ToggleButton lengthEnabled,autoNames;juce::TextButton reset,done;bool syncing=false;unsigned counter=0;int flash=0;
};
class PositionPanel final : public juce::Component, private juce::Timer
{
public:
    explicit PositionPanel(LoopXAudioProcessor& p) : processor(p)
    {
        addAndMakeVisible(position); addAndMakeVisible(label); addAndMakeVisible(done);
        position.setRange(0,100,0.01); position.setTextValueSuffix(" %"); position.setTextBoxStyle(juce::Slider::TextBoxRight,false,90,24);
        position.setValue(p.positionParameter->get()*100,juce::dontSendNotification);
        position.onDragStart=[this]{gesture=true; processor.positionParameter->beginChangeGesture();};
        position.onDragEnd=[this]{processor.positionParameter->endChangeGesture(); gesture=false;};
        position.onValueChange=[this]
        {
            if (!gesture) processor.positionParameter->beginChangeGesture();
            processor.positionParameter->setValueNotifyingHost(float(position.getValue()/100));
            if (!gesture) processor.positionParameter->endChangeGesture();
        };
        label.setText("Loop Position - automate this VST3 parameter in your DAW.\n0% = beginning; 100% = last valid start. Snap follows Grid.",juce::dontSendNotification);
        done.setButtonText("Done"); done.onClick=[this]{setVisible(false);}; startTimerHz(15);
    }
    ~PositionPanel() override { stopTimer(); if (gesture) processor.positionParameter->endChangeGesture(); }
    void paint(juce::Graphics& g) override { g.fillAll(processor.getViewState().palette.toolbar); }
    void resized() override { label.setBounds(12,8,getWidth()-24,60); position.setBounds(12,76,getWidth()-24,32); done.setBounds(getWidth()-84,120,72,24); }
private:
    void timerCallback() override { if (!gesture) position.setValue(processor.positionParameter->get()*100,juce::dontSendNotification); }
    LoopXAudioProcessor& processor; juce::Slider position; juce::Label label; juce::TextButton done; bool gesture=false;
};
}
void LoopXAudioProcessorEditor::showBpm()
{
    compactControlPanel = true; controlPanelType = bpmTool; controlPanel = std::make_unique<BpmPanel>(processor); addAndMakeVisible(*controlPanel); resized(); controlPanel->toFront(true);
}
void LoopXAudioProcessorEditor::showControlPanel(int type)
{
    compactControlPanel = false; controlPanelType = type;
    if (type==51) controlPanel=std::make_unique<LoopXThemeEditor>(processor);
    else if (type==63) controlPanel=std::make_unique<NoteMappingPanel>(processor);
    else if (type==66) controlPanel=std::make_unique<MidiPerformancePanel>(processor);
    else controlPanel=std::make_unique<PositionPanel>(processor);
    addAndMakeVisible(*controlPanel); resized(); controlPanel->toFront(true);
}
