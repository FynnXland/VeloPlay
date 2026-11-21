#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

#include "app/StreamAlerts.hpp"

using namespace veloplay;
using namespace std::chrono_literals;

namespace {

using Clock = StreamAlerts::Clock;

bool shows(const StreamAlerts& alerts, StreamAlert alert, Clock::time_point now) {
    const auto active = alerts.active(now);
    return std::find(active.begin(), active.end(), alert) != active.end();
}

/// A stream in which nothing is wrong.
StreamObservation fine() {
    StreamObservation o;
    o.sincePicture = 16ms;
    o.streamFps = 60.0;
    o.requestedFps = 60;
    return o;
}

} // namespace

TEST(StreamAlerts, SayNothingWhileAllIsWell) {
    StreamAlerts alerts;
    Clock::time_point now{};
    alerts.reset(now);
    for (int i = 0; i < 600; ++i, now += 16ms) {
        alerts.update(fine(), now);
        EXPECT_TRUE(alerts.active(now).empty());
        EXPECT_TRUE(alerts.raised().empty());
    }
}

TEST(StreamAlerts, ShowAStallWhileItLastsAndALittleLonger) {
    StreamAlerts alerts;
    Clock::time_point now{};
    alerts.reset(now);
    for (int i = 0; i < 300; ++i, now += 16ms) alerts.update(fine(), now);

    // 200 ms without a picture is within what Wi-Fi does all the time.
    StreamObservation o = fine();
    o.sincePicture = 200ms;
    alerts.update(o, now);
    EXPECT_FALSE(shows(alerts, StreamAlert::ConnectionStalls, now));

    // 400 ms is a hitch one sees.
    o.sincePicture = 400ms;
    now += 200ms;
    alerts.update(o, now);
    EXPECT_TRUE(shows(alerts, StreamAlert::ConnectionStalls, now));
    ASSERT_EQ(alerts.raised().size(), 1u); // newly there: once for the log
    EXPECT_EQ(alerts.raised()[0], StreamAlert::ConnectionStalls);
    now += 16ms;
    alerts.update(o, now);
    EXPECT_TRUE(alerts.raised().empty());

    // Pictures come again. The note stays for a moment, so it can be read, then goes.
    for (int i = 0; i < 60; ++i, now += 16ms) alerts.update(fine(), now);
    EXPECT_TRUE(shows(alerts, StreamAlert::ConnectionStalls, now));
    for (int i = 0; i < 120; ++i, now += 16ms) alerts.update(fine(), now);
    EXPECT_FALSE(shows(alerts, StreamAlert::ConnectionStalls, now));
}

TEST(StreamAlerts, IgnoreTheFirstMomentsOfAStream) {
    StreamAlerts alerts;
    Clock::time_point now{};
    alerts.reset(now);
    StreamObservation o = fine();
    o.sincePicture = 900ms; // the first picture takes its time
    o.streamFps = 4.0;
    for (int i = 0; i < 100; ++i, now += 16ms) alerts.update(o, now);
    EXPECT_TRUE(alerts.active(now).empty());
}

TEST(StreamAlerts, TellAFewLostPicturesFromMany) {
    StreamAlerts alerts;
    Clock::time_point now{};
    alerts.reset(now);
    StreamObservation o = fine();
    o.framesLost = 40; // from before; the count does not start at zero after a reconnect
    for (int i = 0; i < 300; ++i, now += 16ms) alerts.update(o, now);
    EXPECT_TRUE(alerts.active(now).empty());

    // One lost picture every few seconds: repaired unseen, not worth a note.
    for (int round = 0; round < 4; ++round) {
        ++o.framesLost;
        for (int i = 0; i < 400; ++i, now += 16ms) alerts.update(o, now);
    }
    EXPECT_FALSE(shows(alerts, StreamAlert::PicturesLost, now));

    // Several within a moment show.
    o.framesLost += 2;
    alerts.update(o, now);
    now += 500ms;
    o.framesLost += 2;
    alerts.update(o, now);
    EXPECT_TRUE(shows(alerts, StreamAlert::PicturesLost, now));
    for (int i = 0; i < 700; ++i, now += 16ms) alerts.update(o, now);
    EXPECT_FALSE(shows(alerts, StreamAlert::PicturesLost, now));
}

