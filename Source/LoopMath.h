#pragma once
#include <algorithm>
#include <cmath>
namespace LoopMath
{
inline double wrap(double value, double length)
{
    if (!std::isfinite(value) || !std::isfinite(length) || length <= 0.0) return 0.0;
    return value - std::floor(value / length) * length;
}
inline double divisionBeats(int id, int numerator = 4, int denominator = 4)
{
    switch (id)
    {
        case 1: return std::max(1, numerator) * 4.0 / std::max(1, denominator);
        case 2: return 2.0;
        case 3: return 1.0;
        case 4: return 0.5;
        case 5: return 0.25;
        default: return 0.125;
    }
}
inline double phase(double ppq, double beats) { return wrap(ppq, beats) / std::max(1.0e-9, beats); }
inline double scaledEnd(double start, double end, double duration, double factor)
{
    return std::min(duration, start + std::max(0.001, (end - start) * factor));
}
}
