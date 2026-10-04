// OptiScaler HUD-less capture add-on for ReShade (D3D11)
//
// Put the "OptiScaler_Hudless" technique (OptiScaler_Hudless.fx) into the
// ReshadeEffectShaderToggler group that runs right before the game's UI, ordered after
// your other effects. When that technique runs, this add-on copies the render target it
// ran on (the finished scene + your effects, without UI) and hands it to OptiScaler,
// which tags it as the HUD-less image for frame generation.
//
// The file name must sort before "ReshadeEffectShaderToggler" so this add-on's Present
// callback runs first (ReShade loads add-ons alphabetically).
//
// ABI NOTE: this file is built with clang for the MinGW target, while ReShade is built with
// MSVC. MSVC returns structs from C++ member functions through a hidden pointer; MinGW does
// not. So never call ReShade API *member functions that return a struct by value* here
// (get_resource_from_view, get_resource_desc, get_back_buffer, ...). Use the native D3D11
// objects (get_native()) for anything resource related instead. Scalar/pointer/void returns
// are fine. (v1 called get_resource_from_view and crashed inside d3d11.dll.)

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <windows.h>
#include <psapi.h>
#include <d3d11.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char* NAME = "OptiScaler HUD-less Capture";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Captures the frame at the OptiScaler_Hudless marker (place it in your Effect Toggler UI group) "
    "and gives it to OptiScaler as the HUD-less image for frame generation.";

namespace
{
constexpr const char* kMarkerTechnique = "OptiScaler_Hudless";
constexpr const char* kConfigSection = "OptiScalerHudless";

// Must match ExternalHudless::StatusV1 in OptiScaler.
struct OptiStatusV1
{
    uint32_t size;
    uint32_t apiVersion;
    uint64_t submitted;
    uint64_t consumed;
    uint64_t tagged;
    uint64_t rejected;
    uint32_t lastWidth;
    uint32_t lastHeight;
    uint32_t lastFormat;
    uint32_t backBufferFormat;
    char lastMessage[160];
};

using SubmitFn = int(__cdecl*)(ID3D11Texture2D*);
using StatusFn = int(__cdecl*)(OptiStatusV1*);
using VersionFn = uint32_t(__cdecl*)();

struct AddonState
{
    bool enabled = true;

    // Marker technique handle; invalidated by every effect reload.
    effect_technique marker = { 0 };
    bool markerLookupPending = true;

    // Between our Present callback and ReShade's own reshade_present. Captures in this window
    // come from the Toggler's "render remaining effects at Present" fallback, i.e. after the UI.
    bool inPresent = false;

    ID3D11Device* captureDevice = nullptr; // native device the capture texture lives on
    ID3D11Texture2D* capture = nullptr;
    D3D11_TEXTURE2D_DESC captureDesc = {};

    HMODULE optiModule = nullptr;
    SubmitFn submit = nullptr;
    StatusFn status = nullptr;
    uint32_t optiVersion = 0;
    uint32_t lookupCooldown = 0;

    uint64_t presents = 0;
    uint64_t markerRuns = 0;
    uint64_t captures = 0;
    uint64_t submitted = 0;
    uint64_t skippedAtPresent = 0;
    uint64_t lastCapturePresent = 0;

    uint32_t srcWidth = 0, srcHeight = 0, srcFormat = 0;
    uint32_t bbWidth = 0, bbHeight = 0, bbFormat = 0;
    uint64_t formatMismatches = 0;

