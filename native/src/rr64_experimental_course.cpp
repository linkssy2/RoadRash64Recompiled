#include "rr64_experimental_course.hpp"
#include "rr64_diagnostic_options.hpp"
#include "rr64_mk64_item_render.hpp"
#include "rr64_mk64_item_material.hpp"
#include "rr64_mk64_item_audio.hpp"
#include "rr64_race_pack.hpp"
#include "rr64_race_pack_menu.hpp"
#include "rr64_course_material.hpp"
#include "rr64_course_boost.hpp"
#include "rr64_course_audio.hpp"
#include "rr64_course_music.hpp"
#include "rr64_local_race_options.hpp"
#include "rr64_course_sky.hpp"
#include "rr64_course_items.hpp"
#include "rr64_course_item_render.hpp"
#include "rr64_course_walls.hpp"
#include "rr64_course_hazards.hpp"
#include "rr64_course_hazard_render.hpp"
#include "rr64_race_pack_identity.hpp"
#include "rr64_race_pack_mod.hpp"
#include "rr64_race_pack_digest.hpp"
#include "librecomp/game.hpp"
#include "json/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace rr64::experimental_course {
namespace {
using Json = nlohmann::json;
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t original_size = 0x2000000, original_table = 0x18d380;
constexpr unsigned original_textures = 926, cells_count = 4900, maximum_textures = 1280;
constexpr const char *original_sha =
    "74e49e863484b5d17dcbe3891b99f2cafe3cf7511aa1dc5427022f699301db73";
struct Course {
    std::string id, name;
    Bytes preview, records;
    Bytes recovery_support;
    std::vector<float> heights;
    std::vector<course_items::ItemBoxDefinition> items;
    std::shared_ptr<const course_walls::World> walls, surfaces;
    course_hazards::Data hazards;
    course_sky::Definition sky;
    course_boost::Data boosts;
    std::vector<std::uint32_t> wood_surfaces;
    RouteData route{};
};
struct Catalogue {
    std::vector<Course> courses;
    std::vector<race_pack::CourseMenuEntry> menu;
    std::array<int, cells_count> owners;
    std::array<std::uint8_t, 32> digest{};
    unsigned texture_count = original_textures;
    float source_to_world_scale = .05f;
    Catalogue() { owners.fill(-1); }
};
Catalogue catalogue;
std::atomic_bool installed_flag{false};
std::atomic_int selected{-1}, loaded{-1};
[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error("Race pack: " + message);
}
void require(bool ok, const char *message) {
    if (!ok)
        fail(message);
}
unsigned word(std::span<const std::uint8_t> b, std::size_t p) {
    require(p <= b.size() && b.size() - p >= 4, "record outside file");
    return (unsigned(b[p]) << 24) | (unsigned(b[p + 1]) << 16) | (unsigned(b[p + 2]) << 8) |
           b[p + 3];
}
unsigned half(std::span<const std::uint8_t> b, std::size_t p) {
    require(p <= b.size() && b.size() - p >= 2, "record outside file");
    return (unsigned(b[p]) << 8) | b[p + 1];
}
void put_word(Bytes &b, std::size_t p, unsigned v) {
    require(p <= b.size() && b.size() - p >= 4, "output outside bank");
    for (unsigned i = 0; i < 4; ++i)
        b[p + i] = std::uint8_t(v >> (24 - i * 8));
}
void put_half(Bytes &b, std::size_t p, unsigned v) {
    b.at(p) = std::uint8_t(v >> 8);
    b.at(p + 1) = std::uint8_t(v);
}
Bytes read(const std::filesystem::path &path, std::size_t maximum) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        fail("cannot read " + path.filename().string());
    const auto n = in.tellg();
    require(n > 0 && std::uint64_t(n) <= maximum, "file exceeds bounded size");
    Bytes result(static_cast<std::size_t>(n));
    in.seekg(0);
    require(bool(in.read(reinterpret_cast<char *>(result.data()), result.size())),
            "incomplete file");
    return result;
}
bool identifier(const std::string &s) {
    return !s.empty() && s.size() < 32 &&
           s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos;
}
std::filesystem::path checked_path(const std::filesystem::path &root, const std::string &name) {
    const auto relative = std::filesystem::path(name);
    require(!relative.empty() && !relative.is_absolute() && !relative.has_root_name(),
            "absolute asset path");
    for (const auto &part : relative)
        require(part != ".." && part != ".", "asset path traverses directories");
    auto resolved = std::filesystem::weakly_canonical(root / relative);
    auto r = root.begin(), p = resolved.begin();
    for (; r != root.end(); ++r, ++p)
        require(p != resolved.end() && *p == *r, "asset escapes pack folder");
    return resolved;
}
float number(const Json &j) {
    const float n = j.get<float>();
    require(std::isfinite(n), "nonfinite route metadata");
    return n;
}
void decode_route(Course &course, const Json &metadata) {
    require(metadata.at("format") == "rr64-private-course-route" && metadata.at("version") == 2 &&
                metadata.at("course") == course.id,
            "unsupported/mismatched route");
    const auto &n = metadata.at("native");
    auto &r = course.route;
    r.byte_count = static_cast<unsigned>(course.records.size());
    r.records_be = course.records.data();
    r.record_count = n.at("record_count").get<unsigned>();
    r.wrap_segment = n.at("wrap_segment").get<unsigned>();
    r.finish_segment = n.at("finish_segment").get<unsigned>();
    r.initial_adjustment = n.at("initial_adjustment").get<unsigned>();
    r.prior_laps_required = n.at("prior_laps_required").get<unsigned>();
    r.lap_period = number(n.at("lap_period"));
    r.lap_threshold = number(n.at("lap_threshold"));
    r.finish_threshold = number(n.at("finish_threshold"));
    r.finish_parameter = number(n.at("finish_parameter"));
    for (unsigned i = 0; i < 3; ++i) {
        r.start[i] = number(n.at("start").at(i));
        r.finish[i] = number(n.at("finish").at(i));
    }
    for (const auto &h : metadata.at("record_heights"))
        course.heights.push_back(number(h));
    r.record_heights = course.heights.data();
    r.height_count = static_cast<unsigned>(course.heights.size());
    const auto &grid = metadata.at("source_grid");
    require(grid.size() == 14, "course requires all fourteen grid positions");
    for (unsigned i = 0; i < 14; ++i) {
        const auto &g = grid.at(i);
        r.grid[i] = {number(g.at("position").at(0)), number(g.at("position").at(1)),
                     number(g.at("position").at(2)), number(g.at("heading"))};
    }
    r.spawn_count = 14;
    r.native_laps = true;
    if (metadata.contains("recovery_support")) {
        const auto &support = metadata.at("recovery_support");
        require(support.is_array() && support.size() == r.wrap_segment / 2,
                "invalid recovery support table");
        for (const auto &entry : support) {
            require(entry.is_number_unsigned() && entry.get<unsigned>() <= 1,
                    "invalid recovery support flag");
            course.recovery_support.push_back(entry.get<std::uint8_t>());
        }
        r.recovery_support = course.recovery_support.data();
        r.recovery_support_count = static_cast<unsigned>(course.recovery_support.size());
    }
    validate_route(r);
}
void decode_items(Course &course, const Json &metadata, float source_scale) {
    require(metadata.at("format") == "rr64-course-items" && metadata.at("version") == 1 &&
                metadata.at("course") == course.id,
            "unsupported/mismatched item definitions");
    const auto &boxes = metadata.at("boxes");
    require(boxes.is_array() && boxes.size() <= netplay::kMaximumCourseItems,
            "item box count exceeds limit");
    for (const auto &entry : boxes) {
        course_items::ItemBoxDefinition box;
        box.id = entry.at("id").get<unsigned>();
        require(box.id == course.items.size(), "noncontiguous item box identity");
        require(entry.at("position").size() == 3, "invalid item box center");
        for (unsigned axis = 0; axis < 3; ++axis) {
            box.position[axis] = number(entry.at("position").at(axis));
            require(std::abs(box.position[axis]) < 100000, "item box outside world bounds");
        }
        box.radius = number(entry.at("radius"));
        const unsigned kind = entry.at("kind").get<unsigned>();
        // The authored box contact radius scales with the imported world.
        // A fixed one-unit cap silently rejected otherwise valid larger packs.
        require(box.radius > 0 && box.radius <= 5.5f * source_scale + .00001f &&
                    (kind == 2 || kind == 5),
                "invalid item box shape/type");
        box.kind = static_cast<std::uint8_t>(kind);
        box.parent_hazard = entry.value("parent_hazard", ~0u);
        if (box.parent_hazard != ~0u) {
            require(box.parent_hazard < netplay::kMaximumCourseHazards &&
                        entry.contains("parent_offset") && entry.at("parent_offset").size() == 3,
                    "invalid moving item parent");
            for (unsigned axis = 0; axis < 3; ++axis) {
                box.parent_offset[axis] = number(entry.at("parent_offset")[axis]);
                require(std::abs(box.parent_offset[axis]) <= 100, "invalid moving item offset");
            }
        }
        course.items.push_back(box);
    }
}
std::array<float, 3> vector3(const Json &j) {
    require(j.is_array() && j.size() == 3, "invalid world vector");
    std::array<float, 3> value{};
    for (unsigned i = 0; i < 3; ++i) {
        value[i] = number(j[i]);
        require(std::abs(value[i]) < 100000, "world vector exceeds bounds");
    }
    return value;
}
void decode_walls(Course &course, const Json &j, bool surfaces,
                  std::vector<std::uint32_t> *surface_ids = nullptr) {
    require(j.at("format") == (surfaces ? "rr64-course-surfaces" : "rr64-course-walls") &&
                j.at("version") == 1 && j.at("course") == course.id,
            "invalid collision mesh identity");
    const auto &faces = j.at("triangles");
    require(faces.is_array() && faces.size() <= 65536, "collision face limit");
    std::vector<course_walls::Triangle> triangles;
    triangles.reserve(faces.size());
    for (const auto &face : faces) {
        course_walls::Triangle t;
        t.id = face.at("id").get<unsigned>();
        if (surfaces && surface_ids)
            surface_ids->push_back(t.id);
        require(face.at("vertices").size() == 3, "collision triangle vertices");
        for (unsigned i = 0; i < 3; ++i)
            t.vertices[i] = vector3(face.at("vertices")[i]);
        if (surfaces && face.contains("source") &&
            face.at("source").value("surface", std::string()) == "GRASS")
            course.hazards.grass_triangles.push_back(t.id);
        if (surfaces)
            for (const auto &vertex : t.vertices)
                course.route.fall_floor = std::min(course.route.fall_floor, vertex[2] - 8.f);
        triangles.push_back(t);
    }
    if (surfaces) {
        std::sort(course.hazards.grass_triangles.begin(), course.hazards.grass_triangles.end());
        course.surfaces = course_walls::build_surface_world(triangles);
    } else {
        std::vector<course_walls::Rail> rails;
        if (j.contains("rails")) {
            const auto &records = j.at("rails");
            require(records.is_array() && records.size() <= triangles.size() / 2,
                    "collision rail limit");
            for (const auto &record : records) {
                course_walls::Rail rail;
                rail.id = record.at("id").get<unsigned>();
                require(record.at("triangle_ids").size() == 2 &&
                            record.at("base").size() == 2 && record.at("top").size() == 2,
                        "collision rail endpoints");
                for (unsigned i = 0; i < 2; ++i) {
                    rail.triangle_ids[i] = record.at("triangle_ids")[i].get<unsigned>();
                    rail.base[i] = vector3(record.at("base")[i]);
                    rail.top[i] = vector3(record.at("top")[i]);
                }
                rails.push_back(rail);
            }
        }
        course.walls = course_walls::build_world(triangles, rails);
    }
}
void decode_hazards(Course &course, const Json &j) {
    using namespace course_hazards;
    require(j.at("format") == "rr64-course-hazards" && j.at("version") == 1 &&
                j.at("course") == course.id,
            "invalid hazard identity");
    const auto &paths = j.at("paths"), &definitions = j.at("definitions");
    require(paths.is_array() && paths.size() <= 16 && definitions.is_array() &&
                definitions.size() <= netplay::kMaximumCourseHazards,
            "hazard capacity exceeded");
    for (const auto &path : paths) {
        auto &out = course.hazards.paths.emplace_back();
        const auto &points = path.at("points");
        require(points.size() >= 5 && points.size() <= 4096, "invalid hazard path length");
        for (const auto &p : points)
            out.points.push_back(vector3(p));
        if (path.contains("durations")) {
            const auto &durations = path.at("durations");
            require(durations.is_array() && durations.size() == points.size(),
                    "invalid actor spline duration count");
            for (const auto &duration : durations) {
                require(duration.is_number_unsigned() || duration.is_number_integer(),
                        "invalid actor spline duration");
                const auto value = duration.get<std::int64_t>();
                require(value > 0 && value <= 10000, "actor spline duration out of range");
                out.durations.push_back(static_cast<unsigned>(value));
            }
        }
        if (path.contains("left") || path.contains("right")) {
            require(path.at("left").size() == points.size() &&
                        path.at("right").size() == points.size(),
                    "hazard lane size mismatch");
            for (const auto &p : path.at("left"))
                out.left.push_back(vector3(p));
            for (const auto &p : path.at("right"))
                out.right.push_back(vector3(p));
        }
    }
    for (const auto &entry : definitions) {
        Definition d;
        d.id = entry.at("id").get<unsigned>();
        require(d.id == course.hazards.definitions.size(), "noncontiguous hazard identity");
        const std::string kind = entry.at("kind");
        static constexpr std::array<std::pair<const char *, Kind>, 25> kinds{
            {{"thwomp", Kind::Thwomp},     {"rock", Kind::Rock},         {"train", Kind::Train},
             {"traffic", Kind::Traffic},   {"mole", Kind::Mole},         {"crab", Kind::Crab},
             {"hedgehog", Kind::Hedgehog}, {"plant", Kind::Plant},       {"snowman", Kind::Snowman},
             {"egg", Kind::Egg},           {"penguin", Kind::Penguin},   {"chomp", Kind::Chomp},
             {"kiwano", Kind::Kiwano},     {"crossing", Kind::Crossing}, {"ferry", Kind::Ferry},
             {"bat", Kind::Bat},           {"boo", Kind::Boo},           {"fish", Kind::Fish},
             {"flame", Kind::Flame},       {"smoke", Kind::Smoke},       {"wheel", Kind::Wheel},
             {"neon", Kind::Neon},         {"balloon", Kind::Balloon},   {"seagull", Kind::Seagull},
             {"sign", Kind::Sign}}};
        const auto found = std::find_if(kinds.begin(), kinds.end(),
                                        [&](const auto &item) { return kind == item.first; });
        require(found != kinds.end(), "unknown hazard kind");
        d.kind = found->second;
        d.model = entry.at("model").get<unsigned>();
        d.subtype = entry.value("subtype", 0u);
        d.phase = entry.value("phase", 0u);
        d.position = vector3(entry.at("position"));
        d.offset = vector3(entry.at("offset"));
        d.half_extent = vector3(entry.at("half_extent"));
        d.scale = number(entry.at("scale"));
        d.speed = number(entry.at("speed"));
        d.lane = number(entry.at("lane"));
        d.minimum_height = number(entry.at("minimum_height"));
        d.path = entry.value("path", 0u);
        d.node = entry.value("node", 0u);
        d.animation_frames = entry.value("animation_frames", 1u);
        d.frame_ticks = entry.value("frame_ticks", 1u);
        d.parent = entry.value("parent", ~0u);
        if (entry.contains("target"))
            d.target = vector3(entry.at("target"));
        else
            d.target = d.position;
        const std::string billboard = entry.value("billboard", std::string("none"));
        require(billboard == "none" || billboard == "yaw" || billboard == "full",
                "invalid actor billboard");
        d.billboard = billboard == "yaw"    ? Billboard::Yaw
                      : billboard == "full" ? Billboard::Full
                                            : Billboard::None;
        d.solid = entry.value("solid", true);
        d.collision_radius =
            entry.contains("collision_radius") ? number(entry.at("collision_radius")) : 0;
        if (entry.contains("rotation")) {
            require(entry.at("rotation").size() == 3, "invalid actor rotation");
            for (unsigned axis = 0; axis < 3; ++axis) {
                const auto angle = entry.at("rotation")[axis].get<unsigned>();
                require(angle <= 65535, "invalid actor angle");
                d.rotation[axis] = static_cast<std::uint16_t>(angle);
            }
        }
        if (entry.contains("frame_sequence")) {
            const auto &sequence = entry.at("frame_sequence");
            require(sequence.is_array() && !sequence.empty() && sequence.size() <= 4096,
                    "invalid actor frame schedule");
            for (const auto &frame : sequence) {
                const auto index = frame.get<unsigned>();
                require(index < d.animation_frames, "actor frame schedule out of bounds");
                d.frame_sequence.push_back(index);
            }
        }
        if (entry.contains("clips")) {
            const auto &clips = entry.at("clips");
            require(clips.is_array() && clips.size() <= 3, "invalid actor clip count");
            for (unsigned i = 0; i < clips.size(); ++i) {
                d.clip_start[i] = clips[i].at("start").get<unsigned>();
                d.clip_count[i] = clips[i].at("count").get<unsigned>();
                require(d.clip_start[i] < d.animation_frames && d.clip_count[i] &&
                            d.clip_count[i] <= d.animation_frames - d.clip_start[i],
                        "invalid actor clip");
            }
        }
        d.animation_loop_start = entry.value("animation_loop_start", 0u);
        require(d.frame_sequence.empty() ? d.animation_loop_start == 0
                                         : d.animation_loop_start < d.frame_sequence.size(),
                "invalid actor loop start");
        if (entry.contains("visible_sequence")) {
            const auto &sequence = entry.at("visible_sequence");
            require(sequence.is_array() && !sequence.empty() &&
                        sequence.size() == d.frame_sequence.size(),
                    "invalid actor visibility schedule");
            for (const auto &visible : sequence) {
                require(visible.is_boolean(), "invalid actor visibility flag");
                d.visible_sequence.push_back(visible.get<bool>());
            }
        }
        require(d.animation_frames && d.animation_frames <= 1024 && d.frame_ticks &&
                    d.frame_ticks <= 600 && d.model < netplay::kMaximumCourseHazardModels &&
                    d.animation_frames <= netplay::kMaximumCourseHazardModels - d.model &&
                    (d.parent == ~0u || d.parent < d.id) && d.collision_radius >= 0 &&
                    d.collision_radius <= 100,
                "invalid actor animation/collision");
        require(d.model < netplay::kMaximumCourseHazardModels && d.scale > 0 && d.scale <= 4 &&
                    d.speed >= 0 && d.speed <= 10 && std::abs(d.lane) <= 1 &&
                    std::abs(d.minimum_height) < 100000,
                "invalid hazard parameters");
        for (unsigned i = 0; i < 3; ++i)
            require(d.half_extent[i] > 0 && d.half_extent[i] <= 100 && std::abs(d.offset[i]) <= 100,
                    "invalid hazard collider");
        if (d.kind == Kind::Thwomp)
            require(d.model == 0 && d.subtype >= 1 && d.subtype <= 6 && d.phase <= 3,
                    "invalid Thwomp configuration");
        if (d.kind == Kind::Rock)
            require(d.phase < 3, "invalid rock delay");
        if (d.kind == Kind::Train || d.kind == Kind::Traffic) {
            require(d.path < course.hazards.paths.size() &&
                        d.node < course.hazards.paths[d.path].points.size() && d.speed > 0,
                    "invalid vehicle path reference");
            if (d.kind == Kind::Traffic)
                require(d.subtype <= 2 && !course.hazards.paths[d.path].left.empty(),
                        "traffic needs lane geometry");
        }
        d.collision_model = entry.value("collision_model", ~0u);
        require(d.collision_model == ~0u ||
                    (d.kind == Kind::Train && d.collision_model >= 13 && d.collision_model <= 15),
                "invalid articulated collision model");
        if (d.kind == Kind::Ferry || d.kind == Kind::Chomp || d.kind == Kind::Seagull ||
            d.kind == Kind::Kiwano || (d.kind == Kind::Penguin && d.subtype == 0))
            require(d.path < course.hazards.paths.size() &&
                        course.hazards.paths[d.path].points.size() >= 5 &&
                        d.node < course.hazards.paths[d.path].points.size(),
                    "invalid scene path");
        if (d.kind == Kind::Boo) {
            require(d.path < course.hazards.paths.size(), "invalid Boo path");
            const auto &path = course.hazards.paths[d.path];
            require(path.points.size() >= 4 && path.durations.size() == path.points.size() &&
                        d.animation_frames >= 58,
                    "Boo requires complete spline and views");
        }
        if (d.kind == Kind::Bat)
            require(d.subtype == 1 || d.subtype == 2, "invalid bat group");
        if (d.kind == Kind::Flame)
            require(d.subtype == 0 || d.subtype == 9 || (d.subtype == 4 && d.phase < 20) ||
                        (d.subtype == 5 && d.phase < 10),
                    "invalid flame state");
        if (d.kind == Kind::Smoke) {
            require(d.parent < d.id &&
                        (d.subtype == 0 || d.subtype == 1 || d.subtype == 3 || d.subtype == 4),
                    "invalid particle parent/type");
            const auto parent = course.hazards.definitions[d.parent].kind;
            require(parent == (d.subtype == 0   ? Kind::Train
                               : d.subtype == 1 ? Kind::Ferry
                               : d.subtype == 3 ? Kind::Mole
                                                : Kind::Snowman),
                    "wrong particle parent");
        }
        if (d.kind == Kind::Wheel)
            require(d.parent < d.id && (course.hazards.definitions[d.parent].kind == Kind::Train ||
                                        course.hazards.definitions[d.parent].kind == Kind::Ferry),
                    "invalid wheel parent");
        if (d.kind == Kind::Snowman) {
            require(d.subtype <= 1, "invalid snowman part");
            if (d.subtype == 1)
                require(d.parent < d.id &&
                            course.hazards.definitions[d.parent].kind == Kind::Snowman &&
                            course.hazards.definitions[d.parent].subtype == 0,
                        "invalid snowman body parent");
        }
        if (d.kind == Kind::Penguin)
            require(d.subtype < 15 && d.clip_count[0] &&
                        (d.subtype <= 8 || (d.clip_count[1] && d.clip_count[2])),
                    "invalid penguin clips/identity");
        if (d.kind == Kind::Kiwano)
            require(d.subtype < 14, "invalid Kiwano target");
        if (d.kind == Kind::Mole)
            require(d.subtype >= 1 && d.subtype <= 3, "invalid mole pool");
        if (d.kind == Kind::Boo || d.kind == Kind::Fish || d.kind == Kind::Flame ||
            d.kind == Kind::Smoke || d.kind == Kind::Wheel || d.kind == Kind::Crossing ||
            d.kind == Kind::Neon || d.kind == Kind::Balloon || d.kind == Kind::Seagull)
            require(!d.solid && d.collision_radius == 0, "decorative actor cannot have collision");
        course.hazards.definitions.push_back(d);
    }
}
Bytes original_record(std::span<const std::uint8_t> rom, unsigned index) {
    const auto ref = original_table + 24 + std::size_t(index) * 12;
    const auto offset = word(rom, ref), bytes = word(rom, ref + 4);
    require(offset && bytes && offset <= rom.size() - ref && bytes <= rom.size() - ref - offset,
            "original terrain reference invalid");
    return Bytes(rom.begin() + ref + offset, rom.begin() + ref + offset + bytes);
}
// Relocate a complete immutable bank ONCE before native threads start. Original
// ROM bytes remain untouched; stock textures/cells retain their exact payloads.
Bytes compose(std::span<const std::uint8_t> rom, const Json &manifest,
              const std::map<std::string, Bytes> &files, Catalogue &out) {
    const auto &added = manifest.at("textures");
    require(added.is_array() && !added.empty() &&
                added.size() <= maximum_textures - original_textures,
            "native texture dependency limit exceeded");
    out.texture_count = original_textures + static_cast<unsigned>(added.size());
    Bytes bank(24 + (out.texture_count + cells_count) * 12, 0);
    bank.resize((bank.size() + 7) & ~std::size_t(7));
    std::copy_n(rom.begin() + original_table, 24, bank.begin());
    put_half(bank, 20, out.texture_count);
    auto append = [&](unsigned index, const Bytes &data) {
        require(!data.empty() && data.size() % 8 == 0 && data.size() <= 0x7fff8,
                "invalid native record length");
        const auto ref = 24 + std::size_t(index) * 12, offset = bank.size();
        require(offset >= ref && offset + data.size() <= 32 * 1024 * 1024,
                "terrain bank exceeds budget");
        put_word(bank, ref, static_cast<unsigned>(offset - ref));
        put_word(bank, ref + 4, static_cast<unsigned>(data.size()));
        bank.insert(bank.end(), data.begin(), data.end());
    };
    for (unsigned i = 0; i < original_textures; ++i)
        append(i, original_record(rom, i));
    for (unsigned i = 0; i < added.size(); ++i) {
        const auto &t = added.at(i);
        require(t.at("index") == original_textures + i, "noncontiguous texture ID");
        const auto &b = files.at(t.at("file").get<std::string>());
        require(word(b, 0) == 0x16 && word(b, 4) == b.size() && word(b, 0x34) == 16,
                "invalid imported texture");
        append(original_textures + i, b);
    }
    for (unsigned i = 0; i < cells_count; ++i) {
        const auto ref = original_table + 24 + (original_textures + i) * 12;
        if (word(rom, ref))
            append(out.texture_count + i, original_record(rom, original_textures + i));
    }
    for (const auto &entry : manifest.at("cells")) {
        const auto i = entry.at("index").get<unsigned>();
        require(i < cells_count && out.owners[i] < 0, "invalid/duplicate imported cell");
        require(word(rom, original_table + 24 + (original_textures + i) * 12) == 0,
                "import would replace original terrain");
        const auto id = entry.at("course").get<std::string>();
        const auto owner = std::find_if(out.courses.begin(), out.courses.end(),
                                        [&](const auto &c) { return c.id == id; });
        require(owner != out.courses.end(), "unknown cell owner");
        const auto &b = files.at(entry.at("file").get<std::string>());
        require(word(b, 0) == 0x3f && word(b, 4) == b.size() && word(b, 8) == i && b.size() >= 0xf0,
                "invalid imported partition");
        for (unsigned s = 0; s < half(b, 12); ++s) {
            auto ref = 0xf0 + s * 12;
            auto mesh = ref + word(b, ref);
            auto t = half(b, mesh + 18);
            require(t == 0xffff || (t >= original_textures && t < out.texture_count),
                    "partition texture outside imported range");
        }
        out.owners[i] = static_cast<int>(owner - out.courses.begin());
        append(out.texture_count + i, b);
    }
    put_word(bank, 4, static_cast<unsigned>(bank.size()));
    Bytes result(rom.begin(), rom.end());
    result.insert(result.end(), bank.begin(), bank.end());
    return result;
}
} // namespace