TEST(StreamAlerts, SayWhenThisPcFallsBehind) {
    StreamAlerts alerts;
    Clock::time_point now{};
    alerts.reset(now);
    StreamObservation o = fine();
    for (int i = 0; i < 300; ++i, now += 16ms) alerts.update(o, now);
    o.framesDropped = 8;
    alerts.update(o, now);
    EXPECT_TRUE(shows(alerts, StreamAlert::DecoderBehind, now));
    EXPECT_FALSE(shows(alerts, StreamAlert::PicturesLost, now));
}

TEST(StreamAlerts, MissAControllerOnlyWhenOneIsWanted) {
    StreamAlerts alerts;
    Clock::time_point now{};
    alerts.reset(now);
    StreamObservation o = fine();
    o.controllerConnected = false;
    // A controller that reconnects is gone for a moment; that is not worth a note.
    for (int i = 0; i < 60; ++i, now += 16ms) alerts.update(o, now);
    EXPECT_FALSE(shows(alerts, StreamAlert::NoController, now));
    // Gone for good: said even in the first seconds, and for as long as it lasts.
    for (int i = 0; i < 60; ++i, now += 16ms) alerts.update(o, now);
    EXPECT_TRUE(shows(alerts, StreamAlert::NoController, now));
    for (int i = 0; i < 2000; ++i, now += 16ms) alerts.update(o, now);
    EXPECT_TRUE(shows(alerts, StreamAlert::NoController, now));
    // It is the most pressing note.
    EXPECT_EQ(alerts.active(now).front(), StreamAlert::NoController);

    // Plugged in: gone at once.
    o.controllerConnected = true;
    alerts.update(o, now);
    EXPECT_FALSE(shows(alerts, StreamAlert::NoController, now));

    // Something else steers: no controller is not a fault then.
    o.controllerConnected = false;
    o.controllerWanted = false;
    for (int i = 0; i < 300; ++i, now += 16ms) alerts.update(o, now);
    EXPECT_FALSE(shows(alerts, StreamAlert::NoController, now));
}

TEST(StreamAlerts, SayOnceWhenTheConsoleSendsFewerPictures) {
    StreamAlerts alerts;
    Clock::time_point now{};
    alerts.reset(now);
    for (int i = 0; i < 300; ++i, now += 16ms) alerts.update(fine(), now);

    // A second at 40 fps is nothing.
    StreamObservation o = fine();
    o.streamFps = 40.0;
    o.sincePicture = 25ms;
    for (int i = 0; i < 60; ++i, now += 16ms) alerts.update(o, now);
    EXPECT_FALSE(shows(alerts, StreamAlert::FewerPictures, now));
    // Several seconds are.
    for (int i = 0; i < 200; ++i, now += 16ms) alerts.update(o, now);
    EXPECT_TRUE(shows(alerts, StreamAlert::FewerPictures, now));
    // It stays that way for minutes: the note does not.
    for (int i = 0; i < 1000; ++i, now += 16ms) alerts.update(o, now);
    EXPECT_FALSE(shows(alerts, StreamAlert::FewerPictures, now));

    // With pictures getting lost it is the network, and the other note says so.
    StreamAlerts lossy;
    now = {};
    lossy.reset(now);
    o.framesLost = 0;
    for (int i = 0; i < 300; ++i, now += 16ms) lossy.update(o, now);
    for (int i = 0; i < 300; ++i, now += 16ms) {
        if (i % 20 == 0) o.framesLost += 2;
        lossy.update(o, now);
    }
    EXPECT_TRUE(shows(lossy, StreamAlert::PicturesLost, now));
    EXPECT_FALSE(shows(lossy, StreamAlert::FewerPictures, now));
}
