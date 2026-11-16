// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wii U stand-ins for PC-only runtime features (PORTING_NOTES.md, D10). The real
// implementations are excluded from the Wii U build because they depend on SDL3,
// Win32/POSIX IPC or ImGui:
//   - Discord Rich Presence, Steam Input layout, the F10 ImGui settings bar and the
//     friend-code keyboard: no Wii U equivalent; no-ops.
//   - Wiimote host (SDL HIDAPI + Bluetooth): reports no physical remotes until the
//     Phase 4 WPAD/KPAD-backed RemoteHost replaces it.
//   - Network HLE (BSD sockets + mbedtls 3.x): "not a network device" everywhere,
//     so the game sees no network. Online play comes after Phase 4.
#include "discord_presence.h"
#include "friend_code_keyboard.h"
#include "settings_overlay.h"
#include "steam_input_layout.h"
#include "wiimote_host.h"
#include "bluetooth/bt_controller.h"
#include "hle/net/network.h"

#include <cstdio>

// ---- Discord ---------------------------------------------------------------------
namespace DiscordPresence {
void Initialize(const std::string&, const std::string&) {}
void SetClient(const std::string&) {}
void SetActivity(Activity) {}
void Reset() {}
void Shutdown() {}
} // namespace DiscordPresence

// ---- Steam Input -----------------------------------------------------------------
namespace SteamInputLayout {
void Initialize(bool, uint32_t) {}
void HandleEvents(const AuroraEvent*) {}
void Release() noexcept {}
} // namespace SteamInputLayout

// ---- ImGui settings bar ----------------------------------------------------------
namespace settings_overlay {
void InitializeRuntimeSettings() noexcept {}
void HandleEvents(const AuroraEvent*) noexcept {}
void Draw() noexcept {}
bool StartupScreenVisible() noexcept { return false; }
void NotifyStrapInputAccepted() noexcept {}
void AdvancePresentedFrame() noexcept {}
void ReleaseControllers() noexcept {}
} // namespace settings_overlay

// ---- friend-code keyboard --------------------------------------------------------
namespace FriendCodeKeyboard {
void HandleEvents(const AuroraEvent*) noexcept {}
} // namespace FriendCodeKeyboard

// ---- Wiimote host ----------------------------------------------------------------
namespace Wiimote {
namespace {
class NoRemotesHost final : public Bluetooth::RemoteHost {
public:
    bool IsAttached(size_t) const override { return false; }
    bool ConsumeWake(size_t) override { return false; }
    bool PopInputReport(size_t, std::vector<uint8_t>&) override { return false; }
    void SendOutputReport(size_t, const uint8_t*, size_t) override {}
    void OnLinkStateChanged(size_t, Bluetooth::LinkState) override {}
    void OnGuestDisconnect(size_t) override {}
    void OnGuestInquiry(bool) override {}
    const char* DeviceName(size_t) const override { return "Nintendo RVL-CNT-01"; }
};
} // namespace

void ConfigureSdlHints(bool) {}
void Initialize() {}
void Shutdown() {}
Bluetooth::RemoteHost& Host()
{
    static NoRemotesHost host;
    return host;
}
} // namespace Wiimote

// ---- network HLE ------------------------------------------------------------------
extern "C" int32_t Network_HLE_OpenDevice(const char* path, uint32_t)
{
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::printf("[wiiu] network HLE not available yet (first device: %s)\n", path ? path : "?");
    }
    return 0;  // not a network device: the IOS layer falls through to its own handling
}
extern "C" bool Network_HLE_IsFd(uint32_t) { return false; }
extern "C" int32_t Network_HLE_Close(uint32_t) { return -1; }
extern "C" int32_t Network_HLE_Ioctl(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return -1; }
extern "C" int32_t Network_HLE_Ioctlv(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return -1; }
NetworkDeferredContract::StartOutcome Network_HLE_StartIoctlSync(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                                                 uint32_t, uint32_t)
{
    return NetworkDeferredContract::StartOutcome::NotApplicable();
}
NetworkDeferredContract::StartOutcome Network_HLE_StartIoctlvSync(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                                                  uint32_t)
{
    return NetworkDeferredContract::StartOutcome::NotApplicable();
}
NetworkDeferredContract::StartOutcome Network_HLE_StartIoctlAsync(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                                                  uint32_t, uint32_t, uint32_t)
{
    return NetworkDeferredContract::StartOutcome::NotApplicable();
}
NetworkDeferredContract::StartOutcome Network_HLE_StartIoctlvAsync(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                                                   uint32_t, uint32_t)
{
    return NetworkDeferredContract::StartOutcome::NotApplicable();
}
bool Network_HLE_TakeSyncResult(uint64_t, int32_t*) { return false; }
bool Network_HLE_ProcessCompletions(CpuContext*) { return false; }
