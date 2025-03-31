#ifdef MESHTASTIC_INCLUDE_INKHUD

#include "./Applet.h"

#include "main.h"

#include "RTC.h"

using namespace NicheGraphics;

InkHUD::AppletFont InkHUD::Applet::fontLarge;      // General purpose font. Set by setDefaultFonts
InkHUD::AppletFont InkHUD::Applet::fontSmall;      // General purpose font. Set by setDefaultFonts
constexpr float InkHUD::Applet::LOGO_ASPECT_RATIO; // Ratio of the Meshtastic logo

InkHUD::Applet::Applet() : GFX(0, 0)
{
    // GFX is given initial dimensions of 0
    // The width and height will change dynamically, depending on Applet tiling
    // If you're getting a "divide by zero error", consider it an assert:
    // WindowManager should be the only one controlling the rendering

    inkhud = InkHUD::getInstance();
    settings = &inkhud->persistence->settings;
    latestMessage = &inkhud->persistence->latestMessage;
}

// Draw a single pixel
// The raw pixel output generated by AdafruitGFX drawing all passes through here
// Hand off to the applet's tile, which will in-turn pass to the renderer
void InkHUD::Applet::drawPixel(int16_t x, int16_t y, uint16_t color)
{
    // Only render pixels if they fall within user's cropped region
    if (x >= cropLeft && x < (cropLeft + cropWidth) && y >= cropTop && y < (cropTop + cropHeight))
        assignedTile->handleAppletPixel(x, y, (Color)color);
}

// Link our applet to a tile
// This can only be called by Tile::assignApplet
// The tile determines the applets dimensions
// Pixel output is passed to tile during render()
void InkHUD::Applet::setTile(Tile *t)
{
    // If we're setting (not clearing), make sure the link is "reciprocal"
    if (t)
        assert(t->getAssignedApplet() == this);

    assignedTile = t;
}

// The tile to which our applet is assigned
InkHUD::Tile *InkHUD::Applet::getTile()
{
    return assignedTile;
}

// Draw the applet
void InkHUD::Applet::render()
{
    assert(assignedTile);                              // Ensure that we have a tile
    assert(assignedTile->getAssignedApplet() == this); // Ensure that we have a reciprocal link with the tile

    // WindowManager::update has now consumed the info about our update request
    // Clear everything for future requests
    wantRender = false;                                       // Flag set by requestUpdate
    wantAutoshow = false;                                     // Flag set by requestAutoShow. May or may not have been honored.
    wantUpdateType = Drivers::EInk::UpdateTypes::UNSPECIFIED; // Update type we wanted. May on may not have been granted.

    updateDimensions();
    resetDrawingSpace();
    onRender(); // Derived applet's drawing takes place here

    // Handle "Tile Highlighting"
    // Some devices may use an auxiliary button to switch between tiles
    // When this happens, we temporarily highlight the newly focused tile with a border

    // If our tile is (or was) highlighted, to indicate a change in focus
    if (Tile::highlightTarget == assignedTile) {
        // Draw the highlight
        if (!Tile::highlightShown) {
            drawRect(0, 0, width(), height(), BLACK);
            Tile::startHighlightTimeout();
            Tile::highlightShown = true;
        }

        // Clear the highlight
        else {
            Tile::cancelHighlightTimeout();
            Tile::highlightShown = false;
            Tile::highlightTarget = nullptr;
        }
    }
}

// Does the applet want to render now?
// Checks whether the applet called requestUpdate recently, in response to an event
// Used by WindowManager::update
bool InkHUD::Applet::wantsToRender()
{
    return wantRender;
}

// Does the applet want to be moved to foreground before next render, to show new data?
// User specifies whether an applet has permission for this, using the on-screen menu
// Used by WindowManager::update
bool InkHUD::Applet::wantsToAutoshow()
{
    return wantAutoshow;
}

// Which technique would this applet prefer that the display use to change the image?
// Used by WindowManager::update
Drivers::EInk::UpdateTypes InkHUD::Applet::wantsUpdateType()
{
    return wantUpdateType;
}

