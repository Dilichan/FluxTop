#pragma once
// Pure installation decision logic, shared by the real installer and offline tests.
// No Win32 API calls, no UAC prompts, no driver/service modifications.
namespace fluxtop {
enum class PawnioAction { reuse_existing, skip_without_consent, skip_without_elevation,
                          skip_invalid_bundle, install_embedded };
constexpr PawnioAction pawnio_install_policy(bool device_available,
    bool user_consented, bool elevated, bool verified_bundle) noexcept {
    if(device_available) return PawnioAction::reuse_existing;
    if(!user_consented) return PawnioAction::skip_without_consent;
    if(!elevated) return PawnioAction::skip_without_elevation;
    if(!verified_bundle) return PawnioAction::skip_invalid_bundle;
    return PawnioAction::install_embedded;
}
}