#pragma once

#include <JuceHeader.h>

/** A scrollbar drawn below the waveform. The handle position represents the
    current view's start offset within the full file, and the handle's size
    is proportional to the current zoom level (view length / total length). */
class WaveformScrollbar : public juce::Component
{
public:
    WaveformScrollbar() = default;

    void setRange(double totalLengthIn, double viewStartIn, double viewLengthIn)
    {
        totalLength = juce::jmax(1.0, totalLengthIn);
        viewStart = viewStartIn;
        viewLength = juce::jlimit(1.0, totalLength, viewLengthIn);
        repaint();
    }

    std::function<void(double newViewStart)> onScroll;

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour(juce::Colours::black.withAlpha(0.4f));
        g.fillRoundedRectangle(bounds, 3.0f);

        auto handleBounds = getHandleBounds();
        g.setColour(juce::Colours::lightgrey.withAlpha(isMouseOverOrDown ? 0.9f : 0.7f));
        g.fillRoundedRectangle(handleBounds, 3.0f);
    }

    void mouseEnter(const juce::MouseEvent&) override { isMouseOverOrDown = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { isMouseOverOrDown = false; repaint(); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        isMouseOverOrDown = true;
        auto handleBounds = getHandleBounds();
        if (handleBounds.contains(e.position))
        {
            dragOffsetInHandle = e.position.x - handleBounds.getX();
        }
        else
        {
            // Clicked in the track: jump the handle so its centre is under the mouse.
            dragOffsetInHandle = handleBounds.getWidth() * 0.5f;
            moveHandleTo(e.position.x - dragOffsetInHandle);
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        moveHandleTo(e.position.x - dragOffsetInHandle);
    }

private:
    juce::Rectangle<float> getHandleBounds() const
    {
        const float w = (float) getWidth();
        const float h = (float) getHeight();
        const float fraction = (float) (viewLength / totalLength);
        const float handleWidth = juce::jmax(12.0f, w * fraction);
        const float maxX = w - handleWidth;
        const float startFraction = (float) (viewStart / juce::jmax(1.0, totalLength - viewLength));
        const float x = totalLength > viewLength ? juce::jlimit(0.0f, maxX, startFraction * maxX) : 0.0f;
        return { x, 0.0f, handleWidth, h };
    }

    void moveHandleTo(float newX)
    {
        const float w = (float) getWidth();
        auto handleBounds = getHandleBounds();
        const float maxX = w - handleBounds.getWidth();
        const float clampedX = juce::jlimit(0.0f, juce::jmax(0.0f, maxX), newX);
        const float fraction = maxX > 0.0f ? clampedX / maxX : 0.0f;
        const double newViewStart = fraction * (totalLength - viewLength);

        if (onScroll != nullptr)
            onScroll(newViewStart);
    }

    double totalLength = 1.0;
    double viewStart = 0.0;
    double viewLength = 1.0;
    float dragOffsetInHandle = 0.0f;
    bool isMouseOverOrDown = false;
};
