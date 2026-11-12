#pragma once

#include <cstdint>

namespace rr64::online_menu {

void initialize_ui();
void update_ui();
bool controls_online_players();
bool host_controls_game_setup();

} // namespace rr64::online_menu