bool installed() noexcept { return installed_flag.load(std::memory_order_acquire); }
bool active() noexcept { return installed() && loaded.load(std::memory_order_acquire) >= 0; }
unsigned terrain_rom_offset() noexcept {
    return installed() ? unsigned(original_size) : unsigned(original_table);
}
unsigned terrain_texture_count() noexcept {
    return installed() ? catalogue.texture_count : original_textures;
}
void decode_sky(Course &course, const Json &j) {
    require(j.at("version") == 1, "unsupported sky definition");
    auto &d = course.sky;
    d.enabled = true;
    d.stars = j.at("stars").get<bool>();
    d.center = vector3(j.at("center"));
    d.radius = number(j.at("radius"));
    require(d.radius > 0 && d.radius <= 100000, "sky radius exceeds supported range");
    const auto &colors = j.at("colors");
    require(colors.size() == 4, "sky requires four horizon colors");
    for (unsigned i = 0; i < 4; ++i) {
        require(colors.at(i).size() == 3, "invalid sky color");
        for (unsigned n = 0; n < 3; ++n) {
            const auto &channel = colors.at(i).at(n);
            require(channel.is_number_unsigned() && channel.get<unsigned>() <= 255,
                    "sky channel exceeds byte range");
            d.colors[i][n] = channel.get<std::uint8_t>();
        }
    }
    const auto &textures = j.at("textures"), &sprites = j.at("sprites");
    require(textures.is_array() && textures.size() <= 4 && sprites.is_array() &&
                sprites.size() <= 64,
            "sky asset count limit");
    for (const auto &entry : textures) {
        course_sky::Texture t;
        t.width = entry.at("width").get<unsigned>();
        t.height = entry.at("height").get<unsigned>();
        require((t.width == 16 && t.height == 16) || (t.width == 64 && t.height == 32),
                "unsupported sky texture dimensions");
        const auto hex = entry.at("intensity4").get<std::string>();
        require(hex.size() == t.width * t.height, "sky texture byte count");
        auto digit = [](char c) -> unsigned {
            if (c >= '0' && c <= '9')
                return unsigned(c - '0');
            if (c >= 'a' && c <= 'f')
                return unsigned(c - 'a' + 10);
            fail("invalid sky texture encoding");
        };
        for (std::size_t i = 0; i < hex.size(); i += 2)
            t.intensity4.push_back(std::uint8_t(digit(hex[i]) * 16 + digit(hex[i + 1])));
        d.textures.push_back(std::move(t));
    }
    for (const auto &entry : sprites) {
        course_sky::Sprite s;
        s.yaw = entry.at("yaw").get<unsigned>();
        s.texture = entry.at("texture").get<unsigned>();
        s.elevation = number(entry.at("elevation"));
        s.size = number(entry.at("size"));
        require(s.yaw <= 65535 && s.texture < d.textures.size() && std::abs(s.elevation) <= 120 &&
                    s.size > 0 && s.size <= 3,
                "invalid sky sprite");
        d.sprites.push_back(s);
    }
}
float source_to_world_scale() noexcept {
    return installed() ? catalogue.source_to_world_scale : .05f;
}
const RouteData *route_data() noexcept {
    const auto i = loaded.load(std::memory_order_acquire);
    return installed() && i >= 0 ? &catalogue.courses[static_cast<unsigned>(i)].route : nullptr;
}
bool cell_allowed(unsigned cell) noexcept {
    if (!installed())
        return true;
    if (cell >= cells_count)
        return false;
    const auto course = loaded.load(std::memory_order_acquire);
    return catalogue.owners[cell] == course;
}
void load_selection() noexcept {
    course_music::reset_runtime();
    course_audio::reset_runtime();
    course_audio::set_wood_surfaces({});
    course_boost::reset_runtime();
    course_items::reset_runtime();
    course_hazards::reset_runtime();
    loaded.store(selected.load(std::memory_order_acquire), std::memory_order_release);
    const auto index = loaded.load(std::memory_order_acquire);
    if (installed() && index >= 0 && static_cast<unsigned>(index) < catalogue.courses.size()) {
        course_audio::set_wood_surfaces(catalogue.courses[static_cast<unsigned>(index)].wood_surfaces,
                                      catalogue.courses[static_cast<unsigned>(index)].id == "banshee_boardwalk");
        const auto &id = catalogue.courses[static_cast<unsigned>(index)].id;
        course_music::select_course(id);
        const bool music_enabled = local_race_options::course_music_enabled(id);
        course_music::set_enabled(music_enabled);
        if (rr64::diagnostics::routine_enabled()) {
            std::fprintf(stderr, "[RR64-COURSE-MUSIC] course=%s enabled=%u bank=%u\n",
                         id.c_str(), unsigned(music_enabled), unsigned(course_music::available()));
        }
    }
}
void load_stock() noexcept {
    course_music::reset_runtime();
    course_audio::reset_runtime();
    course_audio::set_wood_surfaces({});
    course_boost::reset_runtime();
    course_items::reset_runtime();
    course_hazards::reset_runtime();
    loaded.store(-1, std::memory_order_release);
}

