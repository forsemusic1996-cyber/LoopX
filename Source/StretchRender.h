#pragma once
#include "PluginProcessor.h"
#include "StretchKernel.h"
#include <stdexcept>

inline std::shared_ptr<LoopXSample> renderBpmSample(const LoopXSample& original,
                                                        double offset, double ratio,
                                                        const std::function<bool()>& cancelled)
{
    const int first = juce::jlimit(0, original.audio.getNumSamples() - 2, int(std::llround(offset * original.rate)));
    const double wanted = std::round((original.audio.getNumSamples() - first) * ratio);
    if (!std::isfinite(wanted) || wanted < 2 || wanted > 20000000) throw std::runtime_error("Stretched sample is too large");
    auto result = std::make_shared<LoopXSample>(); result->rate = original.rate; result->file = original.file;
    result->audio.setSize(original.audio.getNumChannels(), int(wanted)); result->audio.clear();
    if (!stretchAudio(original.audio.getArrayOfReadPointers(), original.audio.getNumChannels(),
                      original.audio.getNumSamples(), original.rate, first, int(wanted),
                      result->audio.getArrayOfWritePointers(), ratio, cancelled)) return {};
    if (!result->buildWaveform(cancelled)) return {};
    return result;
}
