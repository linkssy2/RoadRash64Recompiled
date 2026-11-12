#include "rr64_local_players.hpp"
#include "recompinput/rr64_player_assignment.h"
#include <array>
#include <vector>
#include <cstdio>

struct Slot { int controller = 0; bool keyboard_enabled = false; };
int main() {
    using recompinput::reconcile_local_players;
    std::array<Slot, 8> slots{};
    reconcile_local_players(slots, std::vector<int>{}, false);
    for (auto s : slots) if (s.controller || s.keyboard_enabled) return 1;
    reconcile_local_players(slots, std::vector<int>{11,22,33,44,55}, false);
    if (slots[0].controller!=11 || slots[3].controller!=44 || slots[4].controller) return 2;
    reconcile_local_players(slots, std::vector<int>{11,33,44}, false);
    if (slots[1].controller || slots[2].controller!=33 || slots[3].controller!=44) return 3;
    reconcile_local_players(slots, std::vector<int>{11,33,44}, true);
    if (!slots[1].keyboard_enabled || slots[2].controller!=33) return 4;
    reconcile_local_players(slots, std::vector<int>{11,33,44}, false);
    if (slots[1].keyboard_enabled) return 5;
    reconcile_local_players(slots, std::vector<int>{11,33,44,66}, false);
    if (slots[1].controller!=66 || slots[2].controller!=33) return 6;
    std::swap(slots[0],slots[3]);
    reconcile_local_players(slots, std::vector<int>{11,33,44,66}, false);
    if (slots[0].controller!=44 || slots[3].controller!=11) return 7;
    using rr64::local_players::display_name;
    if (display_name("   dave!  ",0)!="DAVE" || display_name("",3)!="PLAYER 4" ||
        display_name("abcdefghijklmnop",0)!="ABCDEFGHIJK" || display_name("<>",1)!="PLAYER 2") return 8;
    rr64::local_players::set_name(2,"Gina");
    if (rr64::local_players::snapshot()[2]!="GINA") return 9;
    std::puts("Empty slots, four-port cap, stable hotplug/reassignment, optional keyboard and bounded names passed.");
}