namespace {
void reset_assets() {
    mk64_items::reset_audio();
    mk64_items::clear_audio_bank();
    mk64_items::reset_render_session();
    mk64_items::reset_material_session();
    mk64_items::clear_render_asset();
    course_music::reset_runtime();
    course_music::unload_bank();
    course_audio::reset_runtime();
    course_audio::set_wood_surfaces({});
    course_audio::unload_bank();
    course_boost::reset_runtime();
    reset_descriptor();
    rr64_race_pack_menu_reset_session();
    rr64_course_material_reset_scratch();
    course_sky::reset();
    course_items::reset_runtime();
    course_items::reset_render_session();
    course_items::clear_render_asset();
    course_hazards::reset_runtime();
    course_hazards::reset_hazard_render_session();
    course_hazards::clear_hazard_asset();
    installed_flag.store(false, std::memory_order_release);
    selected = -1;
    loaded = -1;
    catalogue = Catalogue{};
}

void load_pack(const std::filesystem::path &configured) {
    const auto root = std::filesystem::weakly_canonical(configured);
    const auto metadata = read(root / "catalogue.json", 2 * 1024 * 1024);
    const auto manifest = Json::parse(metadata.begin(), metadata.end());
    require(manifest.at("format") == "rr64-race-pack-catalogue" && manifest.at("version") == 1 &&
                manifest.at("group_id") == "mk64" && manifest.at("group_name") == "MK64",
            "unsupported catalogue");
    require(manifest.value("mk64_items_version", 0u) == 1u &&
                manifest.contains("mk64_items_asset") && manifest.contains("mk64_items_audio_asset"),
            "Reimport your Mario Kart 64 ROM to add the full item set.");
    const auto rom = recomp::get_rom();
    require(rom.size() == original_size &&
                race_pack::hex_digest(race_pack::sha256(rom)) == original_sha,
            "requires the validated original Road Rash 64 USA ROM");
    std::map<std::string, Bytes> files;
    std::size_t total = 0;
    const auto music_asset = manifest.value("course_music_asset", std::string{});
    std::size_t music_bytes = 0;
    require(manifest.at("files").size() <= 4096, "too many assets");
    for (const auto &entry : manifest.at("files")) {
        const auto name = entry.at("file").get<std::string>();
        // Only the explicitly declared, validated soundtrack bank has the
        // larger PCM budget. Terrain, textures and all other assets retain
        // their original individual and aggregate limits.
        const bool is_music = !music_asset.empty() && name == music_asset;
        auto bytes = read(checked_path(root, name), (is_music ? 128u : 8u) * 1024 * 1024);
        require(bytes.size() == entry.at("bytes").get<std::size_t>() &&
                    race_pack::hex_digest(race_pack::sha256(bytes)) ==
                        entry.at("sha256").get<std::string>(),
                "asset integrity check failed");
        total += bytes.size();
        if (is_music) music_bytes = bytes.size();
        require(total - music_bytes <= 64 * 1024 * 1024 &&
                    files.emplace(name, std::move(bytes)).second,
                "duplicate asset or pack exceeds budget");
    }
    Catalogue prepared;
    // One conversion factor covers source model geometry, motion and collider
    // bounds. Positions in the pack are already in final rider-world units.
    if (manifest.contains("source_to_world_scale")) {
        prepared.source_to_world_scale = number(manifest.at("source_to_world_scale"));
        require(prepared.source_to_world_scale >= .025f && prepared.source_to_world_scale <= 1.f,
                "unsupported source-to-world scale");
    }
    const auto &courses = manifest.at("courses");
    require(courses.size() == 16, "MK64 pack requires all sixteen race courses");
    prepared.courses.reserve(courses.size());
    std::set<std::string> ids;
    for (const auto &c : courses) {
        auto &course = prepared.courses.emplace_back();
        course.hazards.source_to_world_scale = prepared.source_to_world_scale;
        course.id = c.at("id").get<std::string>();
        course.name = c.at("name").get<std::string>();
        for (auto &letter : course.name)
            if (letter >= 'a' && letter <= 'z')
                letter -= 'a' - 'A';
        require(identifier(course.id) && ids.insert(course.id).second && !course.name.empty() &&
                    course.name.size() <= 23,
                "invalid/duplicate course identity");
        require(c.at("preview_width") == 128 && c.at("preview_height") == 78,
                "unsupported preview dimensions");
        course.preview = files.at(c.at("preview").get<std::string>());
        require(course.preview.size() == 128 * 78 * 2, "truncated preview");
        const auto route_name = c.at("route").get<std::string>();
        const auto &route_json = files.at(route_name);
        const auto route_metadata = Json::parse(route_json.begin(), route_json.end());
        require(route_metadata.at("binary") == "route.bin", "unsupported route binary path");
        course.records = files.at(
            (std::filesystem::path(route_name).parent_path() / "route.bin").generic_string());
        decode_route(course, route_metadata);
        if (c.contains("boosts")) {
            const auto &bytes = files.at(c.at("boosts").get<std::string>());
            const auto boost = Json::parse(bytes.begin(), bytes.end());
            require(boost.at("format") == "rr64-course-boosts" && boost.at("version") == 1 &&
                        boost.at("course") == course.id && boost.at("pads").size() <= 32,
                    "invalid boost metadata");
            const auto vec = [](const Json &j) {
                require(j.is_array() && j.size() == 3, "invalid boost vector");
                return course_boost::Vec{number(j[0]), number(j[1]), number(j[2])};
            };
            for (const auto &entry : boost.at("pads")) {
                course_boost::Pad pad;
                pad.id = entry.at("id").get<unsigned>();
                pad.direction = vec(entry.at("direction"));
                pad.lip = vec(entry.at("lip"));
                pad.slope = number(entry.at("slope"));
                pad.minimum_speed = number(entry.at("minimum_speed"));
                pad.length = number(entry.at("length"));
                require(entry.at("triangles").size() <= 64, "too many boost triangles");
                for (const auto &triangle : entry.at("triangles")) {
                    require(triangle.is_array() && triangle.size() == 3,
                            "invalid boost triangle");
                    pad.triangles.push_back({vec(triangle[0]), vec(triangle[1]), vec(triangle[2])});
                }
                course.boosts.pads.push_back(std::move(pad));
            }
            course_boost::validate(course.boosts);
        }
        if (c.contains("sky")) {
            decode_sky(course, c.at("sky"));
            course.sky.identity = static_cast<unsigned>(prepared.courses.size());
        }
        course.route.delayed_fall_recovery = true;
        course.route.fall_floor =
            course.heights.empty()
                ? -8.f
                : std::min(0.f, *std::min_element(course.heights.begin(), course.heights.end())) -
                      8.f;
        if (c.contains("item_boxes")) {
            require(manifest.contains("item_box_asset"), "item definitions require render asset");
            const auto &item_json = files.at(c.at("item_boxes").get<std::string>());
            decode_items(course, Json::parse(item_json.begin(), item_json.end()),
                         prepared.source_to_world_scale);
        }
        // Keep IDs only while validating this course's optional sound metadata;
        // no duplicate contact index is retained in the installed catalogue.
        std::vector<std::uint32_t> audio_surface_ids;
        for (const auto &key : {"walls", "surfaces"})
            if (c.contains(key)) {
                const auto &bytes = files.at(c.at(key).get<std::string>());
                decode_walls(course, Json::parse(bytes.begin(), bytes.end()),
                             std::string_view(key) == "surfaces",
                             c.contains("audio_wood_surfaces") ? &audio_surface_ids : nullptr);
            }
        if (c.contains("hazards")) {
            require(manifest.contains("hazard_asset"), "hazard definitions require render asset");
            const auto &bytes = files.at(c.at("hazards").get<std::string>());
            decode_hazards(course, Json::parse(bytes.begin(), bytes.end()));
        }
        if (c.contains("audio_wood_surfaces")) {
            const auto &wood = c.at("audio_wood_surfaces");
            require(wood.is_array() && wood.size() <= 4096 && course.surfaces,
                    "invalid wooden surface audio metadata");
            std::sort(audio_surface_ids.begin(), audio_surface_ids.end());
            for (const auto &id : wood) {
                require(id.is_number_unsigned(), "invalid wooden surface triangle id");
                const auto value = id.get<std::uint64_t>();
                require(value <= std::numeric_limits<std::uint32_t>::max(),
                        "wooden surface triangle id exceeds uint32 range");
                const auto triangle = static_cast<std::uint32_t>(value);
                require(std::binary_search(audio_surface_ids.begin(), audio_surface_ids.end(),
                                           triangle),
                        "wooden surface triangle id missing from surface mesh");
                course.wood_surfaces.push_back(triangle);
            }
            std::sort(course.wood_surfaces.begin(), course.wood_surfaces.end());
            require(std::adjacent_find(course.wood_surfaces.begin(), course.wood_surfaces.end()) ==
                        course.wood_surfaces.end(), "duplicate wooden surface triangle id");
        }
        for (const auto &item : course.items)
            if (item.parent_hazard != ~0u)
                require(item.parent_hazard < course.hazards.definitions.size() &&
                            course.hazards.definitions[item.parent_hazard].kind ==
                                course_hazards::Kind::Balloon,
                        "moving item requires balloon parent");
        validate_route(course.route); // Includes the derived missing-contact floor.
    }
    auto combined = compose(rom, manifest, files, prepared);
    {
        std::string error;
        const bool art_ready = mk64_items::install_render_asset(
            files.at(manifest.at("mk64_items_asset").get<std::string>()), error);
        require(art_ready, error.c_str());
        const bool audio_ready = mk64_items::install_audio_bank(
            files.at(manifest.at("mk64_items_audio_asset").get<std::string>()), error);
        require(audio_ready, error.c_str());
    }
    if (manifest.contains("course_audio_asset"))
        course_audio::load_bank(files.at(manifest.at("course_audio_asset").get<std::string>()));
    if (manifest.contains("course_music_asset"))
        course_music::load_bank(files.at(manifest.at("course_music_asset").get<std::string>()));
    if (manifest.contains("item_box_asset")) {
        std::string error;
        require(course_items::install_render_asset(
                    files.at(manifest.at("item_box_asset").get<std::string>()), error),
                error.empty() ? "invalid item box render asset" : error.c_str());
    }
    if (manifest.contains("hazard_asset")) {
        std::string error;
        require(course_hazards::install_hazard_asset(
                    files.at(manifest.at("hazard_asset").get<std::string>()), error),
                error.empty() ? "invalid hazard render asset" : error.c_str());
        for (const auto &course : prepared.courses)
            for (const auto &d : course.hazards.definitions) {
                course_hazards::HazardModelBounds bounds;
                require(course_hazards::hazard_model_bounds(d.model, bounds),
                        "hazard model does not exist");
                for (unsigned frame = 1; frame < d.animation_frames; ++frame) {
                    course_hazards::HazardModelBounds frame_bounds;
                    require(course_hazards::hazard_model_bounds(d.model + frame, frame_bounds),
                            "actor animation frame does not exist");
                    require(frame_bounds.source_scale == bounds.source_scale,
                            "actor animation changes authored scale");
                }
                // Metadata cannot invent a displaced, invisible collider. Bounds are
                // derived from the same source model used by the renderer.
                const std::array<unsigned, 3> axes{0, 2, 1};
                // New sprite actors use the donor's collision sphere; the flat
                // visible quad is intentionally not a physical box. Decorative
                // effects cannot become collision through animation state.
                const bool original_box = d.kind <= course_hazards::Kind::Traffic;
                if (d.collision_model != ~0u)
                    require(course_hazards::hazard_model_bounds(d.collision_model, bounds),
                            "articulated collision model does not exist");
                for (unsigned i = 0; original_box && i < 3; ++i) {
                    const auto a = axes[i];
                    const float k = prepared.source_to_world_scale * bounds.source_scale;
                    const float half = (bounds.maximum[a] - bounds.minimum[a]) * .5f * k;
                    const float center =
                        (bounds.maximum[a] + bounds.minimum[a]) * .5f * k * (i == 1 ? -1.f : 1.f);
                    require(std::abs(d.half_extent[i] - half) < .001f &&
                                std::abs(d.offset[i] - center) < .001f,
                            "hazard collider/model mismatch");
                }
                if (d.kind == course_hazards::Kind::Rock)
                    require(bool(course.surfaces), "falling rocks require authored surfaces");
            }
    }
    prepared.digest = race_pack::sha256(metadata);
    catalogue = std::move(prepared);
    for (auto &c : catalogue.courses)
        catalogue.menu.push_back({"mk64", "MK64", c.id, c.name, c.preview, 128, 78, 0, 0});
    recomp::set_rom_contents(std::move(combined));
    installed_flag.store(true, std::memory_order_release);
    if (rr64::diagnostics::routine_enabled()) {
        std::fprintf(stderr,
                     "[race-pack] Loaded catalogue: %zu courses, %u native textures, "
                     "source_scale=%.6g, identity=%s\n",
                     catalogue.courses.size(), catalogue.texture_count, catalogue.source_to_world_scale,
                     race_pack::hex_digest(catalogue.digest).c_str());
    }
}
} // namespace

