#pragma once

namespace rr64::achievements {

void initialize();
void set_enabled(bool enabled);
bool enabled();
void register_config_tab();
void initialize_toast_ui();
void update_ui();
// Single background owner; call once more after joining it at orderly exit.
void flush_progress();

} // namespace rr64::achievements
