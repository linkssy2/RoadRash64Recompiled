#include "../src/rr64_popup_input.hpp"
#include <cstdio>
int main() {
    using namespace rr64::popup_input;
    if(suppress(false,true,0x8000,0,0))return 1;
    closed();
    if(!consume_menu_action() || !suppress(true,true,0,0,0) || !wait_for_release.load())return 2;
    for(int i=0;i<120;++i)if(!suppress(false,true,0x8000,0,0) || !consume_menu_action())return 3;
    if(!suppress(false,true,0,0.7f,0) || !wait_for_release.load())return 4;
    if(!suppress(false,false,0,0,0) || !wait_for_release.load())return 5;
    if(!suppress(false,true,0,0,0) || wait_for_release.load())return 6;
    if(suppress(false,true,0x8000,0,0) || consume_menu_action())return 7;
    closed();suppress(false,true,0,0,0);
    if(!consume_menu_action() || consume_menu_action())return 8;
    // Nested overlays: closing a child must not accept artificial neutral
    // from the still-open parent, and closing the parent must re-arm the guard.
    closed();
    for(int i=0;i<120;++i) {
        if(!suppress(true,true,0,0,0) || !consume_menu_action())return 9;
    }
    closed();
    if(!suppress(false,true,0x8000,0,0) || !wait_for_release.load())return 10;
    if(!suppress(false,true,0,0,0) || wait_for_release.load())return 11;
    consume_menu_action();
    if(suppress(false,true,0x8000,0,0) || consume_menu_action())return 12;
    std::puts("Popup close: held confirm, UI artificial neutral, analog hold, unavailable input, release, fresh press and cached menu action passed.");
}
