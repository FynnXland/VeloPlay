#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace veloplay {

/// Live figures of a running stream, shown by the in-stream display. Filled by
/// the session and the render pipeline once per frame.
struct StreamStats {
    /// False while the stream is received but not decoded and shown. The
    /// display then leaves out everything that describes the picture.
    bool presenting = false;
    float streamFps = 0.0f;         // frames per second arriving from the console
    float outputFps = 0.0f;         // frames per second shown on the monitor
    int gameFps = 0;                // rate of the game where it is below the stream's (30 in a 60 fps stream); 0 otherwise
    int inputWidth = 0;             // size of the picture as it arrives; 0 until the first one
    int inputHeight = 0;
    int outputWidth = 0;            // size of the picture on screen; 0 until the first one is drawn
    int outputHeight = 0;
    float insidePcMs = 0.0f;        // from a picture's data arriving to it being drawn; 0 until measured
    std::string scaler;             // how the picture is enlarged: "RTX Video", "Lanczos" or empty
    bool frameGenerationAvailable = true; // false if the graphics card could not set it up
    int frameGenerationFactor = 1;  // 1 = off
    bool frameGenerationActive = false; // false while paused, e.g. for duplicate frames
    std::string frameGenerationEngine;  // short name of the method in use
    float bitrateMbps = 0.0f;
    float roundTripMs = 0.0f;       // network round trip to the console
    float packetLossPercent = 0.0f;
    float addedLatencyMs = 0.0f;    // delay caused by frame generation
    float decodeMs = 0.0f;
    float decodeMaxMs = 0.0f;       // the slowest picture of the last one to two seconds
    std::string decoder;            // for example "HEVC, D3D11VA"
    std::uint64_t droppedFrames = 0; // thrown away because the decoder fell behind
    /// Time between presented frames in ms, oldest first. Even bars mean smooth motion.
    std::vector<float> frameTimes;
};

} // namespace veloplay