    std::string issue;
};

AddonState g;

void SetIssue(const char* text)
{
    if (g.issue != text)
    {
        g.issue = text;
        if (text[0] != '\0')
            reshade::log::message(reshade::log::level::warning, text);
    }
}

void FindOptiScaler()
{
    if (g.submit != nullptr)
        return;

    if (g.lookupCooldown > 0)
    {
        g.lookupCooldown--;
        return;
    }

    g.lookupCooldown = 300; // retry roughly every few seconds until found

    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        return;

    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
    for (DWORD i = 0; i < count; ++i)
    {
        auto submit = reinterpret_cast<SubmitFn>(GetProcAddress(modules[i], "OptiScaler_SubmitHudlessDx11V1"));
        if (submit == nullptr)
            continue;

        g.optiModule = modules[i];
        g.submit = submit;
        g.status = reinterpret_cast<StatusFn>(GetProcAddress(modules[i], "OptiScaler_GetHudlessStatusV1"));

        if (auto version = reinterpret_cast<VersionFn>(GetProcAddress(modules[i], "OptiScaler_ExternalHudlessVersion")))
            g.optiVersion = version();

        char path[MAX_PATH] = {};
        GetModuleFileNameA(modules[i], path, MAX_PATH);
        char message[512];
        snprintf(message, sizeof(message), "Found OptiScaler HUD-less API v%u in %s", g.optiVersion, path);
        reshade::log::message(reshade::log::level::info, message);
        return;
    }
}

void LookupMarker(effect_runtime* runtime)
{
    g.marker = { 0 };
    g.markerLookupPending = false;

    runtime->enumerate_techniques(nullptr, [](effect_runtime* rt, effect_technique technique) {
        char name[128] = {};
        rt->get_technique_name(technique, name);
        if (strcmp(name, kMarkerTechnique) == 0)
            g.marker = technique;
    });
}

DXGI_FORMAT TypedFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

void DestroyCapture()
{
    if (g.capture != nullptr)
        g.capture->Release();

    g.capture = nullptr;
    g.captureDevice = nullptr;
    g.captureDesc = {};
}

bool EnsureCapture(ID3D11Device* nativeDevice, const D3D11_TEXTURE2D_DESC& source)
{
    const DXGI_FORMAT typed = TypedFormat(source.Format);

    if (g.capture != nullptr && g.captureDevice == nativeDevice && g.captureDesc.Width == source.Width &&
        g.captureDesc.Height == source.Height && g.captureDesc.Format == typed)
    {
        return true;
    }

    DestroyCapture();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = source.Width;
    desc.Height = source.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = typed;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* texture = nullptr;
    if (FAILED(nativeDevice->CreateTexture2D(&desc, nullptr, &texture)) || texture == nullptr)
    {
        SetIssue("Could not create the capture texture (unsupported render target format?)");
        return false;
    }

    g.captureDevice = nativeDevice; // identity only; the texture holds the device reference
    g.capture = texture;
    g.captureDesc = desc;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------

void OnInitEffectRuntime(effect_runtime* runtime)
{
    reshade::get_config_value(runtime, kConfigSection, "Enabled", g.enabled);
    g.markerLookupPending = true;
}

void OnReloadedEffects(effect_runtime*)
{
    g.marker = { 0 };
    g.markerLookupPending = true;
}

void OnDestroyDevice(device* dev)
{
    if (g.captureDevice != nullptr && reinterpret_cast<ID3D11Device*>(dev->get_native()) == g.captureDevice)
        DestroyCapture();
}

void OnDestroyEffectRuntime(effect_runtime*)
{
    DestroyCapture();
}

void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*)
{
    g.inPresent = true;
    g.presents++;
}

void OnReshadePresent(effect_runtime*)
{
    g.inPresent = false;
}

void OnRenderTechnique(effect_runtime* runtime, effect_technique technique, command_list* cmd_list, resource_view rtv,
                       resource_view)
{
    if (g.markerLookupPending)
        LookupMarker(runtime);

    if (g.marker.handle == 0 || technique != g.marker)
        return;

    g.markerRuns++;

    if (!g.enabled)
        return;

    if (g.inPresent)
    {
        // Leftover effects rendered at Present: the UI is already in this image (or there was no UI).
        g.skippedAtPresent++;
        return;
    }

    device* dev = runtime->get_device();
    if (dev->get_api() != device_api::d3d11)
    {
        SetIssue("Only Direct3D 11 is supported");
        return;
    }

    if (rtv.handle == 0)
        return;

    // On D3D11 a ReShade resource_view handle is the native ID3D11View pointer.
    ID3D11Resource* sourceResource = nullptr;
    reinterpret_cast<ID3D11View*>(static_cast<uintptr_t>(rtv.handle))->GetResource(&sourceResource);
    if (sourceResource == nullptr)
        return;

    ID3D11Texture2D* source = nullptr;
    sourceResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source));
    sourceResource->Release();

    if (source == nullptr)
    {
        SetIssue("Marker ran on a target that is not a 2D texture; move it to a different Toggler group");
        return;
    }

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    source->GetDesc(&sourceDesc);

    g.srcWidth = sourceDesc.Width;
    g.srcHeight = sourceDesc.Height;
    g.srcFormat = static_cast<uint32_t>(sourceDesc.Format);

    if (sourceDesc.SampleDesc.Count != 1 || sourceDesc.ArraySize != 1)
    {
        source->Release();
        SetIssue("Marker ran on a multisampled or array target; move it to a different Toggler group");
        return;
    }

    uint32_t screenWidth = 0, screenHeight = 0;
    runtime->get_screenshot_width_and_height(&screenWidth, &screenHeight);
    g.bbWidth = screenWidth;
    g.bbHeight = screenHeight;

    if (sourceDesc.Width != screenWidth || sourceDesc.Height != screenHeight)
    {
        source->Release();
        SetIssue("Marker ran on a target that is not screen-sized; move it to the group right before the UI");
        return;
    }

    // The HUD-less image must be the finished, display-ready frame minus the UI, so it has to be in
    // the same format as the screen. A different format (e.g. R16G16B16A16_FLOAT) means the marker ran
    // in a Toggler group that fires earlier in the frame, on the HDR scene buffer: DLSS-G would treat
    // every difference (water, fog, VFX, tonemapping) as UI, and a format change resets frame generation.
    DXGI_SWAP_CHAIN_DESC swapDesc = {};
    auto swapChain = reinterpret_cast<IDXGISwapChain*>(static_cast<uintptr_t>(runtime->get_native()));
    if (swapChain != nullptr && SUCCEEDED(swapChain->GetDesc(&swapDesc)))
    {
        g.bbFormat = static_cast<uint32_t>(swapDesc.BufferDesc.Format);

        if (TypedFormat(sourceDesc.Format) != TypedFormat(swapDesc.BufferDesc.Format))
        {
            source->Release();
            g.formatMismatches++;
            char message[256];
            snprintf(message, sizeof(message),
                     "Marker ran on a format %u target but the screen is format %u. It is in a Toggler group "
                     "that runs too early in the frame; keep it only in the group right before the UI.",
                     g.srcFormat, g.bbFormat);
            SetIssue(message);
            return;
        }
    }

    auto nativeDevice = reinterpret_cast<ID3D11Device*>(static_cast<uintptr_t>(dev->get_native()));
    auto nativeContext = reinterpret_cast<ID3D11DeviceContext*>(static_cast<uintptr_t>(cmd_list->get_native()));

    if (nativeDevice == nullptr || nativeContext == nullptr || !EnsureCapture(nativeDevice, sourceDesc))
    {
        source->Release();
        return;
    }

    // Same type group (typeless -> typed) is a legal CopyResource.
    nativeContext->CopyResource(g.capture, source);
    source->Release();

    g.captures++;
    g.lastCapturePresent = g.presents;

    FindOptiScaler();

    if (g.submit == nullptr)
    {
        SetIssue("OptiScaler with HUD-less support was not found in the game process");
        return;
    }

    // D3D11: the ReShade resource handle is the native ID3D11Resource pointer.
    if (g.submit(g.capture) != 0)
    {
        g.submitted++;
        SetIssue("");
    }
}

