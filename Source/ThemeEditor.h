#pragma once
#include "PluginProcessor.h"

class LoopXLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    juce::PopupMenu::Options getOptionsForComboBoxPopupMenu(juce::ComboBox& box, juce::Label&) override
    {
        auto options = juce::PopupMenu::Options().withTargetComponent(&box).withStandardItemHeight(22);
        if (auto* editor = box.findParentComponentOfClass<juce::AudioProcessorEditor>()) options = options.withParentComponent(editor);
        return options;
    }
    void apply(const LoopXPalette& p)
    {
        setColour(juce::PopupMenu::backgroundColourId,p.toolbar); setColour(juce::PopupMenu::textColourId,p.text);
        setColour(juce::PopupMenu::highlightedBackgroundColourId,p.active); setColour(juce::PopupMenu::highlightedTextColourId,p.activeText);
        setColour(juce::TextButton::buttonColourId,p.button); setColour(juce::TextButton::buttonOnColourId,p.active);
        setColour(juce::TextButton::textColourOffId,p.text); setColour(juce::TextButton::textColourOnId,p.activeText);
        setColour(juce::Label::textColourId,p.text); setColour(juce::ComboBox::backgroundColourId,p.button);
        setColour(juce::ComboBox::textColourId,p.text); setColour(juce::ComboBox::outlineColourId,p.buttonBorder);
        setColour(juce::ComboBox::arrowColourId,p.text); setColour(juce::TextEditor::backgroundColourId,p.background);
        setColour(juce::TextEditor::textColourId,p.text); setColour(juce::TextEditor::outlineColourId,p.buttonBorder);
        setColour(juce::TextEditor::focusedOutlineColourId,p.accent);
        setColour(juce::Slider::thumbColourId,p.accent); setColour(juce::Slider::trackColourId,p.active);
        setColour(juce::Slider::textBoxTextColourId,p.text); setColour(juce::Slider::textBoxBackgroundColourId,p.button);
        setColour(juce::Slider::textBoxOutlineColourId,p.buttonBorder);
        setColour(juce::ColourSelector::backgroundColourId,p.toolbar); setColour(juce::ColourSelector::labelTextColourId,p.text);
    }
};

