#include "rr64_rider_skin_fixture.hpp"
#include "rr64_rider_skin_mod_ui.hpp"
#include "rr64_netplay.hpp"
#include "miniz.h"
#include "miniz_zip.h"
#include "librecomp/mods.hpp"
#include <cstdio>
#include <cstdlib>

namespace mods=recomp::mods;
namespace skin=rr64::rider_skins;
namespace ui=rr64::rider_skin_mod_ui;
namespace {
unsigned checks=0,registrations=0;bool started=false,online=false;
mods::ModContentType content;mods::ModTogglePolicy policy;
void check(bool v,const char* why){++checks;if(!v){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
using Files=std::vector<std::pair<std::string,std::vector<std::uint8_t>>>;
bool load(std::string_view marker,const Files& files,const char* id=skin::mod_id){
    mz_zip_archive writer{};check(mz_zip_writer_init_heap(&writer,0,0),"ZIP starts");
    check(mz_zip_writer_add_mem(&writer,"rr64-rider-skins.json",marker.data(),marker.size(),MZ_BEST_SPEED),"descriptor writes");
    for(const auto& [name,bytes]:files)check(mz_zip_writer_add_mem(&writer,name.c_str(),bytes.data(),bytes.size(),MZ_BEST_SPEED),"texture writes");
    void* data=nullptr;size_t size=0;check(mz_zip_writer_finalize_heap_archive(&writer,&data,&size),"ZIP finishes");
    mods::ModOpenError error{};mods::ZipModFileHandle file(std::span(static_cast<const std::uint8_t*>(data),size),error);
    check(error==mods::ModOpenError::Good,"real runtime archive reader opens");
    const bool result=ui::load_archive(file,id);mz_free(data);mz_zip_writer_end(&writer);return result;
}
}
mods::ModContentTypeId mods::register_mod_content_type(const ModContentType& value){content=value;++registrations;return {0};}
void mods::register_mod_toggle_policy(const std::string& id,ModTogglePolicy value){check(id==skin::mod_id,"mod identity");policy=std::move(value);}
bool ultramodern::is_game_started(){return started;}
rr64::netplay::PhysicsRules rr64::netplay::get_physics_rules(){PhysicsRules p;p.active=online;return p;}
int main(int argc,char** argv){
    check(argc==1||argc==2,"optional final .nrm argument");ui::install();ui::install();
    check(registrations==1&&content.content_filename=="rr64-rider-skins.json"&&!content.allow_runtime_toggle,"one startup-only native-skin registration");
    check(policy.can_toggle(),"prestart toggle allowed");started=true;check(!policy.can_toggle(),"running game blocks toggle");started=false;
    online=true;check(!policy.can_rescan(),"online blocks rescan");online=false;
    nlohmann::json marker={{"format","rr64-rider-skins"},{"version",1},{"characters",nlohmann::json::array()}};
    Files files;
    if(argc==2){
        mods::ModOpenError error{};mods::ZipModFileHandle actual(std::filesystem::path(argv[1]),error);
        check(error==mods::ModOpenError::Good,"final mod opens");bool exists=false;const auto descriptor=actual.read_file("rr64-rider-skins.json",exists);check(exists,"final descriptor exists");
        marker=nlohmann::json::parse(descriptor.begin(),descriptor.end());
        for(const auto& a:marker["characters"])for(const auto& name:a["textures"]){
            const auto bytes=actual.read_file(name.get<std::string>(),exists);check(exists,"final texture exists");files.push_back({name.get<std::string>(),{bytes.begin(),bytes.end()}});
        }
    } else for(unsigned i=0;i<2;++i){
        const auto a=skin_fixture(i?"cortana":"punisher",i?"Cortana":"Punisher",i?10:0);
        nlohmann::json textures=nlohmann::json::array();const char* slots[]={"head","torso","body","far"};
        for(unsigned s=0;s<4;++s){const auto name=a.id+"-"+slots[s]+".ci8";textures.push_back(name);files.push_back({name,a.textures[s].bytes});}
        marker["characters"].push_back({{"id",a.id},{"name",a.name},{"donor",a.donor},{"textures",textures}});
    }
    check(!load(marker.dump(),files,"different_mod"),"unrelated mod cannot install catalog");
    for(const auto& bad:std::vector<nlohmann::json>{{{"version",2}},{{"version",-1}},{{"format","rr64-characters"}},{{"characters",nlohmann::json::array()}}}){
        auto invalid=marker;invalid.update(bad);check(!load(invalid.dump(),files)&&!skin::enabled(),"invalid/retired schema rejected");
    }
    auto invalid=marker;invalid["characters"][0]["donor"]=45;check(!load(invalid.dump(),files),"invalid donor rejected");
    invalid=marker;invalid["characters"][0]["textures"][0]="../escape.ci8";check(!load(invalid.dump(),files),"path traversal rejected");
    invalid=marker;invalid["characters"][0]["model"]="old.mesh";check(!load(invalid.dump(),files),"mesh metadata cannot reactivate retired renderer");
    invalid=marker;invalid["characters"][0]["turtle_shell"]="yes";check(!load(invalid.dump(),files),"shell flag must be boolean");
    invalid=marker;invalid["characters"][0]["dual_head"]="yes";check(!load(invalid.dump(),files),"separate cheeks flag must be boolean");
    invalid=marker;invalid["characters"][0]["donor"]=10;invalid["characters"][0]["dual_head"]=true;check(!load(invalid.dump(),files),"separate cheeks reject incompatible donor");
    invalid=marker;invalid["characters"][0]["dual_head"]=true;invalid["characters"][0]["turtle_shell"]=true;check(!load(invalid.dump(),files),"unsupported combined cosmetic geometry rejected");
    invalid=marker;invalid["characters"][0]["donor"]=10;invalid["characters"][0]["turtle_shell"]=true;check(!load(invalid.dump(),files),"shell rejects incompatible native donor");
    const auto duplicate=std::string("{\"format\":\"wrong\",")+marker.dump().substr(1);check(!load(duplicate,files),"duplicate JSON key rejected");
    auto missing=files;missing.pop_back();check(!load(marker.dump(),missing)&&!skin::enabled(),"missing last texture never partially publishes roster");
    missing=files;missing.push_back(files.front());check(!load(marker.dump(),missing),"duplicate ZIP texture identity rejected");
    missing=files;missing.back().second.push_back(0);check(!load(marker.dump(),missing),"oversized texture refused before extraction");
    missing=files;missing.back().second.back()=0;check(!load(marker.dump(),missing),"transparent palette rejected");
    // Mirror the real runtime boundary: on_init precedes load_mod callbacks,
    // although is_game_started is already true; only entrypoint seals assets.
    started=true;skin::prepare_session();
    check(skin::can_change()&&!skin::enabled()&&!policy.can_toggle(),"on_init permits content loading while UI mutation remains blocked");
    check(load(marker.dump(),files)&&skin::count()==marker["characters"].size(),"first-start content callback loads before entrypoint");
    for(unsigned i=0;i<skin::count();++i)check(skin::appearance(i+1)->donor==marker["characters"][i]["donor"],"catalog keeps every authored native donor");
    skin::unavailable("disabled");check(!skin::enabled(),"disabled uses native riders");
    check(load(marker.dump(),files),"identical archive rescans safely");skin::begin_session();check(!policy.can_toggle(),"game session locks catalog lifetime");
    const auto* held=skin::appearance(1);skin::set_menu_selection(0,1);skin::set_race_selection(0,1);
    skin::unavailable("hot disable");check(skin::appearance(1)==held&&!load(marker.dump(),files),"running session cannot replace or unload textures");
    skin::prepare_session();check(!skin::enabled()&&!skin::menu_selection(0)&&!skin::race_selection(0)&&!held->textures[0].bytes.empty(),"restart clears active choices while immutable assets stay alive");
    check(load(marker.dump(),files)&&skin::appearance(1)==held,"restart content callbacks reload identical catalog");
    skin::begin_session();skin::prepare_session();skin::begin_session();
    check(!skin::enabled()&&!skin::can_change(),"next session with mod disabled cannot retain old appearance");
    std::printf("Rider skin archive: %u checks passed.\n",checks);
}
