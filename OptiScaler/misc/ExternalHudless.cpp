#include "pch.h"
#include "ExternalHudless.h"

#include <Config.h>
#include <State.h>
#include <shaders/hdr/Hdr10.h>
#include <cstring>
#include <mutex>

namespace
{
std::mutex g_mutex;
ID3D11Texture2D* g_pending = nullptr;
ExternalHudless::StatusV1 g_status = { sizeof(ExternalHudless::StatusV1), ExternalHudless::ApiVersion };
uint64_t g_lastLoggedRejectCount = 0;

void SetMessageLocked(const char* message)
{
    if (message == nullptr)
        message = "";

    strncpy_s(g_status.lastMessage, sizeof(g_status.lastMessage), message, _TRUNCATE);
}
} // namespace

bool ExternalHudless::Active()
{
    if (!Config::Instance()->FGExternalHudless.value_or_default()) return false;
    return Eligible(true, GetModuleHandleW(L"OptiScalerHudless.addon64") != nullptr,
        Hdr10::Active() && !Config::Instance()->FGExternalUIFreeFrameGen.value_or_default(), State::Instance().isShuttingDown);
}

void ExternalHudless::Submit(ID3D11Texture2D* texture)
{
    if (texture == nullptr || !Active())
        return;

    texture->AddRef();

    ID3D11Texture2D* previous = nullptr;
    bool first = false;

    {
        std::lock_guard lock(g_mutex);
        previous = g_pending;
        g_pending = texture;
        first = g_status.submitted == 0;
        g_status.submitted++;
    }

    if (previous != nullptr)
        previous->Release();

    if (first)
        LOG_INFO("External HUD-less: first texture received from ReShade add-on ({:X})", (size_t) texture);
}

ID3D11Texture2D* ExternalHudless::Take()
{
    std::lock_guard lock(g_mutex);

    auto texture = g_pending;
    g_pending = nullptr;

    if (texture != nullptr)
        g_status.consumed++;

    return texture;
}

void ExternalHudless::NoteFormats(uint32_t width, uint32_t height, uint32_t format, uint32_t backBufferFormat)
{
    std::lock_guard lock(g_mutex);

    const bool changed = g_status.lastWidth != width || g_status.lastHeight != height ||
                         g_status.lastFormat != format || g_status.backBufferFormat != backBufferFormat;

    g_status.lastWidth = width;
    g_status.lastHeight = height;
    g_status.lastFormat = format;
    g_status.backBufferFormat = backBufferFormat;

    if (changed)
        LOG_INFO("External HUD-less: {}x{} format {}, backbuffer format {}", width, height, format, backBufferFormat);
}

void ExternalHudless::MarkTagged()
{
    std::lock_guard lock(g_mutex);

    if (g_status.tagged == 0)
        LOG_INFO("External HUD-less: first frame tagged as HUD-less for frame generation");

    g_status.tagged++;
    SetMessageLocked("Tagging HUD-less every frame");
}

void ExternalHudless::MarkRejected(const char* reason)
{
    std::lock_guard lock(g_mutex);

    g_status.rejected++;
    SetMessageLocked(reason);

    // Log the first few and then every 600th so a persistent problem stays visible without flooding.
    if (g_status.rejected <= 5 || g_status.rejected - g_lastLoggedRejectCount >= 600)
    {
        g_lastLoggedRejectCount = g_status.rejected;
        LOG_WARN("External HUD-less rejected ({} total): {}", g_status.rejected, reason != nullptr ? reason : "");
    }
}

void ExternalHudless::Clear()
{
    ID3D11Texture2D* previous = nullptr;

    {
        std::lock_guard lock(g_mutex);
        previous = g_pending;
        g_pending = nullptr;
    }

    if (previous != nullptr)
        previous->Release();
}

namespace
{
ExternalHudless::UiStatus g_uiStatus;
uint64_t g_lastLoggedUiRejectCount = 0;
} // namespace

void ExternalHudless::MarkUiTagged()
{
    std::lock_guard lock(g_mutex);

    if (g_uiStatus.tagged == 0)
        LOG_INFO("External HUD-less: first UI colour+alpha image tagged for frame generation");

    g_uiStatus.tagged++;
    strncpy_s(g_uiStatus.lastMessage, sizeof(g_uiStatus.lastMessage), "UI image tagged every frame", _TRUNCATE);
}

void ExternalHudless::MarkUiRejected(const char* reason)
{
    std::lock_guard lock(g_mutex);

    g_uiStatus.rejected++;
    strncpy_s(g_uiStatus.lastMessage, sizeof(g_uiStatus.lastMessage), reason != nullptr ? reason : "", _TRUNCATE);

    if (g_uiStatus.rejected <= 5 || g_uiStatus.rejected - g_lastLoggedUiRejectCount >= 600)
    {
        g_lastLoggedUiRejectCount = g_uiStatus.rejected;
        LOG_WARN("External UI image skipped ({} total): {}", g_uiStatus.rejected, reason != nullptr ? reason : "");
    }
}

ExternalHudless::UiStatus ExternalHudless::UiSnapshot()
{
    std::lock_guard lock(g_mutex);
    return g_uiStatus;
}

ExternalHudless::StatusV1 ExternalHudless::Snapshot()
{
    std::lock_guard lock(g_mutex);
    return g_status;
}

extern "C"
{
    __declspec(dllexport) uint32_t __cdecl OptiScaler_ExternalHudlessVersion() { return ExternalHudless::ApiVersion; }

    // Called by the ReShade add-on on the game's render thread, once per frame, right after it
    // copied the pre-UI image into `texture`. Returns 1 when accepted.
    __declspec(dllexport) int __cdecl OptiScaler_SubmitHudlessDx11V1(ID3D11Texture2D* texture)
    {
        if (texture == nullptr || !ExternalHudless::Active())
            return 0;

        ExternalHudless::Submit(texture);
        return 1;
    }

    __declspec(dllexport) int __cdecl OptiScaler_GetHudlessStatusV1(ExternalHudless::StatusV1* status)
    {
        if (status == nullptr || status->size < sizeof(ExternalHudless::StatusV1))
            return 0;

        *status = ExternalHudless::Snapshot();
        return 1;
    }
}
