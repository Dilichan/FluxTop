#include "fluxtop/privileges.hpp"
#include "fluxtop/windows.hpp"

namespace fluxtop {
Privileges initialize_privileges() {
    Privileges result;
    HANDLE raw{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return result;
    Handle query(raw);
    TOKEN_ELEVATION elevation{};
    DWORD size{};
    if (!GetTokenInformation(query.get(), TokenElevation, &elevation, sizeof(elevation), &size)) return result;
    result.elevated = elevation.TokenIsElevated != 0;
    if (!*result.elevated) return result;
    raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &raw)) return result;
    Handle token(raw);
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid)) return result;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(token.get(), FALSE, &privileges, 0, nullptr, nullptr)) return result;
    result.debug_enabled = GetLastError() == ERROR_SUCCESS;
    return result;
}
} // namespace fluxtop
