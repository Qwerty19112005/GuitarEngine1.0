#pragma once
#include <JuceHeader.h>


class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow(juce::AudioProcessor* processor, const juce::String& name)
        : juce::DocumentWindow(name, juce::Colours::black, 0)
    {
        setUsingNativeTitleBar(false);
        setTitleBarHeight(0);
        setDropShadowEnabled(false);
        setResizable(false, false);

        if (processor != nullptr && processor->hasEditor())
        {
            scalerContent = new ScalerComponent(processor, [this]() {
                this->closeButtonPressed();
                });
            setContentOwned(scalerContent, true);
        }

        auto screenArea = juce::Desktop::getInstance().getDisplays().getMainDisplay().userArea;
        setBounds(screenArea);
    }

    ~PluginWindow() override
    {
        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        setVisible(false);
    }

private:
    class ScalerComponent : public juce::Component
    {
    public:
        ScalerComponent(juce::AudioProcessor* processor, std::function<void()> onClose)
            : closeCallback(onClose)
        {
            editor.reset(processor->createEditorIfNeeded());
            if (editor != nullptr) {
                addAndMakeVisible(editor.get());

                rawWidth = editor->getWidth();
                rawHeight = editor->getHeight();
            }

            closeButton.setButtonText("X  CLOSE UI");
            closeButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkred.withAlpha(0.9f));
            closeButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
            closeButton.onClick = [this] { if (closeCallback) closeCallback(); };
            addAndMakeVisible(closeButton);
        }

        void resized() override
        {
            int headerHeight = 60;
            int availableWidth = getWidth();
            int availableHeight = getHeight() - headerHeight;

            if (editor != nullptr && rawWidth > 0 && rawHeight > 0)
            {
                editor->setTransform(juce::AffineTransform());

                float scaleX = (float)availableWidth / (float)rawWidth;
                float scaleY = (float)availableHeight / (float)rawHeight;
                float finalScale = juce::jmin(scaleX, scaleY);

                int targetWidth = (int)(rawWidth * finalScale);
                int targetHeight = (int)(rawHeight * finalScale);

                int xPos = (availableWidth - targetWidth) / 2;
                int yPos = headerHeight + (availableHeight - targetHeight) / 2;

                // ==========================================================
                // THE "FORCE FIRST" SMART CASCADE SCALING
                // ==========================================================

                // TIER 1: Ignore the plugin's 'isResizable' flag because many lie.
                // Force the physical bounds change immediately.
                editor->setBounds(xPos, yPos, targetWidth, targetHeight);

                // Now, check if the plugin actually obeyed our physical resize command.
                if (editor->getWidth() == rawWidth && editor->getHeight() == rawHeight)
                {
                    // The plugin stubbornly refused to change physical size. 
                    // TIER 2: Try Host DPI Scale API
                    editor->setScaleFactor(finalScale);

                    if (editor->getWidth() == rawWidth && editor->getHeight() == rawHeight)
                    {
                        // TIER 3: Brute Force 2D Transform (Only works on non-OpenGL UI)
                        editor->setTransform(juce::AffineTransform::scale(finalScale));
                        editor->setTopLeftPosition(xPos, yPos);
                    }
                    else
                    {
                        // DPI command worked! Center it.
                        editor->setTopLeftPosition((availableWidth - editor->getWidth()) / 2,
                            headerHeight + (availableHeight - editor->getHeight()) / 2);
                    }
                }
                else
                {
                    // TIER 1 WORKED! The plugin resized.
                    // However, plugins like Neural DSP enforce their own strict aspect ratios 
                    // and might have snapped to a size slightly different than 'targetWidth/Height'.
                    // We must dynamically re-center it based on the ACTUAL size it chose.
                    editor->setTopLeftPosition((availableWidth - editor->getWidth()) / 2,
                        headerHeight + (availableHeight - editor->getHeight()) / 2);
                }
            }

            closeButton.setBounds(getWidth() - 140, 10, 120, 40);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colours::black);
            g.setColour(juce::Colours::darkgrey.withAlpha(0.5f));
            g.drawLine(0, 60, (float)getWidth(), 60, 2.0f);
        }

    private:
        std::unique_ptr<juce::AudioProcessorEditor> editor;
        juce::TextButton closeButton;
        std::function<void()> closeCallback;
        int rawWidth = 0;
        int rawHeight = 0;
    };

    ScalerComponent* scalerContent = nullptr;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginWindow)
};