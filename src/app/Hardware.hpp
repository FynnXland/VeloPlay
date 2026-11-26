#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "app/Config.hpp"

// What the PC VeloPlay runs on can do, and which picture settings suit it. The shell of each platform
// describes the machine once (Windows: from the Direct3D adapter, Linux: from the OpenGL strings and
// /sys); the rest is plain logic and runs in the tests.
//
// Every threshold here is an estimate. Measured is only the RTX 4070 of the author (frame generation
// about 4 ms per picture at 1080p). Integrated graphics have a fraction of that card's compute, so
// what costs 4 ms there can cost 20 ms and more on a laptop, more than a picture at 60 Hz may take.

namespace veloplay {

struct HardwareInfo {
    enum class Os { Windows, Linux };
    enum class GpuVendor { Other, Nvidia, Amd, Intel };

    Os os = Os::Windows;
    /// The graphics as the driver names it, without version tails ("NVIDIA GeForce RTX 4070",
    /// "Intel Iris Xe Graphics"). Empty when unknown.
    std::string gpuName;
    /// The name exactly as the driver gave it, tails included; Mesa names the chip there ("radeonsi, phoenix").
    /// Only for telling chips apart, never shown and not part of the fingerprint.
    std::string gpuDriverName;
    GpuVendor vendor = GpuVendor::Other;
    /// Graphics in the processor, sharing the system memory (laptops, office PCs, handhelds).
    bool integratedGpu = false;
    /// Memory of the graphics card itself in MiB; 0 = unknown (the Linux driver of NVIDIA does not tell).
    std::uint64_t videoMemoryMb = 0;
    /// No graphics driver draws, the processor does (WARP on Windows; llvmpipe, softpipe or SwiftShader
    /// on Linux). Everything that costs compute is too much then.
    bool softwareRenderer = false;
    /// The video is decoded on the graphics card (D3D11VA on Windows). False: the processor unpacks every
    /// picture itself, as the Linux client does today.
    bool hardwareVideoDecoding = true;
    std::string cpuName;          // empty = unknown
    int cpuThreads = 0;           // logical processors; 0 = unknown
    std::uint64_t systemMemoryMb = 0; // 0 = unknown
    /// The screen the window opens on; zeros = unknown.
    int screenWidth = 0;
    int screenHeight = 0;
    double screenHz = 0.0;
    /// A battery is built in (a laptop or a handheld) and whether it runs on it right now. Only shown,
    /// never part of the choice: plugging in would change the advice from one minute to the next.
    bool hasBattery = false;
    bool onBattery = false;

    friend bool operator==(const HardwareInfo&, const HardwareInfo&) = default;
};

/// How much picture work the graphics can take on top of showing the stream.
enum class GraphicsTier {
    Software,          // the processor draws: nothing extra
    WeakIntegrated,    // older or small integrated graphics (Intel UHD, two-unit Radeon): nothing extra
    CapableIntegrated, // Iris Xe, Radeon 680M/780M and alike, or a small card: the cheap parts, frame generation with care
    Dedicated,         // a graphics card of its own: everything, as on the author's PC
};

/// What the program can do on this platform at all. The shells fill it; the defaults are what the
/// Windows client offers, so a shell that does not set it behaves as before.
struct PlatformCaps {
    HardwareInfo::Os os = HardwareInfo::Os::Windows;
    bool frameGeneration = true; // pictures computed in between
    bool upscaling = true;       // Lanczos and sharpening
    bool deband = true;          // smoothing the steps in gradients
    bool looks = true;           // picture styles
    bool amdFrc = true;          // the build carries AMD FRC (an AMD card is still needed)
    bool captureCards = true;    // video sources of this PC can be shown
    /// Sony's sign-in opens in a window of the program and the answer is read from it. False: it opens in
    /// the browser, and the address the browser ends on is pasted by hand.
    bool embeddedSignIn = true;

    friend bool operator==(const PlatformCaps&, const PlatformCaps&) = default;
};

/// The picture settings that suit a machine.
struct HardwareAdvice {
    GraphicsTier tier = GraphicsTier::Dedicated;
    GraphicsSettings graphics;
    /// "Automatic" asks the console for at most 720p: the processor unpacks the video and is slow.
    bool streamUpTo720p = false;

