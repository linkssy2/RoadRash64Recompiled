#include "recompinput.h"
#include <cstdio>
int main() {
    using namespace recompinput;
    static_assert(int(GameInput::RR64_EJECT)==int(GameInput::TAB_RIGHT_MENU)+1);
    static_assert(int(GameInput::RR64_SPOKE_JAM)==int(GameInput::RR64_EJECT)+1);
    static_assert(int(GameInput::RR64_WEAPON_TRICK)==int(GameInput::RR64_SPOKE_JAM)+1);
    for(auto action:{GameInput::RR64_EJECT,GameInput::RR64_SPOKE_JAM,GameInput::RR64_WEAPON_TRICK}) {
        if(get_game_input_name(action).empty() || get_game_input_description(action).empty() ||
            get_game_input_is_menu(action) || !get_game_input_clearable(action))return 1;
        const auto& fields=get_default_mapping_for_input(InputDevice::Controller,action);
        const auto button=action==GameInput::RR64_WEAPON_TRICK?SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            action==GameInput::RR64_EJECT?SDL_GAMEPAD_BUTTON_LEFT_STICK:SDL_GAMEPAD_BUTTON_RIGHT_STICK;
        if(fields.size()!=1 || fields[0]!=InputField::controller_digital(button))return 2;
        if(!get_default_mapping_for_input(InputDevice::Keyboard,action).empty())return 3;
        nlohmann::json saved;
        saved[get_game_input_enum_name(action)]=std::vector<InputField>{InputField::keyboard(SDL_SCANCODE_G)};
        const auto loaded=nlohmann::json::parse(saved.dump());
        const auto bindings=loaded.at(get_game_input_enum_name(action)).get<std::vector<InputField>>();
        if(bindings.size()!=1 || bindings[0]!=InputField::keyboard(SDL_SCANCODE_G))return 4;
    }
    std::puts("Action binding metadata, stable IDs, default stick clicks, clearable rows and binding JSON roundtrip passed.");
}

