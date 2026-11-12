#pragma once
#include "rr64_engine_layout.hpp"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace rr64::autotest {
// Local developer input only. File I/O stays off the emulated CPU; commands
// expire even if the controlling process disappears. No game-state writes.
struct Control {
    std::uint64_t serial=0;
    std::uint16_t buttons=0;
    std::int8_t x=0,y=0;
    std::uint8_t actions=0; // bit0: manual eject; bit1: restart private sampling, once per serial
    std::uint8_t port=0; // Optional seventh field: zero-based N64 controller port.
};
inline bool read_control(std::istream& file, Control& result, unsigned& duration) {
    std::uint64_t serial=0;
    std::int64_t milliseconds=0,buttons=0,actions=0,port=0;
    int x=0,y=0;
    file >> std::ws;
    if(file.peek()=='-' || !(file >> serial >> milliseconds >> std::hex >> buttons >> std::dec >> x >> y))return false;
    file >> std::ws;
    if(!file.eof()) {
        if(!(file >> actions))return false;
        file >> std::ws;
        if(!file.eof() && !(file >> port))return false;
        file >> std::ws;
        if(!file.eof())return false;
    }
    if(!serial || milliseconds<=0 || milliseconds>30000 || buttons<0 || buttons>65535 ||
       x < -80 || x>80 || y < -80 || y>80 || actions<0 || actions>3 || port<0 || port>3 || (port && actions))return false;
    result={serial,static_cast<std::uint16_t>(buttons),static_cast<std::int8_t>(x),
        static_cast<std::int8_t>(y),static_cast<std::uint8_t>(actions),static_cast<std::uint8_t>(port)};
    duration=static_cast<unsigned>(milliseconds);
    return true;
}

class ControllerInput {
    std::array<std::uint16_t,4> previous_{};
    unsigned port_=0;
    void write(unsigned char* memory, unsigned port, std::uint16_t buttons, std::int8_t x, std::int8_t y) {
        const auto changed=static_cast<std::uint16_t>(previous_[port]^buttons);
        engine::write_u16(memory,engine::globals::controller_buttons+port*2,buttons);
        engine::write_u16(memory,engine::globals::controller_changed_buttons+port*2,changed);
        engine::write_u16(memory,engine::globals::controller_pressed_buttons+port*2,changed&buttons);
        engine::write_s8(memory,engine::globals::controller_stick_x+port,x);
        engine::write_s8(memory,engine::globals::controller_stick_y+port,y);
        previous_[port]=buttons;
    }
public:
    void apply(unsigned char* memory, const Control& control) {
        if(!memory || control.port>=previous_.size())return;
        // One command owns one port. Changing ports releases the previous input.
        if(port_!=control.port)write(memory,port_,0,0,0);
        write(memory,control.port,control.buttons,control.x,control.y);
        port_=control.port;
    }
};

class ControlFile {
    std::mutex mutex_;
    Control current_{};
    std::chrono::steady_clock::time_point expires_{};
    std::string path_;
    std::jthread reader_;
public:
    ControlFile() {
        const char* enabled=std::getenv("RR64_AUTOTEST");
        const char* path=std::getenv("RR64_AUTOTEST_CONTROL");
        if(!enabled || !*enabled || *enabled=='0' || !path || !*path)return;
        path_=path;
        reader_=std::jthread([this](std::stop_token stop){
            std::uint64_t last=0;
            while(!stop.stop_requested()){
                std::ifstream file(path_);
                Control parsed;
                unsigned duration=0;
                if(read_control(file,parsed,duration) && parsed.serial>last){
                    std::lock_guard lock(mutex_);
                    current_=parsed;
                    expires_=std::chrono::steady_clock::now()+std::chrono::milliseconds(duration);
                    last=parsed.serial;
                    std::fprintf(stderr,"[RR64-AUTOTEST-CONTROL] serial=%llu ms=%u buttons=%04x stick=%d/%d actions=%u port=%u\n",
                        static_cast<unsigned long long>(parsed.serial),duration,unsigned(parsed.buttons),
                        int(parsed.x),int(parsed.y),unsigned(parsed.actions),unsigned(parsed.port));
                }
                file.close();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }
    bool enabled()const{return !path_.empty();}
    Control sample(){
        std::lock_guard lock(mutex_);
        if(std::chrono::steady_clock::now()<expires_)return current_;
        Control neutral;
        neutral.port=current_.port;
        return neutral;
    }
};
}
