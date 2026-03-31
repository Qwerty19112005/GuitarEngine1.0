#pragma once
#include <JuceHeader.h>

// ==============================================================================
// 1. PLUGIN UI WINDOW
// ==============================================================================
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow(const juce::String& name, juce::AudioProcessor* p)
        : DocumentWindow(name, juce::Colours::darkgrey, DocumentWindow::allButtons)
    {
        if (p != nullptr && p->hasEditor())
        {
            editor.reset(p->createEditorIfNeeded());
            setContentNonOwned(editor.get(), true);
        }
        setResizable(true, true);
        centreWithSize(getWidth(), getHeight());
    }

    ~PluginWindow() override
    {
        clearContentComponent();
        editor.reset();
    }

    void closeButtonPressed() override { setVisible(false); }

private:
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginWindow)
};