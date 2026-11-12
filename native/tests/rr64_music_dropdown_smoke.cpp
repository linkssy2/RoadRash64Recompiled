// Real RmlUi layout/events with a no-op renderer: no game, GPU or window.
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::_Exit(1); } } while (0)
struct System : Rml::SystemInterface {
    bool LogMessage(Rml::Log::Type, const Rml::String& message) override {
        std::fprintf(stderr, "%s\n", message.c_str());
        return true;
    }
};
struct Renderer : Rml::RenderInterface {
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override { return 0; }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};
void exercise(Rml::Context* context, int count, int selected, int width, int height) {
    context->SetDimensions({width, height});
    const std::string rml = R"(<rml><head><style>
        body { width: 100%; height: 100%; font-family: LatoLatin; font-size: 18px; }
        div { display: block; }
        #page { position: absolute; top: 10px; left: 10px; width: 85%; height: 180px; overflow: auto; }
        .above { height: 130px; } .below { height: 600px; }
        select { display: block; position: relative; width: 100%; height: 48px; nav-up: auto; nav-down: auto; }
        selectbox { max-width: 100%; max-height: 320px; overflow-y: auto; overflow-x: hidden; padding: 8px 0; border: 1px #888; }
        option { display: block; height: 48px; padding: 12px; overflow: hidden; box-sizing: border-box; }
    </style></head><body><div id="page"><div class="above"></div>
        <select id="music"></select><div class="below"></div></div></body></rml>)";
    auto* document = context->LoadDocumentFromMemory(rml);
    CHECK(document);
    auto* control = dynamic_cast<Rml::ElementFormControlSelect*>(document->GetElementById("music"));
    auto* page = document->GetElementById("page");
    CHECK(control && page);
    for (int i = 0; i < count; ++i)
        control->Add("Track " + std::to_string(i) + " " + std::string(512, 'x'), std::to_string(i));
    control->SetSelection(selected);
    if (count > 3) control->GetOption(count - 2)->SetAttribute("disabled", true);
    document->Show();
    context->Update();
    context->Render();
    page->SetScrollTop(70.f);
    context->Update();
    context->Render();
    const float original_scroll = page->GetScrollTop();
    CHECK(original_scroll > 0.f);
    CHECK(control->Focus());
    context->ProcessKeyDown(Rml::Input::KI_RETURN, 0);
    context->ProcessKeyUp(Rml::Input::KI_RETURN, 0);
    context->Update();
    context->Render();
    auto* box = control->GetOption(0)->GetParentNode();
    auto check_visible = [&] {
        context->Update();
        context->Render();
        CHECK(box->IsPseudoClassSet("checked"));
        CHECK(std::abs(page->GetScrollTop() - original_scroll) < 0.1f);
        auto* item = control->GetOption(control->GetSelection());
        const float top = item->GetAbsoluteOffset(Rml::BoxArea::Border).y - box->GetAbsoluteOffset(Rml::BoxArea::Padding).y;
        CHECK(top >= -1.f);
        CHECK(top + item->GetOffsetHeight() <= box->GetClientHeight() + 1.f);
        CHECK(box->GetAbsoluteOffset(Rml::BoxArea::Border).y >= -1.f);
        CHECK(box->GetAbsoluteOffset(Rml::BoxArea::Border).y + box->GetOffsetHeight() <= height + 1.f);
    };
    check_visible(); // A saved late choice must be shown on the first open.
    if (count > 3) {
        control->SetSelection(count - 3);
        check_visible();
        context->ProcessKeyDown(Rml::Input::KI_DOWN, 0);
        context->ProcessKeyUp(Rml::Input::KI_DOWN, 0);
        CHECK(control->GetSelection() == count - 1); // Disabled choice is skipped.
        check_visible();
    }
    for (int i = 0; control->GetSelection() > 0 && i < count; ++i) {
        context->ProcessKeyDown(Rml::Input::KI_UP, 0);
        context->ProcessKeyUp(Rml::Input::KI_UP, 0);
        check_visible();
    }
    CHECK(control->GetSelection() == 0);
    context->ProcessKeyDown(Rml::Input::KI_RETURN, 0);
    context->ProcessKeyUp(Rml::Input::KI_RETURN, 0);
    context->Update();
    CHECK(!box->IsPseudoClassSet("checked"));
    // Reopening after a hidden change must reveal the new persisted choice.
    control->SetSelection(count - 1);
    context->ProcessKeyDown(Rml::Input::KI_RETURN, 0);
    context->ProcessKeyUp(Rml::Input::KI_RETURN, 0);
    check_visible();
    context->UnloadDocument(document);
    context->Update();
}
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    Renderer renderer;
    System system;
    Rml::SetSystemInterface(&system);
    Rml::SetRenderInterface(&renderer);
    CHECK(Rml::Initialise());
    CHECK(Rml::LoadFontFace(argv[1]));
    auto* context = Rml::CreateContext("music-dropdown", {1280, 720});
    CHECK(context);
    for (const auto dimensions : {Rml::Vector2i{1280, 720}, Rml::Vector2i{640, 360}})
        for (int count : {2, 3, 17, 258})
            exercise(context, count, count - 1, dimensions.x, dimensions.y);
    Rml::RemoveContext("music-dropdown");
    Rml::Shutdown();
    std::printf("PASS music dropdown: %u real layout/event checks; first-open, keys, disabled choices, local scroll and compact viewport\n", checks);
}