void initialize() {
    reset_assets();
    try {
        const auto configured = race_pack_mod::begin_session();
        if (!configured.empty())
            load_pack(configured);
    } catch (const std::exception &error) {
        // A rejected optional pack must leave no partial assets or locked Mods controls.
        reset_assets();
        race_pack_mod::abort_session();
        throw std::runtime_error(std::string("Unable to load the MK64 Race Pack.\n\n") +
            error.what() + "\n\nOpen Settings > Mods > MK64 Race Pack and reimport your "
            "Mario Kart 64 ROM, or disable this pack before starting the game.");
    }
}
} // namespace rr64::experimental_course

namespace rr64::course_items {
std::span<const ItemBoxDefinition> definitions() noexcept {
    const auto i = experimental_course::loaded.load(std::memory_order_acquire);
    return experimental_course::installed() && i >= 0
               ? std::span<const ItemBoxDefinition>(
                     experimental_course::catalogue.courses[unsigned(i)].items)
               : std::span<const ItemBoxDefinition>{};
}
} // namespace rr64::course_items

namespace rr64::course_walls {
const World *world() noexcept {
    const auto i = experimental_course::loaded.load(std::memory_order_acquire);
    return experimental_course::installed() && i >= 0
               ? experimental_course::catalogue.courses[unsigned(i)].walls.get()
               : nullptr;
}
const World *surface_world() noexcept {
    const auto i = experimental_course::loaded.load(std::memory_order_acquire);
    return experimental_course::installed() && i >= 0
               ? experimental_course::catalogue.courses[unsigned(i)].surfaces.get()
               : nullptr;
}
} // namespace rr64::course_walls
namespace rr64::course_sky {
const Definition *current() noexcept {
    const auto i = experimental_course::loaded.load(std::memory_order_acquire);
    return experimental_course::installed() && i >= 0
               ? &experimental_course::catalogue.courses[unsigned(i)].sky
               : nullptr;
}
} // namespace rr64::course_sky
namespace rr64::course_hazards {
const Data *data() noexcept {
    const auto i = experimental_course::loaded.load(std::memory_order_acquire);
    return experimental_course::installed() && i >= 0
               ? &experimental_course::catalogue.courses[unsigned(i)].hazards
               : nullptr;
}
} // namespace rr64::course_hazards