    friend bool operator==(const HardwareAdvice&, const HardwareAdvice&) = default;
};

/// Whether a card of this maker and name is part of the processor. `videoMemoryMb` 0 means unknown.
/// Intel is integrated unless it is an Arc card (A380, B580); AMD where the name is one of an APU;
/// anything with less than 1 GiB of its own memory as well.
[[nodiscard]] bool looksIntegrated(HardwareInfo::GpuVendor vendor, std::string_view name, std::uint64_t videoMemoryMb);

/// The maker from the PCI vendor id (0x10DE, 0x1002, 0x8086).
[[nodiscard]] HardwareInfo::GpuVendor vendorFromPci(unsigned vendorId);

/// The tier of a machine. Unknown values never lower it: a machine nothing is known about is treated as
/// the author's.
[[nodiscard]] GraphicsTier tierOf(const HardwareInfo& info);

/// The settings for this machine. A machine nothing is known about, and the author's (RTX 4070 on
/// Windows, a 1080p screen), get exactly the defaults of GraphicsSettings.
[[nodiscard]] HardwareAdvice recommendedSettings(const HardwareInfo& info);

/// Puts the advice into the settings: the picture settings and the limit for "automatic". Nothing else.
void applyAdvice(AppConfig& config, const HardwareAdvice& advice);

/// Whether the settings are those of the advice.
[[nodiscard]] bool followsAdvice(const AppConfig& config, const HardwareAdvice& advice);

/// What the settings remember the machine by: the system and the graphics. Not the screen or the battery,
/// which change without the machine changing; not the driver version, which updates change.
[[nodiscard]] std::string hardwareFingerprint(const HardwareInfo& info);
/// The name of the graphics a fingerprint was made of.
[[nodiscard]] std::string fingerprintGpu(std::string_view fingerprint);
/// Whether a fingerprint stands for the software renderer.
[[nodiscard]] bool fingerprintIsSoftware(std::string_view fingerprint);

/// One setting the advice sets, for the screens to put into words.
struct AdviceLine {
    enum class Setting { FrameGeneration, Upscaling, Sharpness, Deband, Look, AmdFrc, Vsync, StreamUpTo720p };
    Setting setting = Setting::FrameGeneration;
    /// The value: the factor (0 automatic, 1 off) for frame generation, the level (0 off, 1 Lanczos,
    /// 2 RTX Video) for upscaling, percent for sharpness, 0 or 1 for the switches.
    int value = 0;

    friend bool operator==(const AdviceLine&, const AdviceLine&) = default;
};

/// What the advice sets, as far as the platform has it at all. With `current`, only what differs from
/// it (for the question before the settings are put back). `rtxVideo`: the card offers RTX Video; level 2
/// means Lanczos without it and is named so. AMD FRC and the picture style are named only where they are
/// switched on now, and VSync where it is off: the advice keeps the defaults there.
[[nodiscard]] std::vector<AdviceLine> adviceLines(const HardwareAdvice& advice, const PlatformCaps& caps,
                                                  bool rtxVideo, const AppConfig* current = nullptr);

/// A graphics name as drivers give it, made readable and stable: "(R)" and "(TM)" go, and so do
/// version tails after " (" or "/" ("llvmpipe (LLVM 17.0.6, 256 bits)", "NVIDIA GeForce RTX 4070/PCIe/SSE2").
[[nodiscard]] std::string cleanGpuName(std::string_view raw);

/// The machine as Linux shows it. `glVendor` and `glRenderer` are what the OpenGL context reports
/// (GL_VENDOR, GL_RENDERER); the rest is read under `root`: /proc/cpuinfo, /proc/meminfo,
/// /sys/class/drm/card*/device/{vendor,boot_vga,mem_info_vram_total}, /sys/class/power_supply/*.
/// The screen is not known here; the shell sets it. Video counts as decoded on the processor.
[[nodiscard]] HardwareInfo describeLinux(std::string_view glVendor, std::string_view glRenderer,
                                         const std::filesystem::path& root = "/");

/// The graphics part of a description from what Windows says about the adapter (DXGI_ADAPTER_DESC1):
/// PCI vendor, name, own memory in bytes, and whether it is the software renderer. The shell adds
/// the processor, the memory, the screen and the battery.
[[nodiscard]] HardwareInfo describeAdapter(HardwareInfo::Os os, unsigned pciVendor, std::string_view name,
                                           std::uint64_t videoMemoryBytes, bool software);

} // namespace veloplay