// Get size of the applet's drawing space from its tile
// Performed immediately before derived applet's drawing code runs
void InkHUD::Applet::updateDimensions()
{
    assert(assignedTile);
    WIDTH = assignedTile->getWidth();
    HEIGHT = assignedTile->getHeight();
    _width = WIDTH;
    _height = HEIGHT;
}

// Ensure that render() always starts with the same initial drawing config
void InkHUD::Applet::resetDrawingSpace()
{
    resetCrop();         // Allow pixel from any region of the applet to draw
    setTextColor(BLACK); // Reset text params
    setCursor(0, 0);
    setTextWrap(false);
    setFont(fontSmall);
}

// Tell InkHUD::Renderer that we want to render now
// Applets should internally listen for events they are interested in, via MeshModule, CallbackObserver etc
// When an applet decides it has heard something important, and wants to redraw, it calls this method
// Once the renderer has given other applets a chance to process whatever event we just detected,
// it will run Applet::render(), which may draw our applet to screen, if it is shown (foreground)
// We should requestUpdate even if our applet is currently background, because this might be changed by autoshow
void InkHUD::Applet::requestUpdate(Drivers::EInk::UpdateTypes type)
{
    wantRender = true;
    wantUpdateType = type;
    inkhud->requestUpdate();
}

// Ask window manager to move this applet to foreground at start of next render
// Users select which applets have permission for this using the on-screen menu
void InkHUD::Applet::requestAutoshow()
{
    wantAutoshow = true;
}

// Called when an Applet begins running
// Active applets are considered "enabled"
// They should now listen for events, and request their own updates
// They may also be unexpectedly renderer at any time by other InkHUD components
// Applets can be activated at run-time through the on-screen menu
void InkHUD::Applet::activate()
{
    onActivate(); // Call derived class' handler
    active = true;
}

// Called when an Applet stops running
// Inactive applets are considered "disabled"
// They should not listen for events, process data
// They will not be rendered
// Applets can be deactivated at run-time through the on-screen menu
void InkHUD::Applet::deactivate()
{
    // If applet is still in foreground, run its onBackground code first
    if (isForeground())
        sendToBackground();

    // If applet is active, run its onDeactivate code first
    if (isActive())
        onDeactivate(); // Derived class' handler
    active = false;
}

// Is the Applet running?
// Note: active / inactive is not related to background / foreground
// An inactive applet is *fully* disabled
bool InkHUD::Applet::isActive()
{
    return active;
}

// Begin showing the Applet
// It will be rendered immediately to whichever tile it is assigned
// The Renderer will also now honor requestUpdate() calls from this applet
void InkHUD::Applet::bringToForeground()
{
    if (!foreground) {
        foreground = true;
        onForeground(); // Run derived applet class' handler
    }

    requestUpdate();
}

// Stop showing the Applet
// Calls to requestUpdate() will no longer be honored
// When one applet moves to background, another should move to foreground (exception: some system applets)
void InkHUD::Applet::sendToBackground()
{
    if (foreground) {
        foreground = false;
        onBackground(); // Run derived applet class' handler
    }
}

// Is the applet currently displayed on a tile
// Note: in some uncommon situations, an applet may be "foreground", and still not visible.
// This can occur when a system applet is covering the screen (e.g. during BLE pairing)
// This is not our applets responsibility to handle,
// as in those situations, the system applet will have "locked" rendering
bool InkHUD::Applet::isForeground()
{
    return foreground;
}

// Limit drawing to a certain region of the applet
// Pixels outside this region will be discarded
void InkHUD::Applet::setCrop(int16_t left, int16_t top, uint16_t width, uint16_t height)
{
    cropLeft = left;
    cropTop = top;
    cropWidth = width;
    cropHeight = height;
}

// Allow drawing to any region of the Applet
// Reverses Applet::setCrop
void InkHUD::Applet::resetCrop()
{
    setCrop(0, 0, width(), height());
}

