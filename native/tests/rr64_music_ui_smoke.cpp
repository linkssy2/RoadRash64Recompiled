#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace recomp::config {
using ConfigValueVariant = std::variant<uint32_t, double, std::string, bool>;
struct Choice { uint32_t value; std::string name; };
struct ConfigOptionEnum { std::vector<Choice> options; };
struct Config {
    ConfigOptionEnum choices;
    ConfigValueVariant value{uint32_t(0)};
    bool disabled = false;
    std::vector<bool> choice_disabled;
    std::string details;
    template<class T> const T& get_option_config(size_t) const { return choices; }
    bool get_enum_option_disabled(size_t, uint32_t index) const { return choice_disabled.at(index); }
    const std::string& get_enum_option_details(size_t) const { return details; }
};
}

namespace recompui {
using ResourceId = unsigned;
enum class Display { Flex };
enum class FlexDirection { Row };
enum class AlignItems { Center };
enum class JustifyContent { FlexStart };
enum class Unit { Percent };
namespace theme {
enum class Typography { LabelXS };
namespace color { constexpr int Primary = 1; }
}
struct Element {
    bool enabled = true;
    Element(ResourceId, Element*, unsigned = 0, const char* = "div", bool = false) {}
    virtual ~Element() = default;
    virtual std::string_view get_type_name() { return "Element"; }
    void set_display(Display) {}
    void set_flex_direction(FlexDirection) {}
    void set_align_items(AlignItems) {}
    void set_justify_content(JustifyContent) {}
    void set_width(float, Unit) {}
    void set_gap(float) {}
    void set_margin_bottom(float) {}
    void set_color(int) {}
    void set_enabled(bool value) { enabled = value; }
};
struct Label : Element {
    std::string text;
    Label(ResourceId id, Element* parent, const std::string& text, theme::Typography) : Element(id, parent), text(text) {}
    void set_text(const std::string& value) { text = value; }
};
struct SelectOption {
    std::string text, value;
    SelectOption(std::string text, std::string value) : text(text), value(value) {}
};
struct Select : Element {
    std::vector<SelectOption> options;
    std::vector<Element> elements;
    std::string selected;
    std::function<void(bool)> focus, hover;
    std::function<void(SelectOption&,int)> change;
    Select(ResourceId id, Element* parent, std::vector<SelectOption> choices) : Element(id,parent), options(choices) {
        for (size_t i=0; i<choices.size(); ++i) elements.emplace_back(0,this);
    }
    void set_focus_callback(std::function<void(bool)> f) { focus=f; }
    void set_hover_callback(std::function<void(bool)> f) { hover=f; }
    void add_change_callback(std::function<void(SelectOption&,int)> f) { change=f; }
    Element* get_option_element(uint32_t i) { return &elements.at(i); }
    void set_selection(std::string value) {
        if (selected == value) return;
        selected=value;
        for (size_t i=0; i<options.size(); ++i) if (options[i].value==value && change) change(options[i],int(i));
    }
};
struct Radio : Element {
    std::vector<Element> elements;
    uint32_t index = 0;
    std::function<void(bool)> focus;
    std::function<void(uint32_t)> change;
    Radio(ResourceId id, Element* parent) : Element(id,parent) {}
    void set_focus_callback(std::function<void(bool)> f) { focus=f; }
    void add_index_changed_callback(std::function<void(uint32_t)> f) { change=f; }
    void add_option(const std::string&) { elements.emplace_back(0,this); }
    void set_index(uint32_t value) { index=value; }
    Element* get_option_element(uint32_t i) { return &elements.at(i); }
};
std::vector<std::unique_ptr<Element>> widgets;
struct ContextId {
    template<class T,class... Args> T* create_element(Args&&... args) {
        auto value=std::make_unique<T>(0,std::forward<Args>(args)...);
        auto result=value.get(); widgets.emplace_back(std::move(value)); return result;
    }
};
ContextId get_current_context() { return {}; }
using set_option_value_t = std::function<void(const std::string&,recomp::config::ConfigValueVariant)>;
using on_option_hover_t = std::function<void(const std::string&)>;
struct ConfigOptionElement : Element {
    std::string option_id;
    size_t option_index;
    const recomp::config::Config* config;
    set_option_value_t set_option_value;
    on_option_hover_t on_hover;
    Label label{0,this,"",theme::Typography::LabelXS};
    Label* name_label=&label;
    unsigned scrolled=0;
    ConfigOptionElement(ResourceId id,Element* parent,std::string name,size_t index,const recomp::config::Config* c,set_option_value_t setter,on_option_hover_t hover)
      : Element(id,parent),option_id(name),option_index(index),config(c),set_option_value(setter),on_hover(hover) {}
    recomp::config::ConfigValueVariant get_value() { return config->value; }
    bool get_disabled() { return config->disabled; }
    void scroll_into_view() { ++scrolled; }
    virtual void update_value()=0;
    virtual void update_disabled()=0;
};
#include "rr64_music_ui_fixture.inc"
struct Probe : ConfigOptionEnum {
    using ConfigOptionEnum::ConfigOptionEnum;
    using ConfigOptionEnum::select;
    using ConfigOptionEnum::radio;
    using ConfigOptionEnum::details_label;
};
}

