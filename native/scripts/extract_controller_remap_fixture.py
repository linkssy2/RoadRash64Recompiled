"""Extract the actual frontend assignment and SDL axis-scanning paths for tests.

The fixture links production profile/config, binding and SDL polling code. This
small extract avoids linking window creation and the rest of the UI event loop.
"""
from argparse import ArgumentParser
from pathlib import Path


def function(text: str, signature: str) -> str:
    start = text.index(signature)
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
        end += 1
    return text[start:end]


def main() -> None:
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("--frontend-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    # Isolated old-source controls can retain their original assignment code.
    parser.add_argument("--players-source", type=Path)
    args = parser.parse_args()
    source = args.frontend_root / "recompinput/src"
    players = (args.players_source or source / "players.cpp").read_text()
    events = (source / "input_events.cpp").read_text()
    state = players[players.index("using players_array"):
                    players.index("size_t players::get_number_of_assigned_players")]
    # RR64-SDL3: SDL3 event names.
    axis = events[events.index("    case SDL_EventType::SDL_EVENT_GAMEPAD_AXIS_MOTION:"):
                  events.index("    case SDL_EventType::SDL_EVENT_GAMEPAD_SENSOR_UPDATE:")]
    text = """// Generated from production; do not edit.
namespace recompui {
struct FixtureControls { void force_update() {} };
static FixtureControls* controls_page = nullptr;
}
namespace recompinput {
static std::recursive_mutex assignment_mutex;
static std::array<bool, max_num_players_supported> explicit_keyboard{};
"""
    text += state
    text += function(players, "bool players::select_player_device(") + "\n"
    text += function(players, "void playerassignment::commit_player_assignment(") + "\n}\n"
    text += """static void queue_if_enabled(SDL_Event*) {}
static void fixture_input_event(SDL_Event* event) {
    using namespace recompinput;
    switch(event->type) {
"""
    text += axis + "default: break;\n    }\n}\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text)


if __name__ == "__main__":
    main()