namespace rr64::course_boost {
const Data *data() noexcept {
    const auto i = experimental_course::loaded.load(std::memory_order_acquire);
    return experimental_course::installed() && i >= 0
               ? &experimental_course::catalogue.courses[unsigned(i)].boosts
               : nullptr;
}
} // namespace rr64::course_boost

namespace rr64::race_pack {
std::span<const CourseMenuEntry> menu_courses() noexcept {
    return race_pack_mod::enabled_for_session() ? experimental_course::catalogue.menu
                                              : std::span<const CourseMenuEntry>{};
}
std::optional<std::size_t> selected_course() noexcept {
    const auto i = experimental_course::selected.load(std::memory_order_acquire);
    return i < 0 ? std::nullopt : std::optional<std::size_t>(i);
}
bool select_course(std::size_t index) {
    if (!race_pack_mod::enabled_for_session() || !experimental_course::installed() ||
        index >= experimental_course::catalogue.courses.size())
        return false;
    experimental_course::selected.store(static_cast<int>(index), std::memory_order_release);
    return true;
}
void select_stock() noexcept { experimental_course::selected.store(-1, std::memory_order_release); }
} // namespace rr64::race_pack

extern "C" unsigned rr64_experimental_course_rom_base(void) {
    return 0xb0000000u + rr64::experimental_course::terrain_rom_offset();
}

