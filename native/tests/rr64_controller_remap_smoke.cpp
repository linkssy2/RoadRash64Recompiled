#include "recompinput/profiles.h"
#include "recompinput/input_binding.h"
#include "recompinput/input_state.h"
#include "recompui/config.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>

// Real mappings, scanner, SDL state and config codec. Only window/UI callbacks
// and atomic JSON replacement/backups are substituted; no game or audio device.
namespace recompui { void queue_event(const SDL_Event&) {} }
#include "rr64_controller_remap_fixture.inc"
static auto& slots = recompinput::PlayerState.players;
static bool capturing = false;
namespace recompinput::players {
bool is_single_player_mode() { return PlayerState.single_player_mode; }
const Player& get_player(int i, bool) { return slots.at(i); }
size_t get_number_of_assigned_players() { return slots.get_count(); }
}
namespace recompinput::playerassignment {
bool is_blocking_input() { return false; }
void stop_and_close_modal() { PlayerState.is_assigning = false; }
}
namespace recompui { bool is_context_capturing_input() { return capturing; } }
namespace recompui::config::general {
double get_joystick_deadzone() { return 0; }
bool has_rumble_strength_option() { return false; }
double get_rumble_strength() { return 0; }
double get_gyro_sensitivity() { return 0; }
double get_mouse_sensitivity() { return 0; }
}
namespace recompinput {
bool read_json_with_backups(const std::filesystem::path& path, nlohmann::json& out) {
    std::ifstream in(path); if (!in) return false; in >> out; return true;
}
bool save_json_with_backups(const std::filesystem::path& path, const nlohmann::json& in) {
    std::ofstream out(path); out << in.dump(2); return bool(out);
}
}
static unsigned checks;
static void check(bool result, const char* label) {
    ++checks;
    if (!result) { std::fprintf(stderr,"FAIL %u: %s (%s)\n",checks,label,SDL_GetError()); std::exit(1); }
}
struct Pad { SDL_Gamepad* controller; SDL_Joystick* joystick; SDL_JoystickID id; };
static Pad pad(unsigned product) {
    // RR64-SDL3: SDL_INIT_INTERFACE sets the struct version; attach returns an instance ID.
    SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
    d.type=SDL_JOYSTICK_TYPE_GAMEPAD; d.naxes=SDL_GAMEPAD_AXIS_COUNT; d.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    d.vendor_id=0x1209; d.product_id=static_cast<Uint16>(product);
    d.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1; d.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    d.name="RR64 controller remap fixture";
    const SDL_JoystickID index=SDL_AttachVirtualJoystick(&d); check(index!=0,"attach virtual SDL controller");
    auto* c=SDL_OpenGamepad(index); check(c!=nullptr,"open SDL controller");
    auto* j=SDL_GetGamepadJoystick(c); auto id=SDL_GetJoystickID(j);
    recompinput::add_controller_state(id,c); recompinput::poll_inputs(); return {c,j,id};
}
static void unplug(Pad& p) {
    // RR64-SDL3: detach takes the instance ID directly (SDL2 needed a device index).
    check(p.id!=0,"find virtual device to detach");
    recompinput::remove_controller_state(p.id);
    check(SDL_DetachVirtualJoystick(p.id),"detach virtual controller");
    SDL_CloseGamepad(p.controller); p={}; recompinput::poll_inputs();
}
static void axis(Pad& p, SDL_GamepadAxis a, bool down, bool scan=false) {
    // SDL's virtual raw trigger starts at -32768; mapped controller value is 0..32767.
    check(SDL_SetJoystickVirtualAxis(p.joystick,a,down?32767:-32768),"set virtual trigger");
    SDL_UpdateJoysticks(); recompinput::poll_inputs();
    if(scan) { SDL_Event e{}; while(SDL_PollEvent(&e)) fixture_input_event(&e); }
}
static unsigned buttons(int slot=0) {
    uint16_t b{}; float x{},y{}; recompinput::profiles::get_n64_input(slot,&b,&x,&y); return b;
}
static void assign(int slot,Pad& p,int profile) {
    using namespace recompinput;
    slots[slot]={}; slots[slot].controller=p.controller; slots.recount();
    profiles::set_input_profile_for_player(slot,profile,InputDevice::Controller);
    profiles::set_input_profile_for_player(slot,-1,InputDevice::Keyboard);
}
static bool choose(int slot,int profile,recompinput::InputDevice device=recompinput::InputDevice::Controller) {
#ifdef OLD_ASSOCIATION
    recompinput::profiles::set_input_profile_for_player(slot,profile,device); return true;
#else
    return recompinput::profiles::select_input_profile_for_player(slot,profile,device);
#endif
}
static void bind_axis(int slot,Pad& p,recompinput::GameInput input,SDL_GamepadAxis a) {
    using namespace recompinput;
    SDL_Event stale; while(SDL_PollEvent(&stale)) {}
    binding::start_scanning(slot,input,0,InputDevice::Controller);
    axis(p,a,true,true);
    check(!binding::is_binding(),"actual SDL axis event completed frontend scanner");
    check(profiles::get_input_binding(profiles::get_input_profile_for_player(slot,InputDevice::Controller),input,0).input_id==a+1,
          "scanner stored trigger mapping into selected profile");
    axis(p,a,false); binding::stop_skipping_events();
}
static void verify_swap(Pad& p,int slot=0) {
    axis(p,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,true);
    check((buttons(slot)&0x2010)==0x10,"LT now drives N64 R only");
    axis(p,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,false);
    axis(p,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,true);
    check((buttons(slot)&0x2010)==0x2000,"RT now drives N64 Z only");
    axis(p,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,false);
}
static void verify_saved_c_bindings() {
    using namespace recompinput;
    const int sp=profiles::get_sp_controller_profile_index();
    const auto right=profiles::get_input_binding(sp,GameInput::C_RIGHT,0);
    check(right==InputField::controller_analog(SDL_GAMEPAD_AXIS_LEFTY,false),
          "saved SP C-right left-stick-axis customization survives config loading");
    // These are the exact values the former startup migration overwrote.
    for (const auto& binding : {
            std::pair{GameInput::C_LEFT,SDL_GAMEPAD_BUTTON_NORTH},
            std::pair{GameInput::C_RIGHT,SDL_GAMEPAD_BUTTON_EAST},
            std::pair{GameInput::C_UP,SDL_GAMEPAD_BUTTON_RIGHT_STICK},
            std::pair{GameInput::C_DOWN,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}}) {
        check(profiles::get_input_binding(sp,binding.first,1)==InputField::controller_digital(binding.second),
              "exact former startup-migration secondary binding survives loading");
    }
}
static void press(Pad& pad,SDL_GamepadButton button,bool down) {
    check(SDL_SetJoystickVirtualButton(pad.joystick,button,down),"set virtual controller button");
    SDL_UpdateJoysticks(); recompinput::poll_inputs();
}
static void verify_default_c_actions(Pad& pad) {
    using namespace recompinput;
    press(pad,SDL_GAMEPAD_BUTTON_RIGHT_STICK,true);
    check(profiles::get_action_input(0,GameInput::RR64_SPOKE_JAM),"fresh R3 still drives the dedicated spoke-jam action");
    check((buttons()&0x000f)==0,"fresh R3 does not also cycle weapons or send a C-button");
    press(pad,SDL_GAMEPAD_BUTTON_RIGHT_STICK,false);
    press(pad,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true);
    check(profiles::get_action_input(0,GameInput::RR64_WEAPON_TRICK),"fresh RB still drives the dedicated trick action");
    check((buttons()&0x000f)==0,"fresh RB does not also send a C attack");
    press(pad,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,false);
    for(auto button:{SDL_GAMEPAD_BUTTON_NORTH,SDL_GAMEPAD_BUTTON_EAST}) {
        press(pad,button,true);
        check((buttons()&0x000f)==0,"fresh face buttons do not duplicate right-stick C actions");
        press(pad,button,false);
    }
    const struct { SDL_GamepadAxis axis; Sint16 value; unsigned button; } directions[] = {
        {SDL_GAMEPAD_AXIS_RIGHTX,-32768,0x2}, {SDL_GAMEPAD_AXIS_RIGHTX,32767,0x1},
        {SDL_GAMEPAD_AXIS_RIGHTY,-32768,0x8}, {SDL_GAMEPAD_AXIS_RIGHTY,32767,0x4}};
    for(const auto& direction:directions) {
        SDL_SetJoystickVirtualAxis(pad.joystick,direction.axis,direction.value); SDL_UpdateJoysticks();
        check((buttons()&0x000f)==direction.button,"fresh right-stick direction reaches exactly its native C-button");
        SDL_SetJoystickVirtualAxis(pad.joystick,direction.axis,0); SDL_UpdateJoysticks();
    }
}
static void verify_custom_c_actions(Pad& pad) {
    const SDL_GamepadButton button[]={SDL_GAMEPAD_BUTTON_NORTH,SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_RIGHT_STICK,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER};
    const unsigned native[]={0x2,0x1,0x8,0x4};
    for(unsigned i=0;i<4;++i) {
        press(pad,button[i],true);
        check((buttons()&0x000f)==native[i],"saved secondary customization reaches its native C action");
        press(pad,button[i],false);
    }
    SDL_SetJoystickVirtualAxis(pad.joystick,SDL_GAMEPAD_AXIS_LEFTY,-32768); SDL_UpdateJoysticks();
    check((buttons()&0x000f)==0x1,"saved left-stick-up customization reaches C-right");
    SDL_SetJoystickVirtualAxis(pad.joystick,SDL_GAMEPAD_AXIS_LEFTY,0); SDL_UpdateJoysticks();
}
int main(int argc,char** argv) {
    using namespace recompinput;
    check(argc==3,"mode and isolated config path supplied");
    const std::string mode=argv[1]; const std::filesystem::path file=argv[2];
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    check(SDL_Init(SDL_INIT_GAMEPAD),"SDL input only initialization");
    PlayerState.single_player_mode=true;
    const bool reader=mode=="read";
    profiles::load_controls_config(reader ? file : std::filesystem::path(file.string()+".missing"));
    Pad one=pad(1),two=pad(2);
    axis(one,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,false); axis(one,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,false);
    axis(two,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,false); axis(two,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,false);
    if(mode=="defaults") {
        verify_default_c_actions(one); // Fresh pre-assignment SP profile.
        const int reset=profiles::add_input_profile("reset_fixture","Reset fixture",InputDevice::Controller,true);
        profiles::reset_profile_bindings(reset,InputDevice::Controller);
        assign(0,one,reset);
        verify_default_c_actions(one); // Newly created assigned/custom profile.
        profiles::set_input_binding(reset,GameInput::C_UP,1,InputField::controller_digital(SDL_GAMEPAD_BUTTON_RIGHT_STICK));
        profiles::reset_input_binding(reset,InputDevice::Controller,GameInput::C_UP);
        verify_default_c_actions(one); // Reset one action clears old secondaries.
        profiles::set_input_binding(reset,GameInput::C_LEFT,1,InputField::controller_digital(SDL_GAMEPAD_BUTTON_NORTH));
        profiles::reset_profile_bindings(reset,InputDevice::Controller);
        verify_default_c_actions(one); // Full reset uses the same safe defaults.
        std::printf("PASS remap defaults: %u checks\n",checks); SDL_Quit(); return 0;
    }
    if(reader) {
        const int selected=profiles::get_input_profile_by_key("custom_swap");
        check(selected>=0,"saved custom layout loaded in fresh process");
        check(profiles::get_controller_profile_index_from_sdl_controller(one.controller)==selected,
              "fresh process restores controller association from serialized identity");
        check(players::select_player_device(0,one.controller),"assign saved controller after restart");
        verify_swap(one);
        verify_saved_c_bindings();
        assign(0,one,profiles::get_sp_controller_profile_index());
        verify_custom_c_actions(one);
        assign(0,one,selected);
        check(profiles::get_controller_count()==4,"invalid missing/wrong-device associations skipped");
        auto a=profiles::get_guid_from_sdl_controller(one.controller),b=a;
        a.serial="fixture-A"; b.serial="fixture-B";
        const int ca=profiles::get_controller_by_guid(a),cb=profiles::get_controller_by_guid(b);
        check(ca>=0 && cb>=0 && ca!=cb,"serial identity survives fresh-process JSON reload");
        check(profiles::get_controller_profile_index(ca)==selected &&
              profiles::get_controller_profile_index(cb)==profiles::get_input_profile_by_key("default_one"),
              "distinct serials retain separate saved layouts");
        profiles::save_controls_config(file.string()+".roundtrip");
        std::printf("PASS remap fresh reader: %u checks\n",checks); SDL_Quit(); return 0;
    }
    const int base=profiles::add_input_profile("default_one","Device one",InputDevice::Controller,false);
    const int other=profiles::add_input_profile("default_two","Device two",InputDevice::Controller,false);
    const int custom=profiles::add_input_profile("custom_swap","Swapped triggers",InputDevice::Controller,true);
    for(int p:{base,other,custom}) profiles::reset_profile_bindings(p,InputDevice::Controller);
    profiles::add_controller(profiles::get_guid_from_sdl_controller(one.controller),base);
    profiles::add_controller(profiles::get_guid_from_sdl_controller(two.controller),other);
    assign(0,one,base); assign(1,two,other);
    check(choose(0,custom),"select custom profile through explicit UI API");
    bind_axis(0,one,GameInput::Z,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    bind_axis(0,one,GameInput::R,SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    verify_swap(one);
    axis(two,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,true);
    check((buttons(1)&0x2010)==0x2000,"other device retains its original LT binding");
    check((buttons(0)&0x2010)==0,"other device does not drive player one");
    axis(two,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,false);
    std::printf("PASS immediate actual frontend scanner -> SDL polling -> N64 LT/RT swap\n");
    if(mode=="write") {
        // Write a deliberately known association through the existing registry
        // API, isolating JSON identity reconstruction from the selection bug.
        profiles::add_controller(profiles::get_guid_from_sdl_controller(one.controller),custom);
#ifndef OLD_ASSOCIATION
        auto a=profiles::get_guid_from_sdl_controller(one.controller),b=a;
        a.serial="fixture-A"; b.serial="fixture-B";
        profiles::add_controller(a,custom); profiles::add_controller(b,base);
#endif
        const int sp=profiles::get_sp_controller_profile_index();
        profiles::set_input_binding(sp,GameInput::C_RIGHT,0,InputField::controller_analog(SDL_GAMEPAD_AXIS_LEFTY,false));
        profiles::set_input_binding(sp,GameInput::C_LEFT,1,InputField::controller_digital(SDL_GAMEPAD_BUTTON_NORTH));
        profiles::set_input_binding(sp,GameInput::C_RIGHT,1,InputField::controller_digital(SDL_GAMEPAD_BUTTON_EAST));
        profiles::set_input_binding(sp,GameInput::C_UP,1,InputField::controller_digital(SDL_GAMEPAD_BUTTON_RIGHT_STICK));
        profiles::set_input_binding(sp,GameInput::C_DOWN,1,InputField::controller_digital(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
        check(profiles::save_controls_config(file),"save real profile JSON");
        nlohmann::json encoded; { std::ifstream in(file); in>>encoded; }
        auto invalid=encoded.at("controllers").front();
        invalid["guid"]["product"]=99; invalid["profile"]="missing_profile";
        encoded["controllers"].push_back(invalid);
        invalid["guid"]["product"]=98; invalid["profile"]="keyboard_sp";
        encoded["controllers"].push_back(invalid);
        { std::ofstream out(file); out<<encoded.dump(2); }
        std::printf("PASS remap writer: %u checks\n",checks); SDL_Quit(); return 0;
    }
    if(mode=="stale") {
        binding::start_scanning(0,GameInput::Z,0,InputDevice::Controller);
        profiles::set_input_profile_for_player(0,base,InputDevice::Controller);
        binding::set_scanned_input(InputField::controller_analog(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        check(profiles::get_input_binding(base,GameInput::Z,0).input_id==SDL_GAMEPAD_AXIS_LEFT_TRIGGER+1,
              "stale scanner cannot overwrite a newly assigned profile");
        profiles::set_input_profile_for_player(0,custom,InputDevice::Controller);
        binding::start_scanning(0,GameInput::Z,0,InputDevice::Controller);
        slots[0].controller=two.controller;
        binding::set_scanned_input(InputField::controller_analog(SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        check(profiles::get_input_binding(custom,GameInput::Z,0).input_id==SDL_GAMEPAD_AXIS_RIGHT_TRIGGER+1,
              "stale scanner cannot write after a physical controller swap with same profile");
        slots[0].controller=one.controller;
        check(!binding::is_binding(),"invalidated scan is cancelled");
        binding::set_scanned_input(InputField::controller_analog(SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        check(profiles::get_input_binding(custom,GameInput::Z,0).input_id==SDL_GAMEPAD_AXIS_RIGHT_TRIGGER+1,
              "late events after cancellation cannot change the prior binding");
        binding::start_scanning(0,GameInput::Z,0,InputDevice::Controller);
        unplug(one); // Deliberately keep its closed handle in the slot until reconciliation.
        binding::set_scanned_input(InputField::controller_analog(SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        check(!binding::is_binding(),"queued event after device closure safely cancels scanning");
        check(profiles::get_input_binding(custom,GameInput::Z,0).input_id==SDL_GAMEPAD_AXIS_RIGHT_TRIGGER+1,
              "removed controller cannot commit a queued binding");
        binding::start_scanning(0,GameInput::Z,0,InputDevice::Controller);
        check(!binding::is_binding(),"closed handle cannot start a new scan");
        check(!choose(0,base),"closed handle cannot accept an explicit layout selection");
        slots[0]={}; slots.recount();
        const int keyboard=profiles::get_sp_keyboard_profile_index();
        slots[0].keyboard_enabled=true; slots.recount();
        profiles::set_input_profile_for_player(0,keyboard,InputDevice::Keyboard);
        const auto prior=profiles::get_input_binding(keyboard,GameInput::Z,0);
        binding::start_scanning(0,GameInput::Z,0,InputDevice::Keyboard);
        slots[0].keyboard_enabled=false; slots.recount();
        binding::set_scanned_input({InputType::Keyboard,SDL_SCANCODE_F12});
        const auto after=profiles::get_input_binding(keyboard,GameInput::Z,0);
        check(prior.input_type==after.input_type && prior.input_id==after.input_id,
              "removing keyboard ownership rejects queued scan with unchanged profile");
        std::printf("PASS stale scan guard: %u checks\n",checks); SDL_Quit(); return 0;
    }
    check(players::select_player_device(0,nullptr),"temporarily select keyboard");
    check(players::select_player_device(0,one.controller),"reassign original controller");
    check(profiles::get_input_profile_for_player(0,InputDevice::Controller)==custom,
          "custom profile survives controller reassignment");
    verify_swap(one);
    unplug(one); slots[0]={}; slots.recount(); one=pad(1);
    axis(one,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,false); axis(one,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,false);
    check(players::select_player_device(0,one.controller),"reassign reconnected controller");
    check(profiles::get_input_profile_for_player(0,InputDevice::Controller)==custom,"custom profile survives reconnect");
    verify_swap(one);
    Pad twin=pad(2); axis(twin,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,false); axis(twin,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,false);
    assign(2,twin,other);
    check(choose(2,custom),"second same-GUID pad selects separate session layout");
    check(profiles::get_input_profile_for_player(1,InputDevice::Controller)==other,"same GUID selection does not replace other slot");
    PlayerState.temp_players=slots;
    playerassignment::commit_player_assignment();
    check(profiles::get_input_profile_for_player(1,InputDevice::Controller)==other &&
          profiles::get_input_profile_for_player(2,InputDevice::Controller)==custom,"assignment commit preserves separate same-GUID sessions");
    axis(two,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,true);
    check((buttons(1)&0x2010)==0x2000,"same-GUID first pad keeps default controls");
    axis(two,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,false); verify_swap(twin,2);
#ifndef OLD_ASSOCIATION
    check(!choose(-1,custom) && !choose(4,custom),"invalid player selection rejected");
    check(!choose(0,-1) && !choose(0,profiles::get_input_profile_count()),"missing layout rejected");
    check(!choose(0,profiles::get_sp_keyboard_profile_index()),"wrong device layout rejected");
    check(profiles::get_input_profile_for_player(0,InputDevice::Controller)==custom,"invalid choice leaves active mapping intact");
    auto serial_a=profiles::get_guid_from_sdl_controller(one.controller),serial_b=serial_a;
    serial_a.serial="fixture-A"; serial_b.serial="fixture-B";
    const int ca=profiles::add_controller(serial_a,custom),cb=profiles::add_controller(serial_b,base);
    check(ca!=cb,"controller serials distinguish identical model identities");
    check(profiles::get_controller_by_guid(serial_a)==ca && profiles::get_controller_by_guid(serial_b)==cb,
          "identity lookup uses stored fields rather than caller hash");
#endif
    check(profiles::save_controls_config(file),"save after explicit choices");
    std::printf("PASS remap lifecycle: %u checks\n",checks); SDL_Quit(); return 0;
}
