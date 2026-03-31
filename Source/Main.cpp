#include <JuceHeader.h>
#include "SimpleAudioHost.h"

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI guiInit;
    SimpleAudioHost host;
    juce::MessageManager::getInstance()->runDispatchLoop();
    return 0;
}