#pragma once
#include <windows.h>

namespace DlssNr::FocusPolicy {
// Do not alter saved NR settings. Foreground windows owned by this process
// include the game and its in-process overlays; restoring focus resumes the
// existing requested state rather than blindly switching NR back on.
inline bool Paused(bool pauseOnFocusLoss) {
    if (!pauseOnFocusLoss) return false;
    DWORD owner = 0;
    if (const auto foreground = GetForegroundWindow()) GetWindowThreadProcessId(foreground, &owner);
    return owner != GetCurrentProcessId();
}
}