// Convert relative width to absolute width, in px
// X(0) is 0
// X(0.5) is width() / 2
// X(1) is width()
uint16_t InkHUD::Applet::X(float f)
{
    return width() * f;
}

// Convert relative hight to absolute height, in px
// Y(0) is 0
// Y(0.5) is height() / 2
// Y(1) is height()
uint16_t InkHUD::Applet::Y(float f)
{
    return height() * f;
}

// Print text, specifying the position of any edge / corner of the textbox
void InkHUD::Applet::printAt(int16_t x, int16_t y, const char *text, HorizontalAlignment ha, VerticalAlignment va)
{
    printAt(x, y, std::string(text), ha, va);
}

// Print text, specifying the position of any edge / corner of the textbox
void InkHUD::Applet::printAt(int16_t x, int16_t y, std::string text, HorizontalAlignment ha, VerticalAlignment va)
{
    // Custom font
    // - set with AppletFont::addSubstitution
    // - find certain UTF8 chars
    // - replace with glyph from custom font (or suitable ASCII addSubstitution?)
    getFont().applySubstitutions(&text);

    // We do still have to run getTextBounds to find the width
    int16_t textOffsetX, textOffsetY;
    uint16_t textWidth, textHeight;
    getTextBounds(text.c_str(), 0, 0, &textOffsetX, &textOffsetY, &textWidth, &textHeight);

    int16_t cursorX = 0;
    int16_t cursorY = 0;

    switch (ha) {
    case LEFT:
        cursorX = x - textOffsetX;
        break;
    case CENTER:
        cursorX = (x - textOffsetX) - (textWidth / 2);
        break;
    case RIGHT:
        cursorX = (x - textOffsetX) - textWidth;
        break;
    }

    // We're using a fixed line height, rather than sizing to text (getTextBounds)

    switch (va) {
    case TOP:
        cursorY = y + currentFont.heightAboveCursor();
        break;
    case MIDDLE:
        cursorY = (y + currentFont.heightAboveCursor()) - (currentFont.lineHeight() / 2);
        break;
    case BOTTOM:
        cursorY = (y + currentFont.heightAboveCursor()) - currentFont.lineHeight();
        break;
    }

    setCursor(cursorX, cursorY);
    print(text.c_str());
}

// Set which font should be used for subsequent drawing
// This is AppletFont type, which is a wrapper for AdafruitGFX font, with some precalculated dimension data
void InkHUD::Applet::setFont(AppletFont f)
{
    GFX::setFont(f.gfxFont);
    currentFont = f;
}

// Get which font is currently being used for drawing
// This is AppletFont type, which is a wrapper for AdafruitGFX font, with some precalculated dimension data
InkHUD::AppletFont InkHUD::Applet::getFont()
{
    return currentFont;
}

// Gets rendered width of a string
// Wrapper for getTextBounds
uint16_t InkHUD::Applet::getTextWidth(const char *text)
{

    // We do still have to run getTextBounds to find the width
    int16_t textOffsetX, textOffsetY;
    uint16_t textWidth, textHeight;
    getTextBounds(text, 0, 0, &textOffsetX, &textOffsetY, &textWidth, &textHeight);

    return textWidth;
}

// Gets rendered width of a string
// Wrapper for getTextBounds
uint16_t InkHUD::Applet::getTextWidth(std::string text)
{
    getFont().applySubstitutions(&text);

    return getTextWidth(text.c_str());
}

// Evaluate SNR and RSSI to qualify signal strength at one of four discrete levels
// Roughly comparable to values used by the iOS app;
// I didn't actually go look up the code, just fit to a sample graphic I have of the iOS signal indicator
InkHUD::Applet::SignalStrength InkHUD::Applet::getSignalStrength(float snr, float rssi)
{
    uint8_t score = 0;

    // Give a score for the SNR
    if (snr > -17.5)
        score += 2;
    else if (snr > -26.0)
        score += 1;

    // Give a score for the RSSI
    if (rssi > -115.0)
        score += 3;
    else if (rssi > -120.0)
        score += 2;
    else if (rssi > -126.0)
        score += 1;

    // Combine scores, then give a result
    if (score >= 5)
        return SIGNAL_GOOD;
    else if (score >= 4)
        return SIGNAL_FAIR;
    else if (score > 0)
        return SIGNAL_BAD;
    else
        return SIGNAL_NONE;
}

