#pragma once
#include <JuceHeader.h>
#include <atomic>

class InputRouterProcessor : public juce::AudioProcessor
{
public:
    InputRouterProcessor()
        : AudioProcessor(BusesProperties()
            .withInput("Input", juce::AudioChannelSet::stereo(), true)
            .withOutput("Out P1", juce::AudioChannelSet::stereo(), true)
            .withOutput("Out P2", juce::AudioChannelSet::stereo(), true)
            .withOutput("Out P3", juce::AudioChannelSet::stereo(), true)
            .withOutput("Out P4", juce::AudioChannelSet::stereo(), true))
    {
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        tempBuffer.setSize(2, samplesPerBlock);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        auto numSamples = buffer.getNumSamples();

        if (tempBuffer.getNumSamples() < numSamples) {
            tempBuffer.setSize(2, numSamples, false, false, true);
        }

        if (buffer.getNumChannels() >= 2) {
            tempBuffer.copyFrom(0, 0, buffer.getReadPointer(0), numSamples);
            tempBuffer.copyFrom(1, 0, buffer.getReadPointer(1), numSamples);
        }

        buffer.clear();

        // Instantly routes input ONLY to the active preset
        int destChannelOffset = activePresetIndex * 2;
        if (destChannelOffset + 1 < buffer.getNumChannels())
        {
            buffer.copyFrom(destChannelOffset, 0, tempBuffer.getReadPointer(0), numSamples);
            buffer.copyFrom(destChannelOffset + 1, 0, tempBuffer.getReadPointer(1), numSamples);
        }
    }

    void setActivePreset(int index) { activePresetIndex = index; }

    const juce::String getName() const override { return "Input Router"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void releaseResources() override {}
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    std::atomic<int> activePresetIndex{ 0 };
    juce::AudioBuffer<float> tempBuffer;
};

// ==============================================================================
// 3. THE OUTPUT MIXER (4 Stereo Ins -> 1 Stereo Out)
// ==============================================================================
class OutputMixerProcessor : public juce::AudioProcessor
{
public:
    OutputMixerProcessor()
        : AudioProcessor(BusesProperties()
            .withInput("In P1", juce::AudioChannelSet::stereo(), true)
            .withInput("In P2", juce::AudioChannelSet::stereo(), true)
            .withInput("In P3", juce::AudioChannelSet::stereo(), true)
            .withInput("In P4", juce::AudioChannelSet::stereo(), true)
            .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        sumBuffer.setSize(2, samplesPerBlock);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        auto numSamples = buffer.getNumSamples();

        if (sumBuffer.getNumSamples() < numSamples) {
            sumBuffer.setSize(2, numSamples, false, false, true);
        }
        sumBuffer.clear();

        // THE SPILLOVER FIX: Always sum ALL incoming channels.
        // Unbypassed plugins will naturally fade out their delays here.
        // Bypassed plugins output silence, so summing them costs zero DSP.
        int numInputChannels = buffer.getNumChannels();
        for (int ch = 0; ch < numInputChannels; ++ch) {
            int destChannel = ch % 2; // Even channels to Left (0), Odd to Right (1)
            sumBuffer.addFrom(destChannel, 0, buffer.getReadPointer(ch), numSamples);
        }

        buffer.clear();

        if (buffer.getNumChannels() >= 2) {
            buffer.copyFrom(0, 0, sumBuffer.getReadPointer(0), numSamples);
            buffer.copyFrom(1, 0, sumBuffer.getReadPointer(1), numSamples);
        }
    }

    const juce::String getName() const override { return "Output Mixer"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void releaseResources() override {}
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    juce::AudioBuffer<float> sumBuffer;
};