#pragma once
#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>
#include "PluginWindow.h"
#include <memory>
#include <map>
#include <vector>
#include <functional>
#include <fstream>
#include "json.hpp"

namespace te = tracktion_engine;
using json = nlohmann::json;


class SimpleAudioHost : public juce::OSCReceiver::Listener<juce::OSCReceiver::MessageLoopCallback>,
    public juce::Timer
{
public:
    struct PresetTrack {
        int bankIndex;
        int presetIndex;
        te::AudioTrack::Ptr track;
        te::VolumeAndPanPlugin* inputFader = nullptr;
        te::RackType::Ptr rackType;
        te::Plugin::Ptr rackInstance;
        te::VolumeAndPanPlugin* outputFader = nullptr;
    };

    struct NodeLocation {
        int bankIndex;
        int presetIndex;
    };

    SimpleAudioHost()
    {
        std::cout << "\n=======================================================" << std::endl;
        std::cout << "=== V50.5: SETLIST MEGA-GRAPH (ISOLATED SAVE FIX) ====" << std::endl;
        std::cout << "=======================================================\n" << std::endl;

        engine = std::make_unique<te::Engine>(ProjectInfo::projectName);
        auto& tracktionDM = engine->getDeviceManager();
        auto& juceDM = tracktionDM.deviceManager;

        tracktionDM.initialise(2, 2);
        juceDM.setCurrentAudioDeviceType("ASIO", true);

        juce::AudioIODeviceType* asioType = nullptr;
        for (auto* type : juceDM.getAvailableDeviceTypes()) {
            if (type->getTypeName() == "ASIO") {
                asioType = type;
                break;
            }
        }

        if (asioType != nullptr) {
            asioType->scanForDevices();
            for (const auto& name : asioType->getDeviceNames()) {
                if (name.containsIgnoreCase("AudioBox") || name.containsIgnoreCase("Audio Box") || name.containsIgnoreCase("PreSonus")) {
                    juce::AudioDeviceManager::AudioDeviceSetup setup;
                    juceDM.getAudioDeviceSetup(setup);
                    setup.inputDeviceName = name;
                    setup.outputDeviceName = name;
                    setup.bufferSize = 128;
                    setup.sampleRate = 44100;
                    setup.useDefaultInputChannels = false;
                    setup.useDefaultOutputChannels = false;
                    setup.inputChannels.setRange(0, 256, true);
                    setup.outputChannels.setRange(0, 256, true);
                    juceDM.setAudioDeviceSetup(setup, true);
                    std::cout << "[ASIO] Interface Locked to: " << name << std::endl;
                    break;
                }
            }
        }

        tracktionDM.dispatchPendingUpdates();
        tracktionDM.rescanWaveDeviceList();
        tracktionDM.enableAllWaveInputs();
        tracktionDM.enableAllWaveOutputs();

        if (tracktionDM.getNumWaveOutDevices() > 0) {
            if (auto* outDev = tracktionDM.getWaveOutDevice(0)) {
                outDev->setStereoPair(true);
                targetOutputDeviceID = outDev->getName();
                tracktionDM.setDefaultWaveOutDevice(targetOutputDeviceID);
            }
        }

        scanForPlugins();

        if (oscReceiver.connect(7001)) {
            oscReceiver.addListener(this);
            std::cout << "[OSC] SUCCESS: Listening on Port 7001..." << std::endl;
        }

        te::Edit::Options options{ *engine, te::createEmptyEdit(*engine) };
        currentEdit = std::make_unique<te::Edit>(options);
        if (auto* firstTrack = te::getAudioTracks(*currentEdit)[0]) currentEdit->deleteTrack(firstTrack);
        currentEdit->getTransport().ensureContextAllocated();
        currentEdit->getTransport().play(true);

        startTimer(15);
    }

    ~SimpleAudioHost() override
    {
        stopTimer();
        oscReceiver.disconnect();
        activeWindows.clear();
        currentEdit.reset();
        engine.reset();
    }

    int getSafeInt(const juce::OSCMessage& msg, int index) {
        if (index >= msg.size()) return 0;
        if (msg[index].isInt32()) return msg[index].getInt32();
        if (msg[index].isFloat32()) return static_cast<int>(msg[index].getFloat32());
        if (msg[index].isString()) return msg[index].getString().getIntValue();
        return 0;
    }

    juce::String getSafeString(const juce::OSCMessage& msg, int index) {
        if (index >= msg.size()) return "";
        if (msg[index].isString()) return msg[index].getString();
        if (msg[index].isInt32()) return juce::String(msg[index].getInt32());
        return "";
    }

    void clearMegaGraph()
    {
        std::cout << "\n[MEM_CHECK] Purging Mega-Graph..." << std::endl;
        activeWindows.clear();
        activePlugins.clear();
        activeBanks.clear();
        nodeToLocationMap.clear();

        currentActiveBank = 0;
        currentActivePreset = 0;
        pendingRoutingId++;

        te::Edit::Options options{ *engine, te::createEmptyEdit(*engine) };
        currentEdit = std::make_unique<te::Edit>(options);
        if (auto* firstTrack = te::getAudioTracks(*currentEdit)[0]) currentEdit->deleteTrack(firstTrack);

        currentEdit->getTransport().ensureContextAllocated();
        currentEdit->getTransport().play(true);
        std::cout << "[SYSTEM] Engine is empty and ready." << std::endl;
    }

    void ensureBankExists(int bankIndex)
    {
        if (activeBanks.count(bankIndex)) return;

        std::cout << "[SYSTEM] Spawning 4-Track Block for Bank " << bankIndex << "..." << std::endl;

        for (int presetIndex = 1; presetIndex <= 4; ++presetIndex) {
            PresetTrack pt;
            pt.bankIndex = bankIndex;
            pt.presetIndex = presetIndex;

            pt.track = currentEdit->insertNewAudioTrack(te::TrackInsertPoint(nullptr, nullptr), nullptr);
            pt.track->setName("Bank_" + juce::String(bankIndex) + "_Preset_" + juce::String(presetIndex));
            if (targetOutputDeviceID.isNotEmpty()) pt.track->getOutput().setOutputToDeviceID(targetOutputDeviceID);

            if (auto vol = currentEdit->getPluginCache().createNewPlugin(te::VolumeAndPanPlugin::xmlTypeName, juce::PluginDescription())) {
                pt.inputFader = dynamic_cast<te::VolumeAndPanPlugin*>(vol.get());
                pt.inputFader->setVolumeDb(-100.0f);
                pt.track->pluginList.insertPlugin(vol, 0, nullptr);
            }

            pt.rackType = currentEdit->getRackList().addNewRack();
            pt.rackType->rackName = "Rack_B" + juce::String(bankIndex) + "_P" + juce::String(presetIndex);

            auto rackInstanceInfo = te::RackInstance::create(*pt.rackType);
            if (auto rackInst = currentEdit->getPluginCache().createNewPlugin(rackInstanceInfo)) {
                pt.rackInstance = rackInst;
                pt.track->pluginList.insertPlugin(rackInst, 1, nullptr);
            }

            if (auto volOut = currentEdit->getPluginCache().createNewPlugin(te::VolumeAndPanPlugin::xmlTypeName, juce::PluginDescription())) {
                pt.outputFader = dynamic_cast<te::VolumeAndPanPlugin*>(volOut.get());
                pt.outputFader->setVolumeDb(0.0f);
                pt.track->pluginList.insertPlugin(volOut, 2, nullptr);
            }

            activeBanks[bankIndex][presetIndex] = std::move(pt);
        }
    }

    void applyCurrentRouting(bool isBankSwitch)
    {
        int myRoutingId = ++pendingRoutingId;

        for (auto& bankPair : activeBanks) {
            for (auto& presetPair : bankPair.second) {
                if (auto fader = presetPair.second.inputFader) fader->setVolumeDb(-100.0f);
                if (isBankSwitch) {
                    if (auto fader = presetPair.second.outputFader) fader->setVolumeDb(-100.0f);
                }
            }
        }

        if (isBankSwitch) {
            juce::Timer::callAfterDelay(10, [this, myRoutingId]() {
                juce::ScopedLock sl(queueLock);
                taskQueue.push_back([this, myRoutingId]() {
                    if (myRoutingId != pendingRoutingId) return;

                    for (auto& pair : activePlugins) {
                        int nId = pair.first;
                        if (nodeToLocationMap.count(nId)) {
                            int bId = nodeToLocationMap[nId].bankIndex;
                            bool isActiveBank = (bId == currentActiveBank);
                            if (pair.second != nullptr) {
                                pair.second->setEnabled(isActiveBank);
                            }
                        }
                    }

                    juce::Timer::callAfterDelay(10, [this, myRoutingId]() {
                        juce::ScopedLock sl2(queueLock);
                        taskQueue.push_back([this, myRoutingId]() {
                            if (myRoutingId != pendingRoutingId) return;

                            for (auto& bankPair : activeBanks) {
                                int bId = bankPair.first;
                                for (auto& presetPair : bankPair.second) {
                                    int pId = presetPair.first;

                                    if (auto fader = presetPair.second.outputFader) fader->setVolumeDb(0.0f);

                                    if (bId == currentActiveBank && pId == currentActivePreset) {
                                        if (auto fader = presetPair.second.inputFader) fader->setVolumeDb(0.0f);
                                    }
                                }
                            }
                            });
                        });
                    });
                });
        }
        else {
            if (activeBanks.count(currentActiveBank) && activeBanks[currentActiveBank].count(currentActivePreset)) {
                if (auto fader = activeBanks[currentActiveBank][currentActivePreset].inputFader) {
                    fader->setVolumeDb(0.0f);
                }
            }
        }
    }

    void forceScanNewPlugins()
    {
        std::cout << "\n[SYSTEM] Scanning default VST3 folders for new plugins... Please wait." << std::endl;
        if (formatManager.getNumFormats() == 0) return;

        juce::AudioPluginFormat* format = formatManager.getFormat(0);
        juce::FileSearchPath paths = format->getDefaultLocationsToSearch();
        juce::PluginDirectoryScanner scanner(pluginList, *format, paths, true, juce::File());

        juce::String pluginName;
        while (scanner.scanNextFile(true, pluginName)) {
            std::cout << "   -> Found: " << pluginName << std::endl;
        }

        juce::File cacheFile("D:\\Guitar Processor data\\JUCE XML\\PluginCache.xml");

        if (auto xml = pluginList.createXml()) {
            xml->writeTo(cacheFile);
            engine->getPluginManager().knownPluginList.recreateFromXml(*xml);
        }

        std::cout << "[SYSTEM] Plugin scan complete. Cache updated successfully.\n" << std::endl;
    }

    void oscMessageReceived(const juce::OSCMessage& message) override
    {
        juce::String pattern = message.getAddressPattern().toString();

        if (pattern == "/system/scan") {
            juce::ScopedLock sl(queueLock);
            taskQueue.push_back([this]() {
                forceScanNewPlugins();
                });
        }
        else if (pattern == "/setlist/load" && message.size() >= 1) {
            juce::String setName = getSafeString(message, 0);
            juce::ScopedLock sl(queueLock);
            taskQueue.push_back([this, setName]() {

                systemLocked = true;
                clearMegaGraph();
                currentSetName = setName;

                juce::File jsonFile("D:\\Guitar Processor data\\JSON\\Setlist_" + setName + ".json");
                if (!jsonFile.existsAsFile()) {
                    std::cout << "[ERROR] JSON File not found: " << jsonFile.getFullPathName() << std::endl;
                    systemLocked = false;
                    return;
                }

                std::string content = jsonFile.loadFileAsString().toStdString();
                try {
                    json data = json::parse(content);
                    std::cout << "[SYSTEM] Compiling Setlist Mega-Graph: " << setName << std::endl;

                    if (!data.contains("banks")) { systemLocked = false; return; }

                    for (auto& [bKey, bVal] : data["banks"].items()) {
                        int bankIdx = std::stoi(bKey);
                        ensureBankExists(bankIdx);

                        if (!bVal.contains("presets")) continue;

                        for (auto& [pKey, pVal] : bVal["presets"].items()) {
                            int presetIdx = std::stoi(pKey);

                            if (pVal.contains("nodes")) {
                                for (auto& n : pVal["nodes"]) {
                                    int nId = n["id"].get<int>();
                                    std::string pName = n["name"].get<std::string>();
                                    std::string b64State = n.contains("state") ? n["state"].get<std::string>() : "";

                                    std::unique_ptr<juce::PluginDescription> desc = nullptr;
                                    for (const auto& type : pluginList.getTypes()) {
                                        if (type.name.containsIgnoreCase(pName)) {
                                            desc = std::make_unique<juce::PluginDescription>(type);
                                            break;
                                        }
                                    }

                                    if (desc != nullptr) {
                                        juce::ValueTree pluginTree = te::ExternalPlugin::create(*engine, *desc);

                                        if (!b64State.empty()) {
                                            juce::String cleanB64 = juce::String(b64State).removeCharacters("\r\n\t ");
                                            if (cleanB64.isNotEmpty()) {
                                                pluginTree.setProperty(juce::Identifier("state"), cleanB64, nullptr);
                                            }
                                        }

                                        if (auto newPlugin = currentEdit->getPluginCache().getOrCreatePluginFor(pluginTree)) {

                                            newPlugin->setEnabled(bankIdx == currentActiveBank);
                                            activeBanks[bankIdx][presetIdx].rackType->addPlugin(newPlugin, { 0.5f, 0.5f }, false);
                                            activePlugins[nId] = newPlugin;
                                            nodeToLocationMap[nId] = { bankIdx, presetIdx };

                                            std::cout << "   -> Loaded Node " << nId << " (" << pName << ") [PRE-INJECTED SYNC]" << std::endl;
                                        }
                                    }
                                }
                            }

                            if (pVal.contains("links")) {
                                for (auto& l : pVal["links"]) {
                                    int srcN = l["src_node"].get<int>();
                                    int rawSrcC = l["src_chan"].get<int>();
                                    int srcC = rawSrcC + 1;
                                    int dstN = l["dst_node"].get<int>();
                                    int rawDstC = l["dst_chan"].get<int>();
                                    int dstC = rawDstC + 1;

                                    auto& rack = activeBanks[bankIdx][presetIdx].rackType;

                                    if (srcN == 1) {
                                        auto& trackDM = engine->getDeviceManager();
                                        if (auto* waveIn = trackDM.getWaveInDevice(rawSrcC)) {
                                            waveIn->setStereoPair(false);
                                            if (auto* instance = currentEdit->getCurrentInstanceForInputDevice(waveIn)) {
                                                instance->setTarget(activeBanks[bankIdx][presetIdx].track->itemID, false, nullptr, 0);
                                                instance->setRecordingEnabled(activeBanks[bankIdx][presetIdx].track->itemID, true);
                                                instance->getInputDevice().setMonitorMode(te::InputDevice::MonitorMode::on);
                                            }
                                        }
                                        if (activePlugins.count(dstN)) rack->addConnection(te::EditItemID{}, srcC, activePlugins[dstN]->itemID, dstC);
                                        std::cout << "   -> Restored Link: HW Audio (Pin " << srcC << ") -> Node " << dstN << " (Pin " << dstC << ")" << std::endl;
                                    }
                                    else if (dstN == 2) {
                                        if (activePlugins.count(srcN)) rack->addConnection(activePlugins[srcN]->itemID, srcC, te::EditItemID{}, dstC);
                                        std::cout << "   -> Restored Link: Node " << srcN << " (Pin " << srcC << ") -> Speaker Output (Pin " << dstC << ")" << std::endl;
                                    }
                                    else {
                                        if (activePlugins.count(srcN) && activePlugins.count(dstN)) {
                                            rack->addConnection(activePlugins[srcN]->itemID, srcC, activePlugins[dstN]->itemID, dstC);
                                            std::cout << "   -> Restored Link: Node " << srcN << " -> Node " << dstN << std::endl;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    std::cout << "[SYSTEM] Mega-Graph fully assembled. Booting Audio Engine..." << std::endl;
                    if (currentEdit) currentEdit->restartPlayback();
                    systemLocked = false;
                    applyCurrentRouting(true);

                }
                catch (...) {
                    std::cout << "[ERROR] Failed to parse Setlist JSON!" << std::endl;
                    systemLocked = false;
                }
                });
        }
        else if (pattern == "/preset/switch" && message.size() >= 2) {
            int targetBank = getSafeInt(message, 0);
            int targetPreset = getSafeInt(message, 1);

            juce::ScopedLock sl(queueLock);
            taskQueue.push_back([this, targetBank, targetPreset]() {

                if (systemLocked) {
                    std::cout << "[WARN] Preset switch ignored. System is currently locked for boot sequence." << std::endl;
                    return;
                }

                std::cout << "[SPILLOVER] Routing Mega-Graph -> Bank " << targetBank << " / Preset " << targetPreset << std::endl;

                bool isBankSwitch = (currentActiveBank != targetBank);
                currentActiveBank = targetBank;
                currentActivePreset = targetPreset;

                ensureBankExists(targetBank);
                applyCurrentRouting(isBankSwitch);
                });
        }
        else if (pattern == "/preset/save" && message.size() >= 2) {
            int targetBank = getSafeInt(message, 0);
            int targetPreset = getSafeInt(message, 1);

            juce::ScopedLock sl(queueLock);
            taskQueue.push_back([this, targetBank, targetPreset]() {
                std::cout << "[SAVE] Extracting VST States for Bank " << targetBank << " / Preset " << targetPreset << "..." << std::endl;

                juce::File jsonFile("D:\\Guitar Processor data\\JSON\\Setlist_" + currentSetName + ".json");
                if (!jsonFile.existsAsFile()) return;

                std::string content = jsonFile.loadFileAsString().toStdString();
                try {
                    json data = json::parse(content);
                    std::string bKey = std::to_string(targetBank);
                    std::string pKey = std::to_string(targetPreset);

                    if (data["banks"].contains(bKey) && data["banks"][bKey]["presets"].contains(pKey)) {
                        auto& nodesJson = data["banks"][bKey]["presets"][pKey]["nodes"];

                        for (auto& n : nodesJson) {
                            int nId = n["id"].get<int>();
                            if (activePlugins.count(nId)) {

                                // --- FIX: STRICT ISOLATED SAVING ---
                                if (nodeToLocationMap.count(nId) &&
                                    nodeToLocationMap[nId].bankIndex == targetBank &&
                                    nodeToLocationMap[nId].presetIndex == targetPreset)
                                {
                                    if (auto* ext = dynamic_cast<te::ExternalPlugin*>(activePlugins[nId].get())) {
                                        ext->flushPluginStateToValueTree();
                                        juce::String b64State = ext->state.getProperty(juce::Identifier("state"), "").toString();

                                        if (b64State.isNotEmpty()) {
                                            n["state"] = b64State.toStdString();
                                        }
                                    }
                                }
                                // -----------------------------------
                            }
                        }

                        jsonFile.replaceWithText(data.dump(4));
                        std::cout << "[SAVE SUCCESS] Tracktion ValueTree states safely locked to JSON." << std::endl;
                    }
                }
                catch (...) { std::cout << "[ERROR] JSON write failed during Save!" << std::endl; }
                });
        }
        else if (pattern == "/node/add" && message.size() >= 4) {
            int nodeId = getSafeInt(message, 0);
            juce::String pluginName = getSafeString(message, 1);
            int presetIndex = getSafeInt(message, 2);
            int bankIndex = getSafeInt(message, 3);

            juce::ScopedLock sl(queueLock);
            taskQueue.push_back([this, nodeId, pluginName, presetIndex, bankIndex]() {
                std::cout << "[OSC_RX] Received /node/add -> Node " << nodeId << " (" << pluginName << ")" << std::endl;

                ensureBankExists(bankIndex);
                auto& preset = activeBanks[bankIndex][presetIndex];

                std::unique_ptr<juce::PluginDescription> desc = nullptr;
                for (const auto& type : pluginList.getTypes()) {
                    if (type.name.containsIgnoreCase(pluginName)) {
                        desc = std::make_unique<juce::PluginDescription>(type);
                        break;
                    }
                }

                if (desc != nullptr) {
                    juce::ValueTree pluginTree = te::ExternalPlugin::create(*engine, *desc);

                    if (auto newPlugin = currentEdit->getPluginCache().getOrCreatePluginFor(pluginTree)) {
                        newPlugin->setEnabled(bankIndex == currentActiveBank);
                        preset.rackType->addPlugin(newPlugin, { 0.5f, 0.5f }, false);
                        activePlugins[nodeId] = newPlugin;
                        nodeToLocationMap[nodeId] = { bankIndex, presetIndex };
                        std::cout << "[RACK_MGR] Instantiated VST in active RAM." << std::endl;

                        if (currentEdit) currentEdit->restartPlayback();
                    }
                }
                else {
                    std::cout << "[ERROR] Plugin " << pluginName << " not found in Cache." << std::endl;
                }
                });
        }
        else if (pattern == "/node/remove" && message.size() >= 1) {
            int nodeId = getSafeInt(message, 0);
            juce::ScopedLock sl(queueLock);
            taskQueue.push_back([this, nodeId]() {
                std::cout << "[OSC_RX] Received /node/remove -> Node " << nodeId << std::endl;

                if (activePlugins.count(nodeId)) {
                    if (activeWindows.count(nodeId)) activeWindows.erase(nodeId);
                    activePlugins[nodeId]->removeFromParent();
                    activePlugins.erase(nodeId);
                    nodeToLocationMap.erase(nodeId);
                    std::cout << "[RACK_MGR] Destroyed Node " << nodeId << " & closed Window." << std::endl;

                    if (currentEdit) currentEdit->restartPlayback();
                }
                });
        }
        else if (pattern == "/node/show_ui" && message.size() >= 1) {
            int nodeId = getSafeInt(message, 0);
            juce::ScopedLock sl(queueLock);
            taskQueue.push_back([this, nodeId]() {
                std::cout << "[OSC_RX] Received /node/show_ui -> Node " << nodeId << std::endl;

                if (activePlugins.count(nodeId)) {
                    if (!activeWindows.count(nodeId)) {
                        if (auto* ext = dynamic_cast<te::ExternalPlugin*>(activePlugins[nodeId].get())) {
                            if (auto* processor = ext->getAudioPluginInstance()) {
                                activeWindows[nodeId] = std::make_unique<PluginWindow>(processor, activePlugins[nodeId]->getName());
                            }
                        }
                    }
                    if (activeWindows.count(nodeId)) {
                        activeWindows[nodeId]->setAlwaysOnTop(true);
                        activeWindows[nodeId]->setVisible(true);
                        activeWindows[nodeId]->toFront(true);
                    }
                }
                });
        }
        else if (pattern == "/node/connect" && message.size() >= 4) {
            int srcNode = getSafeInt(message, 0);
            int rawSrcChan = getSafeInt(message, 1);
            int srcChanRack = rawSrcChan + 1;
            int dstNode = getSafeInt(message, 2);
            int rawDstChan = getSafeInt(message, 3);
            int dstChanRack = rawDstChan + 1;

            juce::ScopedLock sl(queueLock);
            taskQueue.push_back([this, srcNode, rawSrcChan, srcChanRack, dstNode, rawDstChan, dstChanRack]() {
                std::cout << "[OSC_RX] Received /node/connect -> Node " << srcNode << " to Node " << dstNode << std::endl;

                int tb = -1, tp = -1;
                if (srcNode != 1 && nodeToLocationMap.count(srcNode)) { tb = nodeToLocationMap[srcNode].bankIndex; tp = nodeToLocationMap[srcNode].presetIndex; }
                else if (dstNode != 2 && nodeToLocationMap.count(dstNode)) { tb = nodeToLocationMap[dstNode].bankIndex; tp = nodeToLocationMap[dstNode].presetIndex; }
                if (tb == -1 || tp == -1 || !activeBanks.count(tb) || !activeBanks[tb].count(tp)) return;

                auto& rack = activeBanks[tb][tp].rackType;

                if (srcNode == 1) {
                    auto& trackDM = engine->getDeviceManager();
                    if (auto* waveIn = trackDM.getWaveInDevice(rawSrcChan)) {
                        waveIn->setStereoPair(false);
                        if (auto* instance = currentEdit->getCurrentInstanceForInputDevice(waveIn)) {
                            instance->setTarget(activeBanks[tb][tp].track->itemID, false, nullptr, 0);
                            instance->setRecordingEnabled(activeBanks[tb][tp].track->itemID, true);
                            instance->getInputDevice().setMonitorMode(te::InputDevice::MonitorMode::on);
                        }
                    }
                    rack->addConnection(te::EditItemID{}, srcChanRack, activePlugins[dstNode]->itemID, dstChanRack);
                    std::cout << "   -> Wired: Hardware Audio (Pin " << srcChanRack << ") -> Node " << dstNode << std::endl;
                }
                else if (dstNode == 2) {
                    rack->addConnection(activePlugins[srcNode]->itemID, srcChanRack, te::EditItemID{}, dstChanRack);
                    std::cout << "   -> Wired: Node " << srcNode << " -> Speaker Output (Pin " << dstChanRack << ")" << std::endl;
                }
                else {
                    rack->addConnection(activePlugins[srcNode]->itemID, srcChanRack, activePlugins[dstNode]->itemID, dstChanRack);
                    std::cout << "   -> Wired: Node " << srcNode << " -> Node " << dstNode << std::endl;
                }

                if (currentEdit) currentEdit->restartPlayback();
                });
        }
        else if (pattern == "/node/disconnect" && message.size() >= 4) {
            int srcNode = getSafeInt(message, 0);
            int rawSrcChan = getSafeInt(message, 1);
            int srcChanRack = rawSrcChan + 1;
            int dstNode = getSafeInt(message, 2);
            int rawDstChan = getSafeInt(message, 3);
            int dstChanRack = rawDstChan + 1;

            juce::ScopedLock sl(queueLock);
            taskQueue.push_back([this, srcNode, srcChanRack, dstNode, dstChanRack]() {
                std::cout << "[OSC_RX] Received /node/disconnect -> Node " << srcNode << " -X- Node " << dstNode << std::endl;

                int tb = -1, tp = -1;
                if (srcNode != 1 && nodeToLocationMap.count(srcNode)) { tb = nodeToLocationMap[srcNode].bankIndex; tp = nodeToLocationMap[srcNode].presetIndex; }
                else if (dstNode != 2 && nodeToLocationMap.count(dstNode)) { tb = nodeToLocationMap[dstNode].bankIndex; tp = nodeToLocationMap[dstNode].presetIndex; }
                if (tb == -1 || tp == -1 || !activeBanks.count(tb) || !activeBanks[tb].count(tp)) return;

                auto& rack = activeBanks[tb][tp].rackType;

                if (srcNode == 1) rack->removeConnection(te::EditItemID{}, srcChanRack, activePlugins[dstNode]->itemID, dstChanRack);
                else if (dstNode == 2) rack->removeConnection(activePlugins[srcNode]->itemID, srcChanRack, te::EditItemID{}, dstChanRack);
                else rack->removeConnection(activePlugins[srcNode]->itemID, srcChanRack, activePlugins[dstNode]->itemID, dstChanRack);

                std::cout << "   -> Connection Severed." << std::endl;

                if (currentEdit) currentEdit->restartPlayback();
                });
        }
    }

    void timerCallback() override
    {
        int tasksProcessed = 0;

        while (tasksProcessed < 15) {
            std::function<void()> task = nullptr;
            {
                juce::ScopedLock sl(queueLock);
                if (taskQueue.empty()) break;
                task = taskQueue.front();
                taskQueue.erase(taskQueue.begin());
            }
            if (task != nullptr) {
                task();
                tasksProcessed++;
            }
        }

        static int diagCounter = 0;
        if (++diagCounter >= 100) {
            diagCounter = 0;
            if (engine != nullptr) {
                double cpuLoad = engine->getDeviceManager().deviceManager.getCpuUsage() * 100.0;
                if (currentActiveBank == 0 || systemLocked) {
                    std::cout << "[DIAGNOSTIC] CPU: " << juce::String(cpuLoad, 1) << "% | STATUS: BOOTING / STANDBY" << std::endl;
                }
                else {
                    std::cout << "[DIAGNOSTIC] CPU: " << juce::String(cpuLoad, 1) << "% | Active Bank: " << currentActiveBank << " | Presets Loaded: " << activeBanks.size() * 4 << std::endl;
                }
            }
        }
    }

    void scanForPlugins()
    {
        formatManager.addFormat(new juce::VST3PluginFormat());
        juce::File cacheFile("D:\\Guitar Processor data\\JUCE XML\\PluginCache.xml");
        if (cacheFile.existsAsFile()) {
            if (auto xml = juce::XmlDocument::parse(cacheFile)) {
                pluginList.recreateFromXml(*xml);
                engine->getPluginManager().knownPluginList.recreateFromXml(*xml);
            }
        }
    }

private:
    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<te::Edit> currentEdit;
    juce::String targetOutputDeviceID;

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList pluginList;

    juce::String currentSetName = "Default";
    int currentActiveBank = 0;
    int currentActivePreset = 0;
    bool systemLocked = false;
    int pendingRoutingId = 0;

    std::map<int, std::map<int, PresetTrack>> activeBanks;
    std::map<int, te::Plugin::Ptr> activePlugins;
    std::map<int, std::unique_ptr<PluginWindow>> activeWindows;
    std::map<int, NodeLocation> nodeToLocationMap;

    juce::CriticalSection queueLock;
    std::vector<std::function<void()>> taskQueue;
    juce::OSCReceiver oscReceiver;
};