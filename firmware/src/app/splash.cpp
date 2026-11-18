#include "recorder/recorder_app.h"

#include "recorder/app/app_shared.h"

namespace cardputer_recorder {

namespace {

constexpr unsigned long kSplashMinMs = 1000;

}  // namespace

// Boot screen: drawn as soon as the display is ready so it is on screen
// while the microSD card mounts, then held for at least kSplashMinMs. Any key
// skips the wait (the work behind it, mounting and scanning, still runs).
void RecorderApp::drawSplash(const char* status, unsigned long startedMs)
{
    constexpr std::uint16_t background = 0x0841;
    constexpr std::uint16_t panel = 0x10C3;
    constexpr std::uint16_t muted = 0x8410;
    constexpr std::uint16_t accent = 0x05FF;

    auto& display = recorderCanvas;
    const int w = display.width();
    const int h = display.height();
    display.fillSprite(background);

    // Record button: red dot in a ring.
    display.fillCircle(w / 2, 36, 21, panel);
    display.drawCircle(w / 2, 36, 21, accent);
    display.fillCircle(w / 2, 36, 12, TFT_RED);

    display.setTextDatum(middle_center);
    display.setTextFont(4);
    display.setTextColor(TFT_WHITE, background);
    display.drawString("Recorder", w / 2, 78);
    display.setTextFont(2);
    display.setTextColor(accent, background);
    display.drawString("Cardputer ADV", w / 2, 98);

    // Thin progress bar over the minimum display time.
    const unsigned long elapsed = millis() - startedMs;
    const int barWidth = w - 40;
    display.drawRect(20, 112, barWidth, 3, panel);
    display.fillRect(20, 112,
                     static_cast<int>(barWidth * min(elapsed, kSplashMinMs) /
                                      kSplashMinMs),
                     3, accent);

    display.setTextFont(1);
    display.setTextColor(muted, background);
    display.setTextDatum(bottom_left);
    display.drawString(kAppVersion, 6, h - 3);
    display.setTextDatum(bottom_right);
    display.drawString(status, w - 6, h - 3);
    display.setTextDatum(top_left);
    display.pushSprite(0, 0);
}

void RecorderApp::holdSplash(const char* status, unsigned long startedMs)
{
    while (millis() - startedMs < kSplashMinMs) {
        board_.update();
        if (M5Cardputer.Keyboard.isChange() &&
            M5Cardputer.Keyboard.isPressed()) {
            break;
        }
        drawSplash(status, startedMs);
        delay(30);
    }
}

}  // namespace cardputer_recorder
