#include "app/Hardware.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

namespace veloplay {

namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024ull;

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool contains(std::string_view haystack, std::string_view needle) { return haystack.find(needle) != std::string_view::npos; }

bool containsAny(std::string_view haystack, std::initializer_list<std::string_view> needles) {
    return std::any_of(needles.begin(), needles.end(), [&](std::string_view needle) { return contains(haystack, needle); });
}

std::string stripped(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

/// Lower case without the marks drivers sprinkle in, so "Intel(R) Iris(R) Xe" and "Intel Iris Xe" compare alike.
std::string plain(std::string_view raw) {
    std::string text = lower(raw);
    for (const char* mark : {"(r)", "(tm)", "(c)"}) {
        for (std::size_t at = text.find(mark); at != std::string::npos; at = text.find(mark)) {
            text.erase(at, std::string_view(mark).size());
        }
    }
    return text;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

/// Intel's graphics cards of their own: Arc A380 to A770, B570, B580. "Arc Graphics" and "Arc 140V" are in the processor.
bool intelCard(const std::string& name) {
    static const std::regex kCard("arc.*\\b[ab][0-9]{3}\\b");
    return std::regex_search(name, kCard);
}

/// Names AMD gives the graphics in its processors, on Windows and in Mesa's renderer string (which names the chip).
bool amdProcessorGraphics(const std::string& name) {
    if (contains(name, "vega") && !contains(name, "rx ") && !contains(name, "frontier")) return true; // Vega 8, not RX Vega 64
    static const std::regex kMobile("\\b[0-9]{3}m\\b");          // 680M, 780M, 890M; cards have four digits (RX 6800M)
    static const std::regex kHalo("\\b[0-9]{4}s\\b");            // Radeon 8060S
    static const std::regex kOld("radeon r[2-7] graphics"); // the APUs of the years before Ryzen
    if (std::regex_search(name, kMobile) || std::regex_search(name, kHalo) || std::regex_search(name, kOld)) return true;
    if (contains(name, "amd radeon graphics") || name == "radeon graphics") return true;
    return containsAny(name, {"renoir", "cezanne", "lucienne", "barcelo", "rembrandt", "phoenix", "raphael", "mendocino",
                              "vangogh", "van gogh", "picasso", "raven", "stoney", "carrizo", "kabini", "mullins",
                              "gfx1150", "gfx1151", "gfx1152", "strix"});
}

/// Integrated graphics that are known to be small. Anything not on these lists counts as capable: an unknown
/// name is more likely a new chip than an old one.
bool weakProcessorGraphics(HardwareInfo::GpuVendor vendor, const std::string& name) {
    using Vendor = HardwareInfo::GpuVendor;
    if (vendor == Vendor::Intel) {
        // UHD 620/630/770, HD 520, Iris Plus and Iris Pro of the generations before Xe.
        return containsAny(name, {"uhd", "hd graphics", "iris plus", "iris pro", "iris graphics", "gma"});
    }
    if (vendor == Vendor::Amd) {
        // Two compute units in Ryzen 7000 desktops and Mendocino, the first Vega APUs, and everything older.
        static const std::regex kOld("radeon r[2-7] graphics");
        return containsAny(name, {"raphael", "mendocino", "610m", "vega 3", "vega 6", "picasso", "raven", "stoney",
                                  "carrizo", "kabini", "mullins"}) ||
               std::regex_search(name, kOld);
    }
    return false;
}

} // namespace

HardwareInfo::GpuVendor vendorFromPci(unsigned vendorId) {
    switch (vendorId) {
        case 0x10DE: return HardwareInfo::GpuVendor::Nvidia;
        case 0x1002:
        case 0x1022: return HardwareInfo::GpuVendor::Amd;
        case 0x8086: return HardwareInfo::GpuVendor::Intel;
        default: return HardwareInfo::GpuVendor::Other;
    }
}

bool looksIntegrated(HardwareInfo::GpuVendor vendor, std::string_view rawName, std::uint64_t videoMemoryMb) {
    using Vendor = HardwareInfo::GpuVendor;
    const std::string name = plain(rawName);
    const bool memoryKnown = videoMemoryMb > 0;
    switch (vendor) {
        case Vendor::Nvidia: return false;
        // Integrated Intel graphics report 128 MiB or so of their own; an Arc card has gigabytes.
        case Vendor::Intel: return !intelCard(name) && !(memoryKnown && videoMemoryMb >= 2048);
        case Vendor::Amd: return amdProcessorGraphics(name) || (memoryKnown && videoMemoryMb < 1024);
        case Vendor::Other: break;
    }
    return memoryKnown && videoMemoryMb < 512;
}

GraphicsTier tierOf(const HardwareInfo& info) {
    if (info.softwareRenderer) return GraphicsTier::Software;
    if (info.integratedGpu) {
        const std::string name = plain(info.gpuName + " " + info.gpuDriverName);
        // Integrated graphics share the processor's memory and its power: with few cores or little memory
        // there is nothing left for extra work on the picture. Unknown (0) does not count as little.
        const bool fewThreads = info.cpuThreads > 0 && info.cpuThreads < 4;
        const bool littleMemory = info.systemMemoryMb > 0 && info.systemMemoryMb < 6 * 1024;
        if (fewThreads || littleMemory || weakProcessorGraphics(info.vendor, name)) return GraphicsTier::WeakIntegrated;
        return GraphicsTier::CapableIntegrated;
    }
    // A card of its own with under 1.5 GiB is old or small (GT 710, R5 230): about what Iris Xe can do.
    if (info.videoMemoryMb > 0 && info.videoMemoryMb < 1536) return GraphicsTier::CapableIntegrated;
    return GraphicsTier::Dedicated;
}

HardwareAdvice recommendedSettings(const HardwareInfo& info) {
    HardwareAdvice advice;
    advice.tier = tierOf(info);
    GraphicsSettings& g = advice.graphics; // starts from the defaults, which are the author's PC
    switch (advice.tier) {
        case GraphicsTier::Dedicated:
            // The defaults, Lanczos included. RTX Video is not chosen for anyone: it costs 8 to 10 ms per picture
            // when enlarging on an RTX 4070 (beside about 4 ms of frame generation), it was never measured on a
            // smaller card, and at the same size nobody saw a difference. It stays the user's switch.
            break;
        case GraphicsTier::CapableIntegrated:
            // Frame generation stays automatic: it is the reason to use VeloPlay, and on these chips it may
            // just fit (estimate: four to eight times the 4 ms of an RTX 4070). The screens say to switch it
            // off if the picture stutters. Enlarging onto more than 1440 lines costs more than it shows here.
            if (info.screenHeight > 1440) g.superResolutionLevel = 0;
            break;
        case GraphicsTier::WeakIntegrated:
        case GraphicsTier::Software:
            // Every pass over the picture costs time these chips do not have: only what the stream needs.
            g.frameGenerationFactor = 1;
            g.superResolutionLevel = 0;
            g.sharpness = 0.0f;
            g.deband = false;
            break;
    }
    // Decoding 1080p at 60 fps in software needs about two fast cores; with four threads or fewer, and
    // with the processor drawing the picture as well, 720p leaves room. Unknown (0) threads: no limit.
    const bool fewThreads = info.cpuThreads > 0 && info.cpuThreads <= 4;
    advice.streamUpTo720p = (!info.hardwareVideoDecoding || info.softwareRenderer) && fewThreads;
    return advice;
}

void applyAdvice(AppConfig& config, const HardwareAdvice& advice) {
    config.graphics = advice.graphics;
    config.autoUpTo720p = advice.streamUpTo720p;
}

bool followsAdvice(const AppConfig& config, const HardwareAdvice& advice) {
    const GraphicsSettings& a = config.graphics;
    const GraphicsSettings& b = advice.graphics;
    // The strength of a style that is off cannot be seen; it does not make the settings differ.
    const bool sameLook = a.look == b.look && (a.look == 0 || a.lookStrength == b.lookStrength);
    return a.frameGenerationFactor == b.frameGenerationFactor && a.superResolutionLevel == b.superResolutionLevel &&
           std::lround(a.sharpness * 100.0f) == std::lround(b.sharpness * 100.0f) && a.deband == b.deband && sameLook &&
           a.amdFrc == b.amdFrc && a.vsync == b.vsync && config.autoUpTo720p == advice.streamUpTo720p;
}

std::string hardwareFingerprint(const HardwareInfo& info) {
    const std::string os = info.os == HardwareInfo::Os::Linux ? "linux" : "windows";
    if (info.softwareRenderer) return os + "|software";
    return os + "|" + info.gpuName;
}

std::string fingerprintGpu(std::string_view fingerprint) {
    const std::size_t bar = fingerprint.find('|');
    if (bar == std::string_view::npos || fingerprintIsSoftware(fingerprint)) return {};
    return std::string(fingerprint.substr(bar + 1));
}

bool fingerprintIsSoftware(std::string_view fingerprint) {
    constexpr std::string_view kSoftware = "|software";
    return fingerprint.size() >= kSoftware.size() && fingerprint.substr(fingerprint.size() - kSoftware.size()) == kSoftware;
}

std::vector<AdviceLine> adviceLines(const HardwareAdvice& advice, const PlatformCaps& caps, bool rtxVideo,
                                    const AppConfig* current) {
    using Setting = AdviceLine::Setting;
    std::vector<AdviceLine> lines;
    const GraphicsSettings& g = advice.graphics;
    // `always`: named on the first start too. Without it a line only comes when it changes something.
    const auto add = [&](Setting setting, int value, int now, bool always) {
        if (current ? value != now : always) lines.push_back({setting, value});
    };
    const auto level = [&](int stored) { return stored == 2 && !rtxVideo ? 1 : stored; };
    const auto percent = [](float value) { return static_cast<int>(std::lround(value * 100.0f)); };
    const GraphicsSettings now = current ? current->graphics : GraphicsSettings{};
    if (caps.frameGeneration) add(Setting::FrameGeneration, g.frameGenerationFactor, now.frameGenerationFactor, true);
    if (caps.upscaling) {
        add(Setting::Upscaling, level(g.superResolutionLevel), level(now.superResolutionLevel), true);
        add(Setting::Sharpness, percent(g.sharpness), percent(now.sharpness), true);
    }
    if (caps.deband) add(Setting::Deband, g.deband ? 1 : 0, now.deband ? 1 : 0, true);
    if (caps.looks) add(Setting::Look, g.look, now.look, false);
    if (caps.amdFrc) add(Setting::AmdFrc, g.amdFrc ? 1 : 0, now.amdFrc ? 1 : 0, false);
    add(Setting::Vsync, g.vsync ? 1 : 0, now.vsync ? 1 : 0, false);
    add(Setting::StreamUpTo720p, advice.streamUpTo720p ? 1 : 0, current && current->autoUpTo720p ? 1 : 0,
        advice.streamUpTo720p);
    return lines;
}

std::string cleanGpuName(std::string_view raw) {
    std::string text(raw);
    for (const char* mark : {"(R)", "(r)", "(TM)", "(tm)", "(C)"}) {
        for (std::size_t at = text.find(mark); at != std::string::npos; at = text.find(mark)) {
            text.erase(at, std::string_view(mark).size());
        }
    }
    // Version tails: "llvmpipe (LLVM 17.0.6, 256 bits)", "AMD Radeon 780M (radeonsi, phoenix, LLVM …)",
    // "NVIDIA GeForce RTX 4070/PCIe/SSE2".
    if (const std::size_t at = text.find(" ("); at != std::string::npos) text.resize(at);
    if (const std::size_t at = text.find('/'); at != std::string::npos) text.resize(at);
    text = stripped(text);
    if (text.rfind("Mesa ", 0) == 0) text.erase(0, 5);
    std::string out;
    for (char c : text) {
        if (c == ' ' && !out.empty() && out.back() == ' ') continue;
        out += c;
    }
    return stripped(out);
}

HardwareInfo describeAdapter(HardwareInfo::Os os, unsigned pciVendor, std::string_view name,
                             std::uint64_t videoMemoryBytes, bool software) {
    HardwareInfo info;
    info.os = os;
    // 0x1414 is Microsoft: the Basic Render Driver (WARP) and the Basic Display Adapter without a real driver.
    info.softwareRenderer = software || pciVendor == 0x1414;
    info.vendor = info.softwareRenderer ? HardwareInfo::GpuVendor::Other : vendorFromPci(pciVendor);
    info.gpuName = cleanGpuName(name);
    info.gpuDriverName = std::string(name);
    info.videoMemoryMb = videoMemoryBytes / kMiB;
    info.integratedGpu = !info.softwareRenderer && looksIntegrated(info.vendor, name, info.videoMemoryMb);
    info.hardwareVideoDecoding = !info.softwareRenderer;
    return info;
}

HardwareInfo describeLinux(std::string_view glVendor, std::string_view glRenderer, const std::filesystem::path& root) {
    using Vendor = HardwareInfo::GpuVendor;
    HardwareInfo info;
    info.os = HardwareInfo::Os::Linux;
    // The Linux client unpacks the video on the processor; a shell with VA-API sets this to true.
    info.hardwareVideoDecoding = false;
    info.gpuName = cleanGpuName(glRenderer);
    info.gpuDriverName = stripped(glRenderer);

    const std::string both = plain(std::string(glRenderer) + " " + std::string(glVendor));
    info.softwareRenderer = containsAny(both, {"llvmpipe", "softpipe", "swiftshader", "software rasterizer", "swrast"});
    if (!info.softwareRenderer) {
        // The renderer names the chip; the vendor string of Mesa is often only "Mesa" or "X.Org".
        if (contains(both, "nvidia") || contains(both, "nouveau")) info.vendor = Vendor::Nvidia;
        else if (contains(both, "intel")) info.vendor = Vendor::Intel;
        else if (containsAny(both, {"amd", "radeon", "ati "})) info.vendor = Vendor::Amd;
    }

    // The card that draws: on a laptop with two, the one whose maker the OpenGL strings name. Only
    // "card0", "card1": the entries "card0-eDP-1" beside them are screens.
    std::error_code ec;
    const std::filesystem::path drm = root / "sys/class/drm";
    unsigned wanted = 0;
    if (info.vendor == Vendor::Nvidia) wanted = 0x10DE;
    if (info.vendor == Vendor::Amd) wanted = 0x1002;
    if (info.vendor == Vendor::Intel) wanted = 0x8086;
    bool foundPrimary = false;
    for (const auto& entry : std::filesystem::directory_iterator(drm, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() <= 4 || name.rfind("card", 0) != 0 ||
            !std::all_of(name.begin() + 4, name.end(), [](unsigned char c) { return std::isdigit(c); })) {
            continue;
        }
        const std::filesystem::path device = entry.path() / "device";
        const unsigned vendor = static_cast<unsigned>(std::strtoul(stripped(readFile(device / "vendor")).c_str(), nullptr, 16));
        if (wanted == 0 || vendor != wanted) continue;
        const bool primary = stripped(readFile(device / "boot_vga")) == "1";
        if (info.videoMemoryMb != 0 && foundPrimary && !primary) continue;
        const std::string vram = stripped(readFile(device / "mem_info_vram_total")); // amdgpu only, in bytes
        if (!vram.empty()) info.videoMemoryMb = std::strtoull(vram.c_str(), nullptr, 10) / kMiB;
        foundPrimary = foundPrimary || primary;
    }
    info.integratedGpu = !info.softwareRenderer && looksIntegrated(info.vendor, glRenderer, info.videoMemoryMb);

    {
        std::istringstream cpuinfo(readFile(root / "proc/cpuinfo"));
        std::string line;
        int processors = 0;
        while (std::getline(cpuinfo, line)) {
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = stripped(std::string_view(line).substr(0, colon));
            if (key == "processor") ++processors;
            if (key == "model name" && info.cpuName.empty()) info.cpuName = stripped(std::string_view(line).substr(colon + 1));
        }
        info.cpuThreads = processors;
    }
    {
        std::istringstream meminfo(readFile(root / "proc/meminfo"));
        std::string key;
        std::uint64_t kilobytes = 0;
        while (meminfo >> key) {
            if (key == "MemTotal:" && meminfo >> kilobytes) {
                info.systemMemoryMb = kilobytes / 1024;
                break;
            }
        }
    }
    {
        // A mouse or a controller lists its battery here as well ("scope" Device): not the machine's.
        bool mainsSeen = false;
        bool mainsOnline = false;
        bool discharging = false;
        for (const auto& entry : std::filesystem::directory_iterator(root / "sys/class/power_supply", ec)) {
            const std::string type = stripped(readFile(entry.path() / "type"));
            if (stripped(readFile(entry.path() / "scope")) == "Device") continue;
            if (type == "Battery") {
                info.hasBattery = true;
                if (stripped(readFile(entry.path() / "status")) == "Discharging") discharging = true;
            } else if (type == "Mains") {
                mainsSeen = true;
                if (stripped(readFile(entry.path() / "online")) == "1") mainsOnline = true;
            }
        }
        info.onBattery = info.hasBattery && !mainsOnline && (discharging || mainsSeen);
    }
    return info;
}

} // namespace veloplay
