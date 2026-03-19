#include <JuceHeader.h>
#include <map>
#include <vector>
#include <atomic>

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

// ==============================================================================
// 2. THE INPUT ROUTER (1 Stereo In -> 4 Stereo Outs)
// ==============================================================================
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

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        auto numSamples = buffer.getNumSamples();
        juce::AudioBuffer<float> tempBuffer(2, numSamples);

        if (buffer.getNumChannels() >= 2) {
            tempBuffer.copyFrom(0, 0, buffer.getReadPointer(0), numSamples);
            tempBuffer.copyFrom(1, 0, buffer.getReadPointer(1), numSamples);
        }

        buffer.clear();

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
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    std::atomic<int> activePresetIndex{ 0 };
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

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        auto numSamples = buffer.getNumSamples();
        juce::AudioBuffer<float> sumBuffer(2, numSamples);
        sumBuffer.clear();

        int srcChannelOffset = activePresetIndex * 2;
        if (srcChannelOffset + 1 < buffer.getNumChannels())
        {
            sumBuffer.addFrom(0, 0, buffer.getReadPointer(srcChannelOffset), numSamples);
            sumBuffer.addFrom(1, 0, buffer.getReadPointer(srcChannelOffset + 1), numSamples);
        }

        buffer.clear();

        if (buffer.getNumChannels() >= 2) {
            buffer.copyFrom(0, 0, sumBuffer.getReadPointer(0), numSamples);
            buffer.copyFrom(1, 0, sumBuffer.getReadPointer(1), numSamples);
        }
    }

    void setActivePreset(int index) { activePresetIndex = index; }

    const juce::String getName() const override { return "Output Mixer"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    std::atomic<int> activePresetIndex{ 0 };
};


