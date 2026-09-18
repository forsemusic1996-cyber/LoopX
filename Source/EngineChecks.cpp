#include "LoopPlayback.h"
#include "StretchKernel.h"
#include <vector>
#include <iostream>
#include <stdexcept>

int main()
{
    try
    {
        constexpr double rate = 48000;
        std::vector<float> source(96000);
        for (int i = 0; i < int(source.size()); ++i)
            source[size_t(i)] = float(std::sin(i * 6.283185307179586 * 440 / rate) * 0.3);
        const float* channels[] {source.data(), source.data()};
        for (double ratio : {0.75, 1.0, 1.24, 1.4})
        {
            LoopPlayback engine; engine.prepare(rate);
            int first = -1, last = -1, crossings = 0;
            float previous = 0;
            for (int i = 0; i < 96000; ++i)
            {
                const double phase = std::fmod(i / rate * ratio / 2, 1.0);
                const float sample = engine.next(channels, 2, int(source.size()), 0, source.size(), phase, 1, rate)[0];
                if (!std::isfinite(sample) || std::abs(sample) > 0.5) throw std::runtime_error("invalid engine output");
                if (i > 12000 && previous <= 0 && sample > 0)
                { if (first < 0) first = i; last = i; ++crossings; }
                previous = sample;
            }
            const double hz = (crossings - 1) * rate / (last - first);
            if (std::abs(hz - 440) > 4) throw std::runtime_error("host tempo changed pitch");
            std::cout << "PASS realtime pitch: tempo ratio " << ratio << ", " << hz << " Hz\n";
        }
        const int count = int(std::llround(source.size() * 100.0 / 124));
        std::vector<float> left(size_t(count), 0), right(size_t(count), 0);
        float* output[] {left.data(), right.data()};
        if (!stretchAudio(channels, 2, int(source.size()), rate, 0, count, output, 100.0 / 124, [] {return false;}))
            throw std::runtime_error("stretch render cancelled");
        int first = -1, last = -1, crossings = 0;
        for (int i = 7201; i < 40000; ++i)
        {
            if (!std::isfinite(left[size_t(i)]) || std::abs(left[size_t(i)] - right[size_t(i)]) > 0.01)
                throw std::runtime_error("stretch stereo coherence failed");
            if (left[size_t(i-1)] <= 0 && left[size_t(i)] > 0)
            {if (first < 0) first = i; last = i; ++crossings;}
        }
        const double hz = (crossings - 1) * rate / (last - first);
        if (std::abs(hz - 440) > 3) throw std::runtime_error("Signalsmith pitch failed");
        std::cout << "PASS Signalsmith 100->124 BPM pitch: " << hz << " Hz\\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