// Apply the standard "node id" formatting to a nodenum int: !0123abdc
std::string InkHUD::Applet::hexifyNodeNum(NodeNum num)
{
    // Not found in nodeDB, show a hex nodeid instead
    char nodeIdHex[10];
    sprintf(nodeIdHex, "!%0x", num); // Convert to the typical "fixed width hex with !" format
    return std::string(nodeIdHex);
}

// Print text, with word wrapping
// Avoids splitting words in half, instead moving the entire word to a new line wherever possible
void InkHUD::Applet::printWrapped(int16_t left, int16_t top, uint16_t width, std::string text)
{
    // Custom font glyphs
    // - set with AppletFont::addSubstitution
    // - find certain UTF8 chars
    // - replace with glyph from custom font (or suitable ASCII addSubstitution?)
    getFont().applySubstitutions(&text);

    // Place the AdafruitGFX cursor to suit our "top" coord
    setCursor(left, top + getFont().heightAboveCursor());

    // How wide a space character is
    // Used when simulating print, for dimensioning
    // Works around issues where getTextDimensions() doesn't account for whitespace
    const uint8_t wSp = getFont().widthBetweenWords();

    // Move through our text, character by character
    uint16_t wordStart = 0;
    for (uint16_t i = 0; i < text.length(); i++) {

        // Found: end of word (split by spaces or newline)
        // Also handles end of string
        if (text[i] == ' ' || text[i] == '\n' || i == text.length() - 1) {
            // Isolate this word
            uint16_t wordLength = (i - wordStart) + 1; // Plus one. Imagine: "a". End - Start is 0, but length is 1
            std::string word = text.substr(wordStart, wordLength);
            wordStart = i + 1; // Next word starts *after* the space

            // If word is terminated by a newline char, don't actually print it.
            // We'll manually add a new line later
            if (word.back() == '\n')
                word.pop_back();

            // Measure the word, in px
            int16_t l, t;
            uint16_t w, h;
            getTextBounds(word.c_str(), getCursorX(), getCursorY(), &l, &t, &w, &h);

            // Word is short
            if (w < width) {
                // Word fits on current line
                if ((l + w + wSp) < left + width)
                    print(word.c_str());

                // Word doesn't fit on current line
                else {
                    setCursor(left, getCursorY() + getFont().lineHeight()); // Newline
                    print(word.c_str());
                }
            }

            // Word is really long
            // (wider than applet)
            else {
                // Horribly inefficient:
                // Rather than working directly with the glyph sizes,
                // we're going to run everything through getTextBounds as a c-string of length 1
                // This is because AdafruitGFX has special internal handling for their legacy 6x8 font,
                // which would be a pain to add manually here.
                // These super-long strings probably don't come up often so we can maybe tolerate this.

                // Todo: rewrite making use of AdafruitGFX native text wrapping
                char cstr[] = {0, 0};
                int16_t l, t;
                uint16_t w, h;
                for (uint16_t c = 0; c < word.length(); c++) {
                    // Shove next char into a c string
                    cstr[0] = word[c];
                    getTextBounds(cstr, getCursorX(), getCursorY(), &l, &t, &w, &h);

                    // Manual newline, if next character will spill beyond screen edge
                    if ((l + w) > left + width)
                        setCursor(left, getCursorY() + getFont().lineHeight());

                    // Print next character
                    print(word[c]);
                }
            }
        }

        // If word was terminated by a newline char, manually add the new line now
        if (text[i] == '\n') {
            setCursor(left, getCursorY() + getFont().lineHeight()); // Manual newline
            wordStart = i + 1; // New word begins after the newline. Otherwise print will add an *extra* line
        }
    }
}

