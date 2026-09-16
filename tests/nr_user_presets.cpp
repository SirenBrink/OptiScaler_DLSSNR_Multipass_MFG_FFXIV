#include "pch.h"
#include "dlssnr/NrUserPresets.h"
#include <cassert>
#include <iostream>

// Exercise real Config members/defaults without loading the game's INI or initializing graphics.
Config::Config() {}

int main(int argc, char** argv)
{
    using namespace DlssNr::UserPresets;
    assert(argc == 2);
    Config c;
    const auto defaults = Capture(c);
    c.DlssNrEnabled = true;
    c.DlssNrUnlockPasses = true;
    c.DlssNrPasses = 30u;
    c.DlssNrWorkingScale = 2.0f;
    c.DlssNrStyle = 2u;
    c.DlssNrPass2Intensity = 0.7f;
    c.DlssNrPass3AutoMask = false;
    c.DlssNrExtraPasses[26].style = 1u;
    c.DlssNrExtraPasses[26].intensity = 1.75f;
    const auto elaborate = Capture(c);
    Apply(defaults, c);
    assert(Capture(c) == defaults && !c.DlssNrPass2Intensity.has_value());
    Apply(elaborate, c);
    assert(Capture(c) == elaborate && c.DlssNrExtraPasses[26].intensity.value() == 1.75f);
    assert(!c.DlssNrExtraPasses[25].intensity.has_value());
    c.DlssNrToggleKey = 123;
    c.DlssNrHoldFrame = true;
    for (const auto& badValue : {Json(-1), Json(1.5), Json("two"), Json(1000)})
    {
        auto broken = defaults;
        broken["Pass30Style"] = badValue;
        bool failed = false;
        try { Apply(broken, c); } catch (...) { failed = true; }
        assert(failed && Capture(c) == elaborate);
    }
    auto missing = elaborate; missing.erase("Pass2Intensity");
    bool rejected = false;
    try { Apply(missing, c); } catch (...) { rejected = true; }
    assert(rejected && Capture(c) == elaborate);
    Apply(defaults, c);
    assert(c.DlssNrToggleKey.value() == 123 && c.DlssNrHoldFrame.value());
    assert(Name("  Gameplay  ") == "Gameplay");
    assert(Name("Portrait / 日本語") == "Portrait / 日本語");
    rejected = false;
    try { Name("   "); } catch (...) { rejected = true; }
    assert(rejected);
    const std::filesystem::path path = std::filesystem::path(argv[1]) / L"preset-test.json";
    Json library{{"version", 1}, {"presets", {{"Gameplay", defaults}, {"Portrait / 日本語", elaborate}}}};
    Write(path, library);
    assert(Read(path) == library);
    library["presets"].erase("Gameplay");
    Write(path, library);
    assert(Read(path) == library);
    // An unsuccessful save must leave the existing file untouched.
    auto oversized = library; oversized["padding"] = std::string(1024 * 1024, 'x');
    rejected = false;
    try { Write(path, oversized); } catch (...) { rejected = true; }
    assert(rejected && Read(path) == library);
    std::filesystem::remove(path);
    std::cout << "PASS: all-pass round trip, inheritance, atomic validation, NR-only scope, Unicode names, persistence and failed-save retention\n";
}