namespace rr64::race_pack {
Identity selected_identity() noexcept {
    Identity id{};
    const auto selected = selected_course();
    if (!selected)
        return id;
    id.version = 1;
    id.pack = {'m', 'k', '6', '4'};
    const auto &c = experimental_course::catalogue.courses[*selected];
    std::copy(c.id.begin(), c.id.end(), id.course.begin());
    id.digest = experimental_course::catalogue.digest;
    return id;
}
Compatibility compatibility(const Identity &id) noexcept {
    if (!valid(id))
        return Compatibility::Different;
    if (id.version == 0) return Compatibility::Ready;
    if (!race_pack_mod::enabled_for_session()) return Compatibility::Disabled;
    if (!experimental_course::installed()) return Compatibility::Missing;
    if (id.digest != experimental_course::catalogue.digest) return Compatibility::Different;
    const std::string_view name(id.course.data());
    for (const auto &course : experimental_course::catalogue.courses)
        if (course.id == name) return Compatibility::Ready;
    return Compatibility::Different;
}
bool apply_identity(const Identity &id) noexcept {
    if (compatibility(id) != Compatibility::Ready) return false;
    if (id.version == 0) {
        select_stock();
        return true;
    }
    const std::string_view name(id.course.data());
    for (std::size_t i = 0; i < experimental_course::catalogue.courses.size(); ++i)
        if (experimental_course::catalogue.courses[i].id == name) return select_course(i);
    return false;
}
} // namespace rr64::race_pack
