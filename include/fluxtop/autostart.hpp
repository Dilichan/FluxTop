#pragma once
namespace fluxtop {
bool autostart_enabled();
void set_autostart(bool enabled);
void migrate_autostart_if_needed() noexcept;
bool legacy_autostart_pending() noexcept;
}