// Simulate running printWrapped, to determine how tall the block of text will be.
// This is a wasteful way of handling things. Maybe some way to optimize in future?
uint32_t InkHUD::Applet::getWrappedTextHeight(int16_t left, uint16_t width, std::string text)
{
    // Cache the current crop region
    int16_t cL = cropLeft;
    int16_t cT = cropTop;
    uint16_t cW = cropWidth;
    uint16_t cH = cropHeight;

    setCrop(-1, -1, 0, 0);              // Set crop to temporarily discard all pixels
    printWrapped(left, 0, width, text); // Simulate only - no pixels drawn

    // Restore previous crop region
    cropLeft = cL;
    cropTop = cT;
    cropWidth = cW;
    cropHeight = cH;

    // Note: printWrapped() offsets the initial cursor position by heightAboveCursor() val,
    // so we need to account for that when determining the height
    return (getCursorY() + getFont().heightBelowCursor());
}

// Fill a region with sparse diagonal lines, to create a pseudo-translucent fill
void InkHUD::Applet::hatchRegion(int16_t x, int16_t y, uint16_t w, uint16_t h, uint8_t spacing, Color color)
{
    // Cache the currently cropped region
    int16_t oldCropL = cropLeft;
    int16_t oldCropT = cropTop;
    uint16_t oldCropW = cropWidth;
    uint16_t oldCropH = cropHeight;

    setCrop(x, y, w, h);

    // Draw lines starting along the top edge, every few px
    for (int16_t ix = x; ix < x + w; ix += spacing) {
        for (int16_t i = 0; i < w || i < h; i++) {
            drawPixel(ix + i, y + i, color);
        }
    }

    // Draw lines starting along the left edge, every few px
    for (int16_t iy = y; iy < y + h; iy += spacing) {
        for (int16_t i = 0; i < w || i < h; i++) {
            drawPixel(x + i, iy + i, color);
        }
    }

    // Restore any previous crop
    // If none was set, this will clear
    cropLeft = oldCropL;
    cropTop = oldCropT;
    cropWidth = oldCropW;
    cropHeight = oldCropH;
}

// Get a human readable time representation of an epoch time (seconds since 1970)
// If time is invalid, this will be an empty string
std::string InkHUD::Applet::getTimeString(uint32_t epochSeconds)
{
#ifdef BUILD_EPOCH
    constexpr uint32_t validAfterEpoch = BUILD_EPOCH - (SEC_PER_DAY * 30 * 6); // 6 Months prior to build
#else
    constexpr uint32_t validAfterEpoch = 1738368000 - (SEC_PER_DAY * 30 * 6); // 6 Months prior to Feb 1, 2025 12:00:00 AM GMT
#endif

    uint32_t epochNow = getValidTime(RTCQuality::RTCQualityDevice, true);

    int32_t daysAgo = (epochNow - epochSeconds) / SEC_PER_DAY;
    int32_t hoursAgo = (epochNow - epochSeconds) / SEC_PER_HOUR;

    // Times are invalid: rtc is much older than when code was built
    // Don't give any human readable string
    if (epochNow <= validAfterEpoch)
        return "";

    // Times are invalid: argument time is significantly ahead of RTC
    // Don't give any human readable string
    if (daysAgo < -2)
        return "";

    // Times are probably invalid: more than 6 months ago
    if (daysAgo > 6 * 30)
        return "";

    if (daysAgo > 1)
        return to_string(daysAgo) + " days ago";

    else if (hoursAgo > 18)
        return "Yesterday";

    else {

        uint32_t hms = epochSeconds % SEC_PER_DAY;
        hms = (hms + SEC_PER_DAY) % SEC_PER_DAY;

        // Tear apart hms into h:m
        uint32_t hour = hms / SEC_PER_HOUR;
        uint32_t min = (hms % SEC_PER_HOUR) / SEC_PER_MIN;

        // Format the clock string, either 12 hour or 24 hour
        char clockStr[11];
        if (config.display.use_12h_clock)
            sprintf(clockStr, "%u:%02u %s", (hour % 12 == 0 ? 12 : hour % 12), min, hour > 11 ? "PM" : "AM");
        else
            sprintf(clockStr, "%02u:%02u", hour, min);

        return clockStr;
    }
}

