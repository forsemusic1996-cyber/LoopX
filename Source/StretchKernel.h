#pragma once
#include "signalsmith-stretch.h"
#include <algorithm>
#include <vector>
#include <functional>

// Latency-compensated fixed-length render. Called only by the worker.
inline bool stretchAudio(const float* const* source, int channels, int total, double rate,
                         int first, int outputCount, float* const* output,
                         double ratio, const std::function<bool()>& cancelled)
{
    signalsmith::stretch::SignalsmithStretch<float> stretch(0);
    stretch.presetDefault(channels, float(rate)); stretch.setTransposeFactor(1.0f); stretch.reset();
    const int inputCount = total - first;
    const int inputLatency = stretch.inputLatency(), outputLatency = stretch.outputLatency();
    struct Input
    {
        const float* const* data; int base, count;
        struct Channel { const float* data; int base, count;
            float operator[](int i) const { return base + i >= 0 && base + i < count ? data[base + i] : 0.0f; } };
        Channel operator[](int ch) const { return {data[ch], base, count}; }
    };
    stretch.seek(Input{source, first, total}, inputLatency, 1.0 / ratio);
    const int scratchSize = std::max(4096, outputLatency);
    std::vector<std::vector<float>> storage(size_t(channels), std::vector<float>(size_t(scratchSize), 0));
    std::vector<float*> scratch;
    for (auto& channel : storage) scratch.push_back(channel.data());
    int inputDone = 0, outputDone = 0;
    while (outputDone < outputCount)
    {
        if (cancelled()) return false;
        const int n = std::min(4096, outputCount - outputDone);
        const int nextInput = int(std::llround(double(outputDone + n) * inputCount / outputCount));
        stretch.process(Input{source, first + inputLatency + inputDone, total}, nextInput - inputDone, scratch.data(), n);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < n; ++i)
            {
                const int destination = outputDone + i - outputLatency;
                if (destination >= 0 && destination < outputCount) output[ch][destination] = scratch[size_t(ch)][i];
            }
        inputDone = nextInput; outputDone += n;
    }
    stretch.flush(scratch.data(), outputLatency);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < outputLatency; ++i)
        {
            const int destination = outputCount - outputLatency + i;
            if (destination >= 0 && destination < outputCount) output[ch][destination] = scratch[size_t(ch)][i];
        }
    return !cancelled();
}