// ---------------------------------------------------------------------------------------------
// Overlay (Add-ons tab)
// ---------------------------------------------------------------------------------------------

void DrawSettings(effect_runtime* runtime)
{
    if (ImGui::Checkbox("Send HUD-less to OptiScaler", &g.enabled))
        reshade::set_config_value(runtime, kConfigSection, "Enabled", g.enabled);

    ImGui::Separator();

    if (g.marker.handle == 0)
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                           "Marker technique \"%s\" not loaded. Install OptiScaler_Hudless.fx and enable it.",
                           kMarkerTechnique);
    else if (g.presents > 0 && g.presents - g.lastCapturePresent > 2 && g.captures > 0)
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Marker is not being captured this frame");
    else if (g.captures == 0)
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                           "Marker loaded but never ran before the UI. Assign it to your Toggler UI group.");
    else
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Capturing before the UI");

    ImGui::Text("Marker runs: %llu   captured: %llu   sent: %llu   skipped at Present: %llu",
                (unsigned long long) g.markerRuns, (unsigned long long) g.captures, (unsigned long long) g.submitted,
                (unsigned long long) g.skippedAtPresent);
    ImGui::Text("Capture target: %ux%u (format %u)   screen: %ux%u (format %u)", g.srcWidth, g.srcHeight,
                g.srcFormat, g.bbWidth, g.bbHeight, g.bbFormat);
    if (g.formatMismatches > 0)
        ImGui::Text("Skipped (wrong format / wrong group): %llu", (unsigned long long) g.formatMismatches);

    ImGui::Separator();

    if (g.submit == nullptr)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "OptiScaler HUD-less API: not found");
        ImGui::TextWrapped("Needs an OptiScaler build with external HUD-less support loaded in this game.");
    }
    else
    {
        ImGui::Text("OptiScaler HUD-less API: v%u", g.optiVersion);

        OptiStatusV1 status = {};
        status.size = sizeof(status);
        if (g.status != nullptr && g.status(&status) != 0)
        {
            ImGui::Text("OptiScaler: received %llu   used %llu   tagged %llu   rejected %llu",
                        (unsigned long long) status.submitted, (unsigned long long) status.consumed,
                        (unsigned long long) status.tagged, (unsigned long long) status.rejected);
            ImGui::Text("Formats: HUD-less %u   backbuffer %u", status.lastFormat, status.backBufferFormat);
            if (status.lastMessage[0] != '\0')
                ImGui::TextWrapped("%s", status.lastMessage);
        }
    }

    if (!g.issue.empty())
    {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", g.issue.c_str());
    }
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module))
            return FALSE;

        reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(OnReloadedEffects);
        reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
        reshade::register_event<reshade::addon_event::present>(OnPresent);
        reshade::register_event<reshade::addon_event::reshade_present>(OnReshadePresent);
        reshade::register_event<reshade::addon_event::reshade_render_technique>(OnRenderTechnique);
        reshade::register_overlay(nullptr, DrawSettings);
        break;

    case DLL_PROCESS_DETACH:
        reshade::unregister_overlay(nullptr, DrawSettings);
        reshade::unregister_addon(module);
        break;
    }

    return TRUE;
}
