#include "rr64_rival_engine_config.hpp"
#include "librecomp/config.hpp"
#include "librecomp/game.hpp"
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace {
std::filesystem::path directory;
double observed_volume=-1;
bool observed_enabled=false;
std::array<double,4> observed_groups{};
unsigned checks=0;
void check(bool condition,const char *message) {
    ++checks;
    if(!condition){std::cerr<<"rival engine config failed: "<<message<<'\n';std::exit(1);}
}
recomp::config::Config make_config() {
    recomp::config::Config config("Audio","sound",false);
    config.add_percent_number_option("main_volume","Main Volume","",100);
    rr64::rival_engine::configure_volume(config);
    return config;
}
void write_json(const nlohmann::json &value) {
    std::ofstream output(directory/"sound.json");output<<value.dump();
}
nlohmann::json read_json() {
    nlohmann::json value;std::ifstream input(directory/"sound.json");input>>value;return value;
}
}
namespace recomp {
std::filesystem::path get_config_path(){return directory;}
const Version &get_project_version(){static const Version version(1,4,1);return version;}
}
namespace rr64::rival_engine {
// The real Config, JSON parser, callbacks and backup-file implementation run.
// The engine sink records UI delivery without starting any audio or game thread.
void set_volume_percent(double value) noexcept {observed_volume=value;}
double get_volume_percent() noexcept {return observed_volume;}
void set_enabled(bool value) noexcept {observed_enabled=value;}
bool get_enabled() noexcept {return observed_enabled;}
void set_group_volume(BikeGroup group,double value) noexcept {observed_groups.at(static_cast<unsigned>(group))=value;}
double get_group_volume(BikeGroup group) noexcept {return observed_groups.at(static_cast<unsigned>(group));}
}
int main(int argc,char **argv) {
    using namespace rr64::rival_engine;
    const bool restarted=argc==3 && std::string_view(argv[1])=="--verify-off";
    if(argc!=2 && !restarted)return 2;
    directory=argv[restarted?2:1];
    if(restarted){
        check(std::filesystem::is_directory(directory),"restart uses existing disposable settings");
        auto config=make_config();check(config.load_config(),"fresh process loads saved settings");
        apply_volume_config(config);
        check(!observed_enabled && observed_volume==35,"disabled rival engines and independent volume survive fresh process");
        std::cout<<"rival engine config restart passed "<<checks<<" checks\n";return 0;
    }
    if(std::filesystem::exists(directory) || !std::filesystem::create_directories(directory))return 3;
    auto config=make_config();
    const auto &schema=std::get<recomp::config::ConfigOptionNumber>(config.get_option(volume_option).variant);
    check(schema.min==0 && schema.max==100 && schema.step==1 && schema.precision==0 && schema.percent,"native percentage slider range");
    check(schema.default_value==35,"conservative default");
    check(std::get<bool>(config.get_option_value(enabled_option)),"rival engines default enabled");
    check(config.get_option(volume_option).description.find("including online players")!=std::string::npos,"opponent scope described");
    check(config.load_config(),"first-run config saved");
    apply_volume_config(config);
    check(observed_volume==35 && observed_enabled && read_json()[volume_option]==35 && read_json()[enabled_option]==true,"first-run defaults persisted and applied");

    write_json({{"main_volume",62}});
    auto old=make_config();check(old.load_config(),"old config loaded");
    apply_volume_config(old);
    check(observed_enabled,"existing settings without switch preserve enabled behavior");
    check(observed_volume==35,"missing option uses default at load");
    check(std::get<double>(old.get_option_value("main_volume"))==62,"existing main volume preserved");
    check(old.save_config() && read_json()[volume_option]==35,"old config gains saved default");
    for (const auto &option : bike_volume_options) {
        const auto index=static_cast<unsigned>(option.group);
        check(observed_groups[index]==default_group_volume(option.group),"old configs gain per-type defaults");
        old.set_option_value(option.id,42.0);
        check(observed_groups[index]==42.0 && old.save_config(),"group slider delivered and saved");
        auto loaded=make_config();check(loaded.load_config(),"group reload");
        apply_volume_config(loaded);
        check(observed_groups[index]==42.0,"group volume survives reload");
        write_json({{option.id,160}});
        auto bounded=make_config();check(bounded.load_config(),"group edited JSON load");
        apply_volume_config(bounded);check(observed_groups[index]==100,"group JSON clamped");
        write_json({{option.id,"invalid"}});
        auto invalid=make_config();check(invalid.load_config(),"group invalid JSON load");
        apply_volume_config(invalid);check(observed_groups[index]==default_group_volume(option.group),"group invalid value defaults");
    }
    for(const auto &[input,expected]:std::array<std::pair<double,double>,5>{{{-40,0},{140,100},{0,0},{100,100},{47,47}}}) {
        write_json({{"main_volume",62},{volume_option,input}});
        auto loaded=make_config();check(loaded.load_config(),"numeric config loaded");
        check(observed_volume==expected && std::get<double>(loaded.get_option_value(volume_option))==expected,"loaded value clamped before UI and runtime");
        check(loaded.save_config() && read_json()[volume_option]==expected,"normalized value persisted");
    }
    for(const auto &invalid:std::array<nlohmann::json,4>{nullptr,"loud",true,nlohmann::json::array({1})}) {
        write_json({{volume_option,invalid}});
        auto loaded=make_config();check(loaded.load_config() && observed_volume==35,"malformed option falls back safely");
    }
    for(const auto &invalid:std::array<nlohmann::json,4>{nullptr,"disabled",0,nlohmann::json::array({false})}) {
        write_json({{enabled_option,invalid}});
        auto loaded=make_config();check(loaded.load_config(),"malformed switch config loads safely");
        apply_volume_config(loaded);check(observed_enabled,"malformed switch retains enabled default");
    }
    auto changed=make_config();
    check(changed.load_config(),"UI config loaded");
    for(bool value:{false,true,false}) {
        const double volume=observed_volume;
        changed.set_option_value(enabled_option,value);
        check(observed_enabled==value && observed_volume==volume,"switch reaches engine without changing saved volume");
        check(changed.save_config() && read_json()[enabled_option]==value,"switch state persisted");
        auto reloaded=make_config();check(reloaded.load_config(),"saved switch config loads");
        apply_volume_config(reloaded);check(observed_enabled==value && observed_volume==volume,"switch and volume independently reload");
    }
    for(double value:{0.0,73.0,100.0}) {
        changed.set_option_value(volume_option,value);
        check(observed_volume==value,"slider update reaches engine immediately including off");
        check(changed.save_config() && read_json()[volume_option]==value,"slider update saved");
        auto restarted=make_config();check(restarted.load_config() && observed_volume==value,"slider setting survives new Config instance");
    }
    changed.set_option_value(volume_option,std::numeric_limits<double>::quiet_NaN());
    check(observed_volume==35 && changed.save_config() && read_json()[volume_option]==35,"nonfinite programmatic value cannot reach audio or JSON");
    changed.set_option_value(enabled_option,false);
    check(changed.save_config() && read_json()[enabled_option]==false,"persist off fixture for separate-process restart");
    check(std::filesystem::exists(directory/"sound.json.bak"),"normal runtime backup retained");
    std::cout<<"rival engine config smoke passed "<<checks<<" checks\n";
}
