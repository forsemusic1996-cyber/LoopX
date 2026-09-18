#pragma once
#include <array>
#include <cmath>
#include <algorithm>

// Streaming WSOLA: musical position advances at host tempo, while each grain
// reads source audio at its original rate. No allocations or look-ahead delay.
// This is a modest-ratio loop stretcher, not an elastique-quality replacement.
class LoopPlayback
{
public:
    void prepare(double outputRate) { hop = std::max(32, int(outputRate * 0.02)); clear(); }
    void clear() { grains = {}; last = {}; transition = 0; ready = false; }
    void reset(double target, double sourceStep)
    {
        from = last; transition = 96;
        grains[0] = { target - hop * sourceStep, hop };
        grains[1] = { target, 0 }; ready = true;
    }
    std::array<float, 2> next(const float* const* source, int channels, int count,
                              double start, double end, double phase, double sourceStep,
                              double sourceRate)
    {
        const double target = start + phase * (end - start);
        if (!ready) reset(target, sourceStep);
        for (int g = 0; g < 2; ++g)
            if (grains[g].age >= hop * 2)
            {
                const auto& other = grains[1 - g];
                const double reference = other.anchor + other.age * sourceStep;
                double best = target, score = -2;
                const int radius = int(sourceRate * 0.008);
                const int stride = std::max(1, int(sourceRate * 0.00025));
                const auto correlation = [&](double candidate)
                {
                    double dot = 0, a2 = 1.0e-12, b2 = 1.0e-12;
                    for (int i = 0; i < 32; ++i)
                    {
                        const double offset = i * sourceRate * 0.0002;
                        const float a = read(source[0], count, reference + offset, start, end);
                        const float b = read(source[0], count, candidate + offset, start, end);
                        dot += a * b; a2 += a * a; b2 += b * b;
                    }
                    // Gentle distance bias avoids wandering on flat/silent audio.
                    return dot / std::sqrt(a2 * b2) - 0.002 * std::abs(candidate - target) / std::max(1, radius);
                };
                for (int offset = -radius; offset <= radius; offset += stride)
                {
                    const double value = correlation(target + offset);
                    if (value > score) { score = value; best = target + offset; }
                }
                const double coarse = best;
                for (int offset = -stride; offset <= stride; ++offset)
                {
                    const double value = correlation(coarse + offset);
                    if (value > score) { score = value; best = coarse + offset; }
                }
                grains[g] = { best, 0 };
            }
        std::array<float, 2> output {};
        for (auto& grain : grains)
        {
            const float window = float(0.5 - 0.5 * std::cos(3.14159265358979323846 * grain.age / hop));
            for (int ch = 0; ch < 2; ++ch)
                output[ch] += window * read(source[std::min(ch, channels - 1)], count,
                    grain.anchor + grain.age * sourceStep, start, end);
            ++grain.age;
        }
        if (transition > 0)
        {
            const float mix = float(97 - transition) / 96.0f;
            for (int ch = 0; ch < 2; ++ch) output[ch] = from[ch] + mix * (output[ch] - from[ch]);
            --transition;
        }
        last = output; return output;
    }
private:
    static float read(const float* data, int count, double position, double start, double end)
    {
        const double length = std::max(1.0, end - start);
        double offset = std::fmod(position - start, length); if (offset < 0) offset += length;
        const double p = start + offset;
        const int a = std::clamp(int(p), 0, count - 1);
        const double nextPosition = p + 1 >= end ? start : p + 1;
        const int b = std::clamp(int(nextPosition), 0, count - 1);
        return data[a] + float(p - a) * (data[b] - data[a]);
    }
    struct Grain { double anchor = 0; int age = 0; };
    std::array<Grain, 2> grains {};
    std::array<float, 2> last {}, from {};
    int hop = 960, transition = 0;
    bool ready = false;
};
