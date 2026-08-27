#include <JuceHeader.h>
#include "MainComponent.h"

class WaveformDisplayApplication : public juce::JUCEApplication
{
public:
    WaveformDisplayApplication() = default;

    const juce::String getApplicationName() override { return "WaveformDisplay"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override
    {
        mainWindow = std::make_unique<MainWindow>(getApplicationName());

        auto args = juce::JUCEApplication::getCommandLineParameterArray();
        for (auto& arg : args)
        {
            juce::File f(arg);
            if (f.existsAsFile())
            {
                mainWindow->loadFileOnStartup(f);
                break;
            }
        }
        juce::ignoreUnused(commandLine);
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow(const juce::String& name)
            : DocumentWindow(name,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour(juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);

            centreWithSize(getWidth(), getHeight());
            setResizable(true, true);
            setVisible(true);
        }

        void loadFileOnStartup(const juce::File& file)
        {
            if (auto* mc = dynamic_cast<MainComponent*>(getContentComponent()))
                mc->loadFile(file);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(WaveformDisplayApplication)
