#pragma once
#include <JuceHeader.h>
struct LoopXPalette
{
    juce::Colour background, toolbar, button, active, accent, wave, wave2, segment, text;
};
inline LoopXPalette loopXPalette(int theme)
{
    switch (theme)
    {
        case 1: return { juce::Colour(0xff141414), juce::Colour(0xff282828), juce::Colour(0xff333333), juce::Colour(0xff414141), juce::Colour(0xffc0c7cd), juce::Colour(0xff5e6367), juce::Colour(0xff73787c), juce::Colour(0xffa0a7ad), juce::Colour(0xffe1e7ed) };
        case 2: return { juce::Colour(0xff191412), juce::Colour(0xff302521), juce::Colour(0xff3a2c26), juce::Colour(0xff633e33), juce::Colour(0xffe07a5f), juce::Colour(0xff725c53), juce::Colour(0xff8b7062), juce::Colour(0xffd6a18d), juce::Colour(0xfff1e4de) };
        case 3: return { juce::Colour(0xff15121b), juce::Colour(0xff292332), juce::Colour(0xff342b40), juce::Colour(0xff4a365f), juce::Colour(0xffb49cda), juce::Colour(0xff64566f), juce::Colour(0xff7a6987), juce::Colour(0xffa591bc), juce::Colour(0xffeae1f3) };
        case 4: return { juce::Colour(0xff19170f), juce::Colour(0xff302c20), juce::Colour(0xff3c3625), juce::Colour(0xff5d4d26), juce::Colour(0xffd6b667), juce::Colour(0xff70654a), juce::Colour(0xff897958), juce::Colour(0xffc2ad78), juce::Colour(0xffeee8d7) };
        default: return { juce::Colour(0xff101315), juce::Colour(0xff202426), juce::Colour(0xff292e30), juce::Colour(0xff23474a), juce::Colour(0xff66cecd), juce::Colour(0xff49656b), juce::Colour(0xff627279), juce::Colour(0xff89b8b7), juce::Colour(0xffe1e7ed) };
    }
}