// Owned by the plugin editor (not a deferred CallOutBox that can outlive the
// processor). Async file callbacks keep only a component SafePointer.
class LoopXThemeEditor final : public juce::Component, private juce::ChangeListener
{
public:
    explicit LoopXThemeEditor(LoopXAudioProcessor& p) : processor(p), initial(p.getViewState()),
        selector(juce::ColourSelector::showAlphaChannel | juce::ColourSelector::showColourAtTop |
                 juce::ColourSelector::editableColour | juce::ColourSelector::showSliders | juce::ColourSelector::showColourspace)
    {
        setName("Theme editor"); setOpaque(true);
        for (auto* c : std::initializer_list<juce::Component*>{&name,&saved,&base,&element,&selector,&save,&create,&rename,&reset,&cancel,&close,&import,&exportButton,&message}) addAndMakeVisible(c);
        name.setText(initial.themeName); name.setTooltip("Theme name; Save creates/updates a user theme, never a built-in theme.");
        name.onTextChange = [this] { processor.setCustomTheme(processor.getViewState().palette,name.getText()); };
        const juce::StringArray bases {"Studio Dark","Graphite","Slate","Warm Gray","Studio Light"};
        for (int i = 0; i < bases.size(); ++i) base.addItem(bases[i],i+1);
        base.setSelectedId(initial.theme+1,juce::dontSendNotification); base.setTooltip("Base theme for Reset");
        int id = 1; for (auto& f : loopXColourFields()) element.addItem(f.label,id++);
        element.setSelectedId(19,juce::dontSendNotification); element.onChange = [this] { selectColour(); };
        selector.addChangeListener(this); selectColour(); refreshSaved();
        saved.setTextWhenNothingSelected("Saved user themes"); saved.onChange = [this]
        { if (processor.loadUserTheme(saved.getSelectedId()-1)) { name.setText(processor.getViewState().themeName,false); selectColour(); repaint(); } };
        save.setButtonText("Save"); create.setButtonText("New"); rename.setButtonText("Rename"); reset.setButtonText("Reset base");
        cancel.setButtonText("Cancel"); close.setButtonText("Done"); import.setButtonText("Import"); exportButton.setButtonText("Export");
        save.onClick = [this] { processor.saveUserTheme(name.getText()); refreshSaved(); message.setText("User theme saved",juce::dontSendNotification); };
        create.onClick = [this] { name.setText("Untitled",false); processor.setCustomTheme(loopXPalette(base.getSelectedId()-1),"Untitled"); saved.setSelectedId(0,juce::dontSendNotification); selectColour(); repaint(); };
        rename.onClick = [this]
        {
            if (processor.renameUserTheme(saved.getSelectedId()-1,name.getText())) { refreshSaved(); message.setText("Renamed",juce::dontSendNotification); }
            else message.setText("Select a saved theme / use a unique name",juce::dontSendNotification);
        };
        reset.onClick = [this] { processor.setCustomTheme(loopXPalette(base.getSelectedId()-1),name.getText()); selectColour(); repaint(); };
        cancel.onClick = [this]
        { if (initial.customTheme) processor.setCustomTheme(initial.palette,initial.themeName); else processor.setTheme(initial.theme); setVisible(false); };
        close.onClick = [this] { setVisible(false); };
        import.onClick = [this] { choose(false); }; exportButton.onClick = [this] { choose(true); };
        for (auto* b : {&save,&create,&rename,&reset,&cancel,&close,&import,&exportButton}) b->setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    ~LoopXThemeEditor() override { selector.removeChangeListener(this); chooser.reset(); }
    void paint(juce::Graphics& g) override
    {
        const auto p = processor.getViewState().palette;
        g.fillAll(p.toolbar); g.setColour(p.buttonBorder); g.drawRect(getLocalBounds(),1);
    }
    void resized() override
    {
        const int w = getWidth()-24;
        name.setBounds(12,12,w/2-4,24); saved.setBounds(16+w/2,12,w/2-4,24);
        base.setBounds(12,42,w/2-4,24); element.setBounds(16+w/2,42,w/2-4,24);
        selector.setBounds(12,72,w,juce::jmax(50,getHeight()-144));
        int x = 12; for (auto* b : {&create,&save,&rename,&reset,&import,&exportButton}) { b->setBounds(x,getHeight()-66,(w-25)/6,24); x += (w-25)/6+5; }
        message.setBounds(12,getHeight()-36,w-154,24); cancel.setBounds(getWidth()-152,getHeight()-36,65,24); close.setBounds(getWidth()-82,getHeight()-36,70,24);
    }
private:
    void selectColour()
    {
        const int index = juce::jlimit(0,32,element.getSelectedId()-1);
        selector.setCurrentColour(processor.getViewState().palette.*(loopXColourFields()[size_t(index)].member),juce::dontSendNotification);
    }
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        auto p = processor.getViewState().palette;
        p.*(loopXColourFields()[size_t(juce::jlimit(0,32,element.getSelectedId()-1))].member) = selector.getCurrentColour();
        processor.setCustomTheme(p,name.getText()); repaint();
    }
    void refreshSaved()
    {
        saved.clear(juce::dontSendNotification); int i=1;
        for (const auto& t : processor.getViewState().userThemes) saved.addItem(t["name"].toString(),i++);
    }
    void choose(bool exporting)
    {
        chooser = std::make_unique<juce::FileChooser>(exporting ? "Export LoopX theme" : "Import LoopX theme",
            exporting ? juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("LoopX.theme.json") : juce::File{},"*.theme.json");
        juce::Component::SafePointer<LoopXThemeEditor> safe(this);
        const auto snapshot = processor.exportTheme();
        chooser->launchAsync((exporting ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting : juce::FileBrowserComponent::openMode)
            | juce::FileBrowserComponent::canSelectFiles,[safe,exporting,snapshot](const juce::FileChooser& c)
        {
            if (!safe || c.getResult() == juce::File{}) return;
            auto file = c.getResult();
            bool ok = false;
            if (exporting) { if (!file.getFileName().endsWithIgnoreCase(".theme.json")) file = file.getSiblingFile(file.getFileName()+".theme.json"); ok = file.replaceWithText(snapshot); }
            else if (file.existsAsFile() && file.getSize()<65536) ok = safe->processor.importTheme(file.loadFileAsString());
            safe->message.setText(ok ? (exporting ? "Theme exported" : "Theme imported") : "Invalid theme / file could not be saved",juce::dontSendNotification);
            if (ok && !exporting) { safe->name.setText(safe->processor.getViewState().themeName,false); safe->refreshSaved(); safe->selectColour(); safe->repaint(); }
        });
    }
    LoopXAudioProcessor& processor;
    LoopXAudioProcessor::ViewState initial;
    juce::TextEditor name;
    juce::ComboBox saved, base, element;
    juce::ColourSelector selector;
    juce::TextButton save, create, rename, reset, cancel, close, import, exportButton;
    juce::Label message;
    std::unique_ptr<juce::FileChooser> chooser;
};
