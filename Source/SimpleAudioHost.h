#pragma once
#include <JuceHeader.h>
#include <map>
#include <vector>
#include "PluginWindow.h"
#include "CustomProcessors.h"
#include "json.hpp"
using json = nlohmann::json;

class SetlistManager
{
public:
    SetlistManager()
    {
        juce::File dir("D:\\Guitar Processor data\\Unified_Setlists");
        if (!dir.exists()) dir.createDirectory();
        setlistDir = dir;
    }

    void createNewSetlist(const std::string& name)
    {
        currentSetlistName = name;
        currentData = json::object();
        currentData["name"] = name;
        currentData["banks"] = json::object();
        saveToDisk();
    }

    bool loadSetlist(const std::string& name)
    {
        juce::File file = setlistDir.getChildFile(name + ".json");
        if (!file.existsAsFile()) return false;

        std::string content = file.loadFileAsString().toStdString();
        try {
            currentData = json::parse(content);
            currentSetlistName = name;
            return true;
        }
        catch (...) {
            return false;
        }
    }

    void saveToDisk()
    {
        if (currentSetlistName.empty()) return;
        juce::File file = setlistDir.getChildFile(currentSetlistName + ".json");
        file.replaceWithText(currentData.dump(4));
    }

    json currentData;
    std::string currentSetlistName;

private:
    juce::File setlistDir;
};


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
            if (id != 1 && id != 2 && id != 1000 && id != 1001) {
                nodesToDestroy.push_back(id);
            }
        }

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

        else if (message.getAddressPattern() == "/list/clear")
        {
            clearMegaGraph();
            currentBank = -1;
        }


        // PRESET SWITCH
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

            // Switch the guitar input to the new preset
            if (activeNodes.find(1000) != activeNodes.end() && activeNodes[1000] != nullptr) {
                if (auto* r = dynamic_cast<InputRouterProcessor*>(activeNodes[1000]->getProcessor()))
                    r->setActivePreset(targetPreset);
            }

            // Wake up the new plugins
            for (int nodeId : presetNodeMap[targetPreset]) {
                if (activeNodes.find(nodeId) != activeNodes.end()) {
                    activeNodes[nodeId]->setBypassed(false);
                }
            }

            // Trigger 2 second crossover timer for the old plugins
          
            int spilloverTimeMs = 2000;
            juce::Timer::callAfterDelay(spilloverTimeMs, [this]() {
                for (const auto& [presetIdx, nodeList] : presetNodeMap) {
                    if (presetIdx != currentActivePreset) {
                        for (int nodeId : nodeList) {
                            if (activeNodes.find(nodeId) != activeNodes.end()) {
                                activeNodes[nodeId]->setBypassed(true);
                            }
                        }
                    }
                }
                });
        }

        else if (message.getAddressPattern() == "/preset/save" && message.size() == 3)
        {
            juce::String listName = message[0].getString();
            int bank = message[1].getInt32();
            int preset = message[2].getInt32();
            int presetIndex = preset - 1;

            std::string listStr = listName.toStdString();
            std::string bKey = std::to_string(bank);
            std::string pKey = std::to_string(preset);

            try {
                if (setlistManager.currentSetlistName != listStr) {
                    if (!setlistManager.loadSetlist(listStr)) {
                        setlistManager.createNewSetlist(listStr);
                    }
                }

                if (!setlistManager.currentData["banks"].is_object()) {
                    setlistManager.currentData["banks"] = json::object();
                }

                json& banksJson = setlistManager.currentData["banks"];
                if (!banksJson.contains(bKey)) banksJson[bKey] = json::object();

                json& bankObj = banksJson[bKey];
                if (!bankObj.contains("presets")) bankObj["presets"] = json::object();

                json& presetsJson = bankObj["presets"];
                if (!presetsJson.contains(pKey)) presetsJson[pKey] = json::object();

                json& presetObj = presetsJson[pKey];
                presetObj["nodes"] = json::array();
                json& nodesJson = presetObj["nodes"];

                presetObj["links"] = json::array();
                json& linksJson = presetObj["links"];

                if (presetNodeMap.find(presetIndex) != presetNodeMap.end())
                {
                    for (int nodeId : presetNodeMap[presetIndex])
                    {
                        if (activeNodes.find(nodeId) != activeNodes.end())
                        {
                            auto* processor = activeNodes[nodeId]->getProcessor();
                            if (processor != nullptr)
                            {
                                juce::MemoryBlock stateBlock;
                                processor->getStateInformation(stateBlock);

                                json nodeObj;
                                nodeObj["id"] = nodeId;
                                nodeObj["name"] = processor->getName().toStdString();
                                nodeObj["state"] = stateBlock.toBase64Encoding().toStdString();
                                nodesJson.push_back(nodeObj);
                            }
                        }
                    }
                }

                for (auto connection : mainGraph->getConnections()) {
                    int srcNode = connection.source.nodeID.uid;
                    int dstNode = connection.destination.nodeID.uid;
                    int srcChan = connection.source.channelIndex;
                    int dstChan = connection.destination.channelIndex;

                    bool belongsToPreset = false;
                    auto& pNodes = presetNodeMap[presetIndex];

                    if (std::find(pNodes.begin(), pNodes.end(), srcNode) != pNodes.end()) belongsToPreset = true;
                    if (std::find(pNodes.begin(), pNodes.end(), dstNode) != pNodes.end()) belongsToPreset = true;
                    if (srcNode == 1000 && (srcChan == presetIndex * 2 || srcChan == presetIndex * 2 + 1)) belongsToPreset = true;
                    if (dstNode == 1001 && (dstChan == presetIndex * 2 || dstChan == presetIndex * 2 + 1)) belongsToPreset = true;

                    if (belongsToPreset) {
                        json linkObj;
                        linkObj["src_node"] = srcNode;
                        linkObj["src_chan"] = srcChan;
                        linkObj["dst_node"] = dstNode;
                        linkObj["dst_chan"] = dstChan;
                        linksJson.push_back(linkObj);
                    }
                }

                setlistManager.saveToDisk();
                std::cout << "SUCCESS: Saved DSP state & Routing to Unified JSON for -> " << listStr << " / Bank " << bKey << " / Preset " << pKey << std::endl;

            }
            catch (const json::exception& e) {
                std::cout << "JSON EXCEPTION DURING SAVE: " << e.what() << std::endl;
            }
            catch (const std::exception& e) {
                std::cout << "STANDARD EXCEPTION DURING SAVE: " << e.what() << std::endl;
            }
        }

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

            // Bypass upon loading if it's not the active preset
            if (presetIndex != currentActivePreset) {
                pluginNode->setBypassed(true);
            }

            activeWindows[nodeId] = std::make_unique<PluginWindow>(pluginName, pluginNode->getProcessor());
            activeWindows[nodeId]->setVisible(false);

            if (listName.isNotEmpty()) {
                int actualPresetNum = presetIndex + 1;
                std::string listStr = listName.toStdString();
                std::string bKey = std::to_string(bank);
                std::string pKey = std::to_string(actualPresetNum);

                if (setlistManager.currentSetlistName != listStr) {
                    setlistManager.loadSetlist(listStr);
                }

                try {
                    if (setlistManager.currentData.contains("banks") &&
                        setlistManager.currentData["banks"].contains(bKey) &&
                        setlistManager.currentData["banks"][bKey].contains("presets") &&
                        setlistManager.currentData["banks"][bKey]["presets"].contains(pKey) &&
                        setlistManager.currentData["banks"][bKey]["presets"][pKey].contains("nodes"))
                    {
                        json& nodesJson = setlistManager.currentData["banks"][bKey]["presets"][pKey]["nodes"];

                        for (auto& n : nodesJson) {
                            if (n["id"].get<int>() == nodeId) {
                                std::string base64State = n["state"].get<std::string>();

                                juce::String cleanB64(base64State);
                                cleanB64 = cleanB64.removeCharacters("\r\n\t ");

                                juce::MemoryBlock stateBlock;
                                if (stateBlock.fromBase64Encoding(cleanB64)) {
                                    auto* processor = activeNodes[nodeId]->getProcessor();
                                    processor->suspendProcessing(true);
                                    processor->setStateInformation(stateBlock.getData(), (int)stateBlock.getSize());
                                    processor->suspendProcessing(false);
                                    std::cout << "SUCCESS: VST3 State Injected from Unified JSON for Node " << nodeId << std::endl;
                                }
                                else {
                                    std::cout << "ERROR: JSON Base64 Decode Failed for Node " << nodeId << std::endl;
                                }
                            }
                        }
                    }
                }
                catch (const std::exception& e) {
                    std::cout << "JSON Parse Error during auto-restore: " << e.what() << std::endl;
                }
            }
        }
    }

    void scanForPlugins()
    {
        formatManager.addFormat(new juce::VST3PluginFormat());

        juce::File cacheFile = getJuceXmlDirectory().getChildFile("PluginCache.xml");

        if (cacheFile.existsAsFile())
        {
            std::unique_ptr<juce::XmlElement> xml = juce::XmlDocument::parse(cacheFile);
            if (xml != nullptr) { pluginList.recreateFromXml(*xml); return; }
        }

        std::cout << "No PluginCache.xml found in D: drive. Starting full VST3 deep scan..." << std::endl;
        std::cout << "Please wait, this may take a few minutes if you have heavy plugins..." << std::endl;

        juce::File vst3Folder("C:\\Program Files\\Common Files\\VST3");
        if (vst3Folder.exists() && formatManager.getFormat(0) != nullptr)
        {
            juce::FileSearchPath searchPath(vst3Folder.getFullPathName());
            juce::PluginDirectoryScanner scanner(pluginList, *formatManager.getFormat(0), searchPath, true, juce::File(), true);
            juce::String name;

            while (scanner.scanNextFile(true, name)) {
                std::cout << "Registered: " << name << std::endl;
            }

            if (auto xml = pluginList.createXml()) {
                xml->writeTo(cacheFile);
                std::cout << "--- PLUGIN SCAN COMPLETE. Cache saved to D: Drive! ---" << std::endl;
            }
        }
    }

    void loadBankNative(const std::string& setlistName, int targetBank)
    {
        std::cout << "--- NATIVE LOAD TRIGGERED: " << setlistName << " / Bank " << targetBank << " ---" << std::endl;

        if (!setlistManager.loadSetlist(setlistName)) {
            std::cout << "ERROR: Could not find setlist JSON." << std::endl;
            return;
        }

        std::string bKey = std::to_string(targetBank);
        if (!setlistManager.currentData["banks"].contains(bKey)) {
            std::cout << "Bank " << targetBank << " is empty or does not exist." << std::endl;
            return;
        }

        clearMegaGraph();
        currentBank = targetBank;

        json& presetsJson = setlistManager.currentData["banks"][bKey]["presets"];

        for (int p = 1; p <= 4; ++p) {
            std::string pKey = std::to_string(p);
            int presetIndex = p - 1;

            if (presetsJson.contains(pKey)) {

                if (presetsJson[pKey].contains("nodes")) {
                    for (auto& n : presetsJson[pKey]["nodes"]) {
                        int nodeId = n["id"].get<int>();
                        std::string pluginName = n["name"].get<std::string>();

                        loadPluginWithID(nodeId, juce::String(pluginName), presetIndex, juce::String(setlistName), targetBank);
                    }
                }

                if (presetsJson[pKey].contains("links")) {
                    for (auto& l : presetsJson[pKey]["links"]) {
                        int srcId = l["src_node"].get<int>();
                        int dstId = l["dst_node"].get<int>();
                        int srcCh = l["src_chan"].get<int>();
                        int dstCh = l["dst_chan"].get<int>();

                        mainGraph->addConnection({ { juce::AudioProcessorGraph::NodeID(srcId), srcCh },
                                                   { juce::AudioProcessorGraph::NodeID(dstId), dstCh } });
                    }
                }
            }
        }

        currentActivePreset = 0;
        if (auto* r = dynamic_cast<InputRouterProcessor*>(activeNodes[1000]->getProcessor())) r->setActivePreset(0);

        for (const auto& [presetIdx, nodeList] : presetNodeMap) {
            bool shouldBypass = (presetIdx != 0);
            for (int nodeId : nodeList) {
                if (activeNodes.find(nodeId) != activeNodes.end()) activeNodes[nodeId]->setBypassed(shouldBypass);
            }
        }

        std::cout << "--- NATIVE BANK LOAD COMPLETE ---" << std::endl;
    }


private:
    juce::File getJuceXmlDirectory()
    {
        juce::File dir("D:\\Guitar Processor data\\JUCE XML");
        if (!dir.exists()) {
            dir.createDirectory();
        }
        return dir;
    }

    SetlistManager setlistManager;
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