// If no argument specified, get time string for the current RTC time
std::string InkHUD::Applet::getTimeString()
{
    return getTimeString(getValidTime(RTCQuality::RTCQualityDevice, true));
}

// Calculate how many nodes have been seen within our preferred window of activity
// This period is set by user, via the menu
// Todo: optimize to calculate once only per WindowManager::render
uint16_t InkHUD::Applet::getActiveNodeCount()
{
    // Don't even try to count nodes if RTC isn't set
    // The last heard values in nodedb will be incomprehensible
    if (getRTCQuality() == RTCQualityNone)
        return 0;

    uint16_t count = 0;

    // For each node in db
    for (uint16_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);

        // Check if heard recently, and not our own node
        if (sinceLastSeen(node) < settings->recentlyActiveSeconds && node->num != nodeDB->getNodeNum())
            count++;
    }

    return count;
}

// Get an abbreviated, human readable, distance string
// Honors config.display.units, to offer both metric and imperial
std::string InkHUD::Applet::localizeDistance(uint32_t meters)
{
    constexpr float FEET_PER_METER = 3.28084;
    constexpr uint16_t FEET_PER_MILE = 5280;

    // Resulting string
    std::string localized;

    // Imperial
    if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) {
        uint32_t feet = meters * FEET_PER_METER;
        // Distant (miles, rounded)
        if (feet > FEET_PER_MILE / 2) {
            localized += to_string((uint32_t)roundf(feet / FEET_PER_MILE));
            localized += "mi";
        }
        // Nearby (feet)
        else {
            localized += to_string(feet);
            localized += "ft";
        }
    }

    // Metric
    else {
        // Distant (kilometers, rounded)
        if (meters >= 500) {
            localized += to_string((uint32_t)roundf(meters / 1000.0));
            localized += "km";
        }
        // Nearby (meters)
        else {
            localized += to_string(meters);
            localized += "m";
        }
    }

    return localized;
}

// Print text with a "faux bold" effect, by drawing it multiple times, offsetting slightly
void InkHUD::Applet::printThick(int16_t xCenter, int16_t yCenter, std::string text, uint8_t thicknessX, uint8_t thicknessY)
{
    // How many times to draw along x axis
    int16_t xStart;
    int16_t xEnd;
    switch (thicknessX) {
    case 0:
        assert(false);
    case 1:
        xStart = xCenter;
        xEnd = xCenter;
        break;
    case 2:
        xStart = xCenter;
        xEnd = xCenter + 1;
        break;
    default:
        xStart = xCenter - (thicknessX / 2);
        xEnd = xCenter + (thicknessX / 2);
    }

    // How many times to draw along Y axis
    int16_t yStart;
    int16_t yEnd;
    switch (thicknessY) {
    case 0:
        assert(false);
    case 1:
        yStart = yCenter;
        yEnd = yCenter;
        break;
    case 2:
        yStart = yCenter;
        yEnd = yCenter + 1;
        break;
    default:
        yStart = yCenter - (thicknessY / 2);
        yEnd = yCenter + (thicknessY / 2);
    }

    // Print multiple times, overlapping
    for (int16_t x = xStart; x <= xEnd; x++) {
        for (int16_t y = yStart; y <= yEnd; y++) {
            printAt(x, y, text, CENTER, MIDDLE);
        }
    }
}

// Allow this applet to suppress notifications
// Asked before a notification is shown via the NotificationApplet
// An applet might want to suppress a notification if the applet itself already displays this info
// Example: AllMessageApplet should not approve notifications for messages, if it is in foreground
bool InkHUD::Applet::approveNotification(NicheGraphics::InkHUD::Notification &n)
{
    // By default, no objection
    return true;
}

// Draw the standard header, used by most Applets
/*
┌───────────────────────────────┐
│ Applet::name here             │
│ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ │
│                               │
│                               │
│                               │
└───────────────────────────────┘
*/
void InkHUD::Applet::drawHeader(std::string text)
{
    // Y position for divider
    // - between header text and messages
    constexpr int16_t padDivH = 2;
    const int16_t headerDivY = padDivH + fontSmall.lineHeight() + padDivH - 1;

    // Print header
    printAt(0, padDivH, text);

    // Divider
    // - below header text: separates message
    // - above header text: separates other applets
    for (int16_t x = 0; x < width(); x += 2) {
        drawPixel(x, 0, BLACK);
        drawPixel(x, headerDivY, BLACK); // Dotted 50%
    }
}

// Get the height of the standard applet header
// This will vary, depending on font
// Applets use this value to avoid drawing overtop the header
uint16_t InkHUD::Applet::getHeaderHeight()
{
    // Y position for divider
    // - between header text and messages
    constexpr int16_t padDivH = 2;
    const int16_t headerDivY = padDivH + fontSmall.lineHeight() + padDivH - 1;

    return headerDivY + 1; // "Plus one": height is always one more than Y position
}

// "Scale to fit": width of Meshtastic logo to fit given region, maintaining aspect ratio
uint16_t InkHUD::Applet::getLogoWidth(uint16_t limitWidth, uint16_t limitHeight)
{
    // Determine whether we're limited by width or height
    // Makes sure we draw the logo as large as possible, within the specified region,
    // while still maintaining correct aspect ratio
    if (limitWidth > limitHeight * LOGO_ASPECT_RATIO)
        return limitHeight * LOGO_ASPECT_RATIO;
    else
        return limitWidth;
}

// "Scale to fit": height of Meshtastic logo to fit given region, maintaining aspect ratio
uint16_t InkHUD::Applet::getLogoHeight(uint16_t limitWidth, uint16_t limitHeight)
{
    // Determine whether we're limited by width or height
    // Makes sure we draw the logo as large as possible, within the specified region,
    // while still maintaining correct aspect ratio
    if (limitHeight > limitWidth / LOGO_ASPECT_RATIO)
        return limitWidth / LOGO_ASPECT_RATIO;
    else
        return limitHeight;
}

// Draw a scalable Meshtastic logo
// Make sure to provide dimensions which have the correct aspect ratio (~2)
// Three paths, drawn thick using quads, with one corner "radiused"
/*
        -        ^
       /-       /-\
      //       // \\
     //       //   \\
    //       //     \\
   //       //       \\

*/
void InkHUD::Applet::drawLogo(int16_t centerX, int16_t centerY, uint16_t width, uint16_t height, Color color)
{
    struct Point {
        int x;
        int y;
    };
    typedef Point Distance;

    int16_t logoTh = width * 0.068; // Thickness scales with width. Measured from logo at meshtastic.org.
    int16_t logoL = centerX - (width / 2) + (logoTh / 2);
    int16_t logoT = centerY - (height / 2) + (logoTh / 2);
    int16_t logoW = width - logoTh;
    int16_t logoH = height - logoTh;
    int16_t logoR = logoL + logoW - 1;
    int16_t logoB = logoT + logoH - 1;

    // Points for paths (a, b, and c)
    /*
      +-----------------------------+
    --|          a2       b2/c1     |
      |                             |
      |                             |
      |                             |
    --|  a1      b1              c2 |
      +-----------------------------+
         |       |       |       |
    */

    Point a1 = {map(0, 0, 3, logoL, logoR), logoB};
    Point a2 = {map(1, 0, 3, logoL, logoR), logoT};
    Point b1 = {map(1, 0, 3, logoL, logoR), logoB};
    Point b2 = {map(2, 0, 3, logoL, logoR), logoT};
    Point c1 = {map(2, 0, 3, logoL, logoR), logoT};
    Point c2 = {map(3, 0, 3, logoL, logoR), logoB};

    // Find angle of the path(s)
    // Used to thicken the single pixel paths
    /*
    +-------------------------------+
    |             a2                |
    |            -|                 |
    |          -/ |                 |
    |        -/   |                 |
    |      -/#    |                 |
    |    -/   #   |                 |
    |   /     #   |                 |
    |  a1----------                 |
    +-------------------------------+
    */

    Distance deltaA = {abs(a2.x - a1.x), abs(a2.y - a1.y)};
    float angle = tanh((float)deltaA.y / deltaA.x);

    // Distance (at right angle to the paths), which will give corners for our "quads"
    // The distance is unsigned. We will vary the signedness of the x and y components to suit the path and corner
    /*
    |                             a2
    |                            .
    |                          ..
    |          aq1           ..
    |            #         ..
    |            | #     ..
    |fromPath.y  |   # ..
    |            +----a1
    |
    |          fromPath.x
    +--------------------------------
    */

    Distance fromPath;
    fromPath.x = cos(radians(90) - angle) * logoTh * 0.5;
    fromPath.y = sin(radians(90) - angle) * logoTh * 0.5;

    // Make the paths thick
    // Corner points for the rectangles (quads):
    /*

          aq2
               a2
               /    aq3
              /
             /
     aq1    /
          a1
                aq3
    */

    // Filled as two triangles per quad:
    /*
                  aq2 #
                 #     ###
               ##         # aq3
             ##       ###   -
           ##     ####    -/
         ##    ###      -/
       ##  ####       -/
     aq1 ##         -/
        ---       -/
           \---aq4
    */

    // Make the path thick: path a becomes quad a
    Point aq1{a1.x - fromPath.x, a1.y - fromPath.y};
    Point aq2{a2.x - fromPath.x, a2.y - fromPath.y};
    Point aq3{a2.x + fromPath.x, a2.y + fromPath.y};
    Point aq4{a1.x + fromPath.x, a1.y + fromPath.y};
    fillTriangle(aq1.x, aq1.y, aq2.x, aq2.y, aq3.x, aq3.y, color);
    fillTriangle(aq1.x, aq1.y, aq3.x, aq3.y, aq4.x, aq4.y, color);

    // Make the path thick: path b becomes quad b
    Point bq1{b1.x - fromPath.x, b1.y - fromPath.y};
    Point bq2{b2.x - fromPath.x, b2.y - fromPath.y};
    Point bq3{b2.x + fromPath.x, b2.y + fromPath.y};
    Point bq4{b1.x + fromPath.x, b1.y + fromPath.y};
    fillTriangle(bq1.x, bq1.y, bq2.x, bq2.y, bq3.x, bq3.y, color);
    fillTriangle(bq1.x, bq1.y, bq3.x, bq3.y, bq4.x, bq4.y, color);

    // Make the path thick: path c becomes quad c
    Point cq1{c1.x - fromPath.x, c1.y + fromPath.y};
    Point cq2{c2.x - fromPath.x, c2.y + fromPath.y};
    Point cq3{c2.x + fromPath.x, c2.y - fromPath.y};
    Point cq4{c1.x + fromPath.x, c1.y - fromPath.y};
    fillTriangle(cq1.x, cq1.y, cq2.x, cq2.y, cq3.x, cq3.y, color);
    fillTriangle(cq1.x, cq1.y, cq3.x, cq3.y, cq4.x, cq4.y, color);

    // Radius the intersection of quad b and quad c
    /*
       b2 / c1
              ####
           ##      ##
          /          \
         /     \/     \
        /      /\      \
       /      /  \      \

    */

    // Don't attempt if logo is tiny
    if (logoTh > 3) {
        // The radius for the cap *should* be the same as logoTh, but it's not, due to accumulated rounding
        // We get better results just re-deriving it
        int16_t capRad = sqrt(pow(fromPath.x, 2) + pow(fromPath.y, 2));
        fillCircle(b2.x, b2.y, capRad, color);
    }
}

#endif