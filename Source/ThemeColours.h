#pragma once
#include <JuceHeader.h>
#include <array>
struct LoopXPalette
{
    juce::Colour background, toolbar, button, active, accent, wave, wave2, segment, text;
    juce::Colour buttonHover, buttonBorder, activeBorder, activeText;
    juce::Colour grid, gridBar, gridBright, gridBarBright, gridText;
    juce::Colour loopFill, loopBorder, loopMarker, loopMarkerBorder, selection;
    juce::Colour slot, slotText, cursor, scrollTrack, scrollThumb, scrollActive;
    juce::Colour fade, start, dropFill, dropBorder;
};
struct LoopXColourField { const char* key; const char* label; juce::Colour LoopXPalette::* member; };
inline const std::array<LoopXColourField, 33>& loopXColourFields()
{
    static const std::array<LoopXColourField, 33> fields {{
        {"background","Background",&LoopXPalette::background}, {"toolbar","Toolbar",&LoopXPalette::toolbar},
        {"button","Button / normal",&LoopXPalette::button}, {"active","Button / active",&LoopXPalette::active},
        {"accent","Accent",&LoopXPalette::accent}, {"wave","Waveform / left",&LoopXPalette::wave},
        {"wave2","Waveform / right",&LoopXPalette::wave2}, {"segment","Segments",&LoopXPalette::segment},
        {"text","Text",&LoopXPalette::text}, {"buttonHover","Button / hover",&LoopXPalette::buttonHover},
        {"buttonBorder","Button border",&LoopXPalette::buttonBorder}, {"activeBorder","Active button border",&LoopXPalette::activeBorder},
        {"activeText","Active button text",&LoopXPalette::activeText}, {"grid","Grid",&LoopXPalette::grid},
        {"gridBar","Grid / bar",&LoopXPalette::gridBar}, {"gridBright","Bright grid",&LoopXPalette::gridBright},
        {"gridBarBright","Bright grid / bar",&LoopXPalette::gridBarBright}, {"gridText","Grid numbers",&LoopXPalette::gridText},
        {"loopFill","Loop / fill",&LoopXPalette::loopFill}, {"loopBorder","Loop / boundaries",&LoopXPalette::loopBorder},
        {"loopMarker","Loop / bottom handle",&LoopXPalette::loopMarker}, {"loopMarkerBorder","Loop / handle border",&LoopXPalette::loopMarkerBorder},
        {"selection","Selection / fill",&LoopXPalette::selection}, {"slot","Slot markers",&LoopXPalette::slot},
        {"slotText","Slot labels",&LoopXPalette::slotText}, {"cursor","Playback cursor",&LoopXPalette::cursor},
        {"scrollTrack","Scrollbar / track",&LoopXPalette::scrollTrack}, {"scrollThumb","Scrollbar / thumb",&LoopXPalette::scrollThumb},
        {"scrollActive","Scrollbar / drag",&LoopXPalette::scrollActive}, {"fade","Fade lines and handles",&LoopXPalette::fade},
        {"start","START marker",&LoopXPalette::start}, {"dropFill","Drag-drop / fill",&LoopXPalette::dropFill},
        {"dropBorder","Drag-drop / border",&LoopXPalette::dropBorder}
    }};
    return fields;
}
inline bool operator==(const LoopXPalette& a, const LoopXPalette& b)
{
    for (auto& f : loopXColourFields()) if (a.*(f.member) != b.*(f.member)) return false;
    return true;
}
inline bool operator!=(const LoopXPalette& a, const LoopXPalette& b) { return !(a == b); }
inline LoopXPalette loopXPalette(int theme)
{
    LoopXPalette p;
    p.background = juce::Colour(0xff101315); p.toolbar = juce::Colour(0xff202426);
    p.button = juce::Colour(0xff292e30); p.active = juce::Colour(0xff23474a);
    p.accent = juce::Colour(0xff66cecd); p.wave = juce::Colour(0xff49656b); p.wave2 = juce::Colour(0xff627279);
    p.segment = juce::Colour(0xff648587); p.text = juce::Colour(0xffe1e7ed);
    p.grid = juce::Colour(0xff252d30); p.gridBar = juce::Colour(0xff375b5e);
    p.gridBright = juce::Colour(0xff343f42); p.gridBarBright = juce::Colour(0xff519b9d);
    p.gridText = juce::Colour(0xff8b989a); p.loopFill = juce::Colour(0x604b8585);
    p.selection = juce::Colour(0x90548787); p.loopMarker = juce::Colour(0xff426f74);
    p.scrollTrack = juce::Colour(0xff303234); p.scrollThumb = juce::Colour(0xff484a4c); p.scrollActive = juce::Colour(0xff686a6c);
    if (theme == 1 || theme == 2 || theme == 3)
    {
        const juce::Colour base(theme == 1 ? 0xff161616 : theme == 2 ? 0xff171d23 : 0xff201e1a);
        p.background = base; p.toolbar = base.brighter(0.10f); p.button = base.brighter(0.16f);
        p.active = base.brighter(0.24f); p.wave = juce::Colour(theme == 1 ? 0xff64696c : theme == 2 ? 0xff596978 : 0xff746c5f);
        p.wave2 = p.wave.brighter(0.12f); p.segment = p.wave.brighter(0.24f);
        p.grid = base.brighter(0.13f); p.gridBar = base.brighter(0.24f);
        p.gridBright = base.brighter(0.23f); p.gridBarBright = base.brighter(0.40f);
        p.gridText = p.wave.brighter(0.35f); p.loopFill = p.accent.withAlpha(0.18f);
        p.selection = juce::Colour(0xffa3b0b8).withAlpha(0.26f);
        p.loopMarker = p.accent.darker(0.55f); p.scrollTrack = base.brighter(0.12f);
        p.scrollThumb = base.brighter(0.24f); p.scrollActive = base.brighter(0.40f);
    }
    if (theme == 4)
    {
        p.background = juce::Colour(0xffe7e9eb); p.toolbar = juce::Colour(0xffd2d6da);
        p.button = juce::Colour(0xffe2e5e7); p.active = juce::Colour(0xffc0d7d8); p.text = juce::Colour(0xff20282d);
        p.accent = juce::Colour(0xff147d80); p.wave = juce::Colour(0xff7a878d); p.wave2 = juce::Colour(0xff64757c);
        p.segment = juce::Colour(0xff87979d); p.grid = juce::Colour(0xffd2d7db); p.gridBar = juce::Colour(0xffb6c2c7);
        p.gridBright = juce::Colour(0xffb0bec4); p.gridBarBright = juce::Colour(0xff7c959e); p.gridText = juce::Colour(0xff56666e);
        p.loopFill = p.accent.withAlpha(0.18f); p.selection = juce::Colour(0xff687f96).withAlpha(0.24f);
        p.loopMarker = juce::Colour(0xffa0c9ca); p.scrollTrack = juce::Colour(0xffcbd0d4);
        p.scrollThumb = juce::Colour(0xffa1a9af); p.scrollActive = juce::Colour(0xff7f898f);
    }
    p.buttonHover = p.button.brighter(0.12f); p.buttonBorder = p.text.withAlpha(0.30f);
    p.activeBorder = p.accent; p.activeText = p.text;
    p.loopBorder = p.accent; p.loopMarkerBorder = p.accent;
    p.slot = juce::Colour(0xffe07a5f); p.slotText = p.slot; p.cursor = p.accent;
    p.fade = p.accent.withAlpha(0.80f); p.start = p.accent;
    p.dropFill = p.accent.withAlpha(0.28f); p.dropBorder = juce::Colour(0xff66ddc9);
    return p;
}
inline juce::var loopXThemeJson(const LoopXPalette& p, juce::String name)
{
    auto* colours = new juce::DynamicObject;
    for (auto& f : loopXColourFields()) colours->setProperty(f.key, "#" + (p.*(f.member)).toDisplayString(true));
    auto* root = new juce::DynamicObject;
    root->setProperty("format", "LoopXTheme"); root->setProperty("version", 1);
    root->setProperty("name", name.trim().substring(0, 80)); root->setProperty("colours", juce::var(colours));
    return juce::var(root);
}
inline bool loopXReadTheme(const juce::var& json, LoopXPalette& result, juce::String& name)
{
    if (json["format"].toString() != "LoopXTheme" || int(json["version"]) != 1 || !json["colours"].isObject()) return false;
    auto palette = loopXPalette(0);
    for (auto& f : loopXColourFields())
    {
        const auto value = json["colours"][f.key].toString();
        if (value.length() != 9 || !value.startsWithChar('#') || !value.substring(1).containsOnly("0123456789abcdefABCDEF")) return false;
        palette.*(f.member) = juce::Colour::fromString(value.substring(1));
    }
    result = palette; name = json["name"].toString().trim().substring(0, 80);
    if (name.isEmpty()) name = "Custom";
    return true;
}