// ==============================================================================
// 4. AUDIO HOST
// ==============================================================================
class SimpleAudioHost : public juce::OSCReceiver,
    public juce::OSCReceiver::ListenerWithOSCAddress<juce::OSCReceiver::MessageLoopCallback>
{
public:
    SimpleAudioHost()
    {
        mainGraph = std::make_unique<juce::AudioProcessorGraph>();
        player.setProcessor(mainGraph.get());

        deviceManager.setCurrentAudioDeviceType("ASIO", true);
        juce::String myDeviceName = "AudioBox ASIO Driver";
        auto error = deviceManager.initialise(2, 2, nullptr, false, myDeviceName, nullptr);

        if (error.isEmpty())
        {
            deviceManager.addAudioCallback(&player);
            std::cout << "Audio Interface is live" << std::endl;

            scanForPlugins();
            setupHardwareNodes();

            if (connect(7001))
            {
                std::cout << "OSC Server listening on Port 7001" << std::endl;
                addListener(this, "/node/add");
                addListener(this, "/node/connect");
                addListener(this, "/node/disconnect");
                addListener(this, "/node/show_ui");
                addListener(this, "/node/remove");
                addListener(this, "/preset/switch");
                addListener(this, "/preset/save");
                addListener(this, "/preset/restore_state");
                addListener(this, "/list/clear");
            }
        }
    }

    ~SimpleAudioHost()
    {
        disconnect();
        deviceManager.removeAudioCallback(&player);
        player.setProcessor(nullptr);
    }

    void clearMegaGraph()
    {
        std::cout << "=== EXECUTING HARD MEMORY CLEAR ===" << std::endl;

        std::vector<int> nodesToDestroy;
        for (const auto& pair : activeNodes) {
            int id = pair.first;
            // Never destroy hardware IO (1,2) or internal Routers (1000, 1001)
            if (id != 1 && id != 2 && id != 1000 && id != 1001) {
                nodesToDestroy.push_back(id);
            }
        }

        // DESTROY WINDOW FIRST, THEN PROCESSOR
        for (int id : nodesToDestroy) {
            if (activeWindows.find(id) != activeWindows.end()) {
                activeWindows.erase(id);
            }
            mainGraph->removeNode(activeNodes[id].get());
            activeNodes.erase(id);
        }

        presetNodeMap.clear();
        std::cout << "Bank Memory Cleared. Awaiting new routing instructions." << std::endl;
    }

    void oscMessageReceived(const juce::OSCMessage& message) override
    {
        // --- ADD PLUGIN ---
        if (message.getAddressPattern() == "/node/add" && message.size() >= 3)
        {
            int nodeId = message[0].getInt32();
            juce::String pluginName = message[1].getString();
            int presetIndex = message[2].getInt32() - 1;

            juce::String listName = "";
            int bank = 0;
            if (message.size() == 5) {
                listName = message[3].getString();
                bank = message[4].getInt32();
            }

            loadPluginWithID(nodeId, pluginName, presetIndex, listName, bank);
        }

        // --- MASTER LIST CLEAR ---
        else if (message.getAddressPattern() == "/list/clear")
        {
            clearMegaGraph();
            currentBank = -1; // Reset tracker so the next switch forces a rebuild
        }

        // --- PRESET SWITCH ---
        else if (message.getAddressPattern() == "/preset/switch" && message.size() == 2)
        {
            int bank = message[0].isInt32() ? message[0].getInt32() : 1;
            int targetPreset = (message[1].isInt32() ? message[1].getInt32() : 1) - 1;

            if (bank != currentBank) {
                clearMegaGraph();
                currentBank = bank;
            }

            currentActivePreset = targetPreset;
            std::cout << "JUCE Router switched to -> Bank " << bank << ", Preset Index " << targetPreset << std::endl;

            if (activeNodes.find(1000) != activeNodes.end() && activeNodes[1000] != nullptr) {
                if (auto* r = dynamic_cast<InputRouterProcessor*>(activeNodes[1000]->getProcessor()))
                    r->setActivePreset(targetPreset);
            }
            if (activeNodes.find(1001) != activeNodes.end() && activeNodes[1001] != nullptr) {
                if (auto* m = dynamic_cast<OutputMixerProcessor*>(activeNodes[1001]->getProcessor()))
                    m->setActivePreset(targetPreset);
            }

            for (const auto& [presetIdx, nodeList] : presetNodeMap)
            {
                bool shouldBypass = (presetIdx != targetPreset);
                for (int nodeId : nodeList)
                {
                    if (activeNodes.find(nodeId) != activeNodes.end()) {
                        activeNodes[nodeId]->setBypassed(shouldBypass);
                    }
                }
            }
        }

        // --- SAVE PRESET DSP STATE ---
        else if (message.getAddressPattern() == "/preset/save" && message.size() == 3)
        {
            juce::String listName = message[0].getString();
            int bank = message[1].getInt32();
            int preset = message[2].getInt32();   // 1-indexed from Python (e.g., 1, 2, 3, 4)
            int presetIndex = preset - 1;         // Convert to 0-indexed for the internal C++ map

            juce::XmlElement presetXml("PRESET_STATE");
            presetXml.setAttribute("bank", bank);
            presetXml.setAttribute("preset", preset);

            // Search the map using the 0-indexed variable
            if (presetNodeMap.find(presetIndex) != presetNodeMap.end())
            {
                for (int nodeId : presetNodeMap[presetIndex])
                {
                    if (activeNodes.find(nodeId) != activeNodes.end())
                    {
                        auto* processor = activeNodes[nodeId]->getProcessor();
                        if (processor != nullptr)
                        {
                            juce::XmlElement* pluginXml = new juce::XmlElement("PLUGIN");
                            pluginXml->setAttribute("id", nodeId);
                            pluginXml->setAttribute("name", processor->getName());

                            juce::MemoryBlock stateBlock;
                            processor->getStateInformation(stateBlock);

                            // Save massive VST3 states as Inner Text
                            juce::XmlElement* stateXml = new juce::XmlElement("STATE_DATA");
                            stateXml->addTextElement(stateBlock.toBase64Encoding());
                            pluginXml->addChildElement(stateXml);

                            presetXml.addChildElement(pluginXml);
                        }
                    }
                }
            }

            // USE NEW DIRECTORY HELPER
            juce::File xmlDir = getJuceXmlDirectory();
            juce::String filename = "DSP_" + listName + "_Bank" + juce::String(bank) + "_Preset" + juce::String(preset) + ".xml";
            juce::File saveFile = xmlDir.getChildFile(filename);

            presetXml.writeTo(saveFile);
            std::cout << "Successfully saved massive DSP state to: " << filename << std::endl;
        }

        // --- RESTORE DSP STATE FROM XML ---
        else if (message.getAddressPattern() == "/preset/restore_state" && message.size() == 3)
        {
            juce::String listName = message[0].getString();
            int bank = message[1].getInt32();
            int preset = message[2].getInt32();

            // USE NEW DIRECTORY HELPER
            juce::File xmlDir = getJuceXmlDirectory();
            juce::String filename = "DSP_" + listName + "_Bank" + juce::String(bank) + "_Preset" + juce::String(preset) + ".xml";
            juce::File loadFile = xmlDir.getChildFile(filename);

            if (loadFile.existsAsFile())
            {
                std::unique_ptr<juce::XmlElement> xml = juce::XmlDocument::parse(loadFile);
                if (xml != nullptr && xml->hasTagName("PRESET_STATE"))
                {
                    for (auto* pluginXml : xml->getChildIterator())
                    {
                        if (pluginXml->hasTagName("PLUGIN"))
                        {
                            int nodeId = pluginXml->getIntAttribute("id");

                            // Read from the inner text node safely
                            auto* stateXml = pluginXml->getChildByName("STATE_DATA");

                            if (stateXml != nullptr && activeNodes.find(nodeId) != activeNodes.end())
                            {
                                juce::String stateBase64 = stateXml->getAllSubText();
                                juce::MemoryBlock stateBlock;
                                stateBlock.fromBase64Encoding(stateBase64);

                                auto* processor = activeNodes[nodeId]->getProcessor();

                                // Suspend VST3 processing before applying megabytes of data
                                processor->suspendProcessing(true);
                                processor->setStateInformation(stateBlock.getData(), (int)stateBlock.getSize());
                                processor->suspendProcessing(false);
                            }
                        }
                    }
                    std::cout << "Restored DSP state from: " << filename << std::endl;
                }
            }
        }

        // --- CONNECT CABLE ---
        else if (message.getAddressPattern() == "/node/connect" && message.size() == 5)
        {
            int srcNode = message[0].getInt32();
            int srcChan = message[1].getInt32();
            int dstNode = message[2].getInt32();
            int dstChan = message[3].getInt32();
            int pIndex = message[4].getInt32() - 1;

            if (srcNode == 1) {
                srcNode = 1000;
                srcChan = srcChan + (pIndex * 2);
            }
            if (dstNode == 2) {
                dstNode = 1001;
                dstChan = dstChan + (pIndex * 2);
            }

            mainGraph->addConnection({ { juce::AudioProcessorGraph::NodeID(srcNode), srcChan },
                                       { juce::AudioProcessorGraph::NodeID(dstNode), dstChan } });
        }

        // --- DISCONNECT CABLE ---
        else if (message.getAddressPattern() == "/node/disconnect" && message.size() == 5)
        {
            int srcNode = message[0].getInt32();
            int srcChan = message[1].getInt32();
            int dstNode = message[2].getInt32();
            int dstChan = message[3].getInt32();
            int pIndex = message[4].getInt32() - 1;

            if (srcNode == 1) {
                srcNode = 1000;
                srcChan = srcChan + (pIndex * 2);
            }
            if (dstNode == 2) {
                dstNode = 1001;
                dstChan = dstChan + (pIndex * 2);
            }

            mainGraph->removeConnection({ { juce::AudioProcessorGraph::NodeID(srcNode), srcChan },
                                          { juce::AudioProcessorGraph::NodeID(dstNode), dstChan } });
        }

        // --- SHOW UI ---
        else if (message.getAddressPattern() == "/node/show_ui" && message.size() == 1)
        {
            int nodeId = message[0].getInt32();
            juce::MessageManager::callAsync([this, nodeId]() {
                if (activeWindows.find(nodeId) != activeWindows.end()) {
                    activeWindows[nodeId]->setVisible(true);
                    activeWindows[nodeId]->toFront(true);
                }
                });
        }

        // --- REMOVE PLUGIN ---
        else if (message.getAddressPattern() == "/node/remove" && message.size() == 1)
        {
            int nodeId = message[0].getInt32();

            if (activeWindows.find(nodeId) != activeWindows.end()) {
                activeWindows.erase(nodeId);
            }

            if (activeNodes.find(nodeId) != activeNodes.end()) {
                mainGraph->removeNode(activeNodes[nodeId].get());
                activeNodes.erase(nodeId);
            }
        }
    }

    void setupHardwareNodes()
    {
        mainGraph->clear();
        activeNodes.clear();

        using AudioGraphIO = juce::AudioProcessorGraph::AudioGraphIOProcessor;

        activeNodes[1] = mainGraph->addNode(std::make_unique<AudioGraphIO>(AudioGraphIO::audioInputNode), juce::AudioProcessorGraph::NodeID(1));
        activeNodes[2] = mainGraph->addNode(std::make_unique<AudioGraphIO>(AudioGraphIO::audioOutputNode), juce::AudioProcessorGraph::NodeID(2));

        activeNodes[1000] = mainGraph->addNode(std::make_unique<InputRouterProcessor>(), juce::AudioProcessorGraph::NodeID(1000));
        activeNodes[1001] = mainGraph->addNode(std::make_unique<OutputMixerProcessor>(), juce::AudioProcessorGraph::NodeID(1001));

        mainGraph->addConnection({ { juce::AudioProcessorGraph::NodeID(1), 0 }, { juce::AudioProcessorGraph::NodeID(1000), 0 } });
        mainGraph->addConnection({ { juce::AudioProcessorGraph::NodeID(1), 1 }, { juce::AudioProcessorGraph::NodeID(1000), 1 } });

        mainGraph->addConnection({ { juce::AudioProcessorGraph::NodeID(1001), 0 }, { juce::AudioProcessorGraph::NodeID(2), 0 } });
        mainGraph->addConnection({ { juce::AudioProcessorGraph::NodeID(1001), 1 }, { juce::AudioProcessorGraph::NodeID(2), 1 } });
    }

    void loadPluginWithID(int nodeId, const juce::String& pluginName, int presetIndex, juce::String listName, int bank)
    {
        std::unique_ptr<juce::PluginDescription> foundDesc;
        for (auto& desc : pluginList.getTypes())
        {
            if (desc.name == pluginName) { foundDesc = std::make_unique<juce::PluginDescription>(desc); break; }
        }

        if (foundDesc == nullptr) return;

        juce::String errorMsg;
        auto instance = formatManager.createPluginInstance(*foundDesc, 48000, 128, errorMsg);

        if (instance != nullptr)
        {
            auto pluginNode = mainGraph->addNode(std::move(instance), juce::AudioProcessorGraph::NodeID(nodeId));
            activeNodes[nodeId] = pluginNode;
            presetNodeMap[presetIndex].push_back(nodeId);

            if (presetIndex != currentActivePreset) {
                pluginNode->setBypassed(true);
            }

            // 1. THE GUI BOOT FIX: 
            // We create the UI immediately (hidden) so internal VST3 controllers fully boot.
            activeWindows[nodeId] = std::make_unique<PluginWindow>(pluginName, pluginNode->getProcessor());
            activeWindows[nodeId]->setVisible(false);

            // 2. INSTANT AUTO-RESTORE (No Timers)
            if (listName.isNotEmpty()) {
                int actualPresetNum = presetIndex + 1;

                // USE NEW DIRECTORY HELPER
                juce::File xmlDir = getJuceXmlDirectory();
                juce::String filename = "DSP_" + listName + "_Bank" + juce::String(bank) + "_Preset" + juce::String(actualPresetNum) + ".xml";

                // Construct the absolute path 
                juce::String fullPath = xmlDir.getChildFile(filename).getFullPathName();

                if (activeNodes.find(nodeId) != activeNodes.end()) {
                    juce::File loadFile(fullPath);
                    if (loadFile.existsAsFile()) {
                        std::unique_ptr<juce::XmlElement> xml = juce::XmlDocument::parse(loadFile);
                        if (xml != nullptr) {
                            for (auto* pluginXml : xml->getChildIterator()) {
                                if (pluginXml->hasTagName("PLUGIN") && pluginXml->getIntAttribute("id") == nodeId) {
                                    auto* stateXml = pluginXml->getChildByName("STATE_DATA");
                                    if (stateXml != nullptr) {

                                        // 3. THE BASE64 CORRUPTION FIX: 
                                        juce::String rawText = stateXml->getAllSubText();
                                        juce::String cleanBase64 = rawText.removeCharacters("\r\n\t ");

                                        juce::MemoryBlock stateBlock;
                                        if (stateBlock.fromBase64Encoding(cleanBase64)) {
                                            auto* processor = activeNodes[nodeId]->getProcessor();
                                            processor->suspendProcessing(true);
                                            processor->setStateInformation(stateBlock.getData(), (int)stateBlock.getSize());
                                            processor->suspendProcessing(false);
                                            std::cout << "SUCCESS: Heavy VST3 State Injected INSTANTLY for Node " << nodeId << std::endl;
                                        }
                                        else {
                                            std::cout << "ERROR: Base64 Decode Failed for Node " << nodeId << std::endl;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    else {
                        // Never fail silently
                        std::cout << "ERROR: Could not find XML file at -> " << fullPath << std::endl;
                    }
                }
            }
        }
    }

    void scanForPlugins()
    {
        formatManager.addFormat(new juce::VST3PluginFormat());

        // USE NEW DIRECTORY HELPER FOR CACHE
        juce::File cacheFile = getJuceXmlDirectory().getChildFile("PluginCache.xml");

        if (cacheFile.existsAsFile())
        {
            std::unique_ptr<juce::XmlElement> xml = juce::XmlDocument::parse(cacheFile);
            if (xml != nullptr) { pluginList.recreateFromXml(*xml); return; }
        }

        // NEW: Tell the console we are starting a deep scan
        std::cout << "No PluginCache.xml found in D: drive. Starting full VST3 deep scan..." << std::endl;
        std::cout << "Please wait, this may take a few minutes if you have heavy plugins..." << std::endl;

        juce::File vst3Folder("C:\\Program Files\\Common Files\\VST3");
        if (vst3Folder.exists() && formatManager.getFormat(0) != nullptr)
        {
            juce::FileSearchPath searchPath(vst3Folder.getFullPathName());
            juce::PluginDirectoryScanner scanner(pluginList, *formatManager.getFormat(0), searchPath, true, juce::File(), true);
            juce::String name;

            // NEW: Print every plugin as it is found so the app doesn't look frozen
            while (scanner.scanNextFile(true, name)) {
                std::cout << "Registered: " << name << std::endl;
            }

            if (auto xml = pluginList.createXml()) {
                xml->writeTo(cacheFile);
                std::cout << "--- PLUGIN SCAN COMPLETE. Cache saved to D: Drive! ---" << std::endl;
            }
        }
    }

private:
    // --- NEW: DEDICATED FOLDER ROUTING ---
    juce::File getJuceXmlDirectory()
    {
        juce::File dir("D:\\Guitar Processor data\\JUCE XML");
        if (!dir.exists()) {
            dir.createDirectory();
        }
        return dir;
    }

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList pluginList;
    juce::AudioProcessorPlayer player;
    std::unique_ptr<juce::AudioProcessorGraph> mainGraph;

    std::map<int, juce::AudioProcessorGraph::Node::Ptr> activeNodes;
    std::map<int, std::unique_ptr<PluginWindow>> activeWindows;

    int currentBank = -1;
    int currentActivePreset = 0;
    std::map<int, std::vector<int>> presetNodeMap;
};

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI guiInit;
    SimpleAudioHost host;
    juce::MessageManager::getInstance()->runDispatchLoop();
    return 0;
}