int main() {
    using namespace recompui;
    for (unsigned songs : {0u,1u,15u,256u}) {
        recomp::config::Config config;
        config.choices.options={{0,"Original soundtrack"},{1,"Custom playlist"}};
        for (unsigned i=0;i<songs;++i) config.choices.options.push_back({100+i*3,std::string(512,'A')+" Song "+std::to_string(i)});
        config.choice_disabled.resize(config.choices.options.size());
        unsigned writes=0,descriptions=0;
        auto set=[&](const std::string& key,recomp::config::ConfigValueVariant value) { CHECK(key=="custom_music_track"); config.value=value; ++writes; };
        Probe music(0,nullptr,"custom_music_track",0,&config,set,[&](const std::string& key){CHECK(key=="custom_music_track");++descriptions;});
        CHECK(music.select && !music.radio);
        CHECK(music.select->options.size()==songs+2 && music.select->selected=="0" && writes==0);
        for (size_t i=0;i<config.choices.options.size();++i) {
            CHECK(music.select->options[i].text==config.choices.options[i].name);
            music.select->change(music.select->options[i],int(i));
            CHECK(std::get<uint32_t>(config.value)==config.choices.options[i].value);
            music.update_value();
            CHECK(music.select->selected==std::to_string(config.choices.options[i].value));
        }
        config.value=uint32_t(0); music.update_value();
        const unsigned unchanged=writes;
        music.update_value(); CHECK(writes==unchanged);
        config.choice_disabled[1]=true; music.update_disabled();
        CHECK(!music.select->elements[1].enabled && music.select->elements[0].enabled);
        music.select->change(music.select->options[1],1); CHECK(writes==unchanged);
        config.disabled=true; music.update_disabled(); CHECK(!music.select->enabled);
        music.select->change(music.select->options[0],-1);
        music.select->change(music.select->options[0],int(songs+2)); CHECK(writes==unchanged);
        config.disabled=false;config.choice_disabled[1]=false;music.update_disabled();
        CHECK(music.select->enabled && music.select->elements[1].enabled);
        music.select->focus(true);music.select->hover(true);
        CHECK(descriptions==2 && music.scrolled==1);
        config.details="Track details";music.update_enum_details();CHECK(music.details_label->text==config.details);
        Probe restored(0,nullptr,"custom_music_track",0,&config,set,[](const auto&){});
        CHECK(restored.select->selected==music.select->selected);
        config.value=uint32_t(42);music.update_value();CHECK(music.select->selected.empty());
        widgets.clear();
    }
    recomp::config::Config short_setting;
    short_setting.choices.options={{0,"Off"},{1,"On"}};short_setting.choice_disabled.resize(2);
    recompui::Probe existing(0,nullptr,"existing_setting",0,&short_setting,[](const auto&,auto){},[](const auto&){});
    CHECK(existing.radio && !existing.select);
    std::printf("PASS music selector: %u checks; 0/1/15/256 songs, actual enum UI methods\n",checks);
}
