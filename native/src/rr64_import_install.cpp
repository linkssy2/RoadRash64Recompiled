#include "rr64_import_install.hpp"
#include "rr64_race_pack_digest.hpp"
#include "json/json.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>

namespace rr64::mk64_import {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

// RR64-MAC: parse byte buffers as char data. nlohmann json 3.9 reads
// unsigned-char input through std::char_traits<unsigned char>, which libc++
// 19 and newer (Xcode 16.3+, and the bundled libc++) no longer provide. The
// parser sees exactly the same bytes.
template <typename Buffer>
Json parse_json(const Buffer& bytes) {
    const char* begin = reinterpret_cast<const char*>(bytes.data());
    return Json::parse(begin, begin + bytes.size());
}
void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}
void check_cancel(std::stop_token stop) {
    require(!stop.stop_requested(), "Import cancelled. The previous pack is unchanged.");
}
std::vector<std::uint8_t> read(const fs::path& path, std::uintmax_t maximum) {
    require(fs::is_regular_file(fs::symlink_status(path)), "The converted pack contains an invalid file.");
    const auto size = fs::file_size(path);
    require(size > 0 && size <= maximum, "A converted file exceeds the supported size.");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    require(bool(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) && input.peek() == EOF,
            "A converted file could not be read completely.");
    return bytes;
}
fs::path relative_path(const std::string& name) {
    // The converter uses portable lowercase ASCII names. Reject both platforms'
    // separators/drive syntax and dot components, regardless of the host OS.
    require(!name.empty() && name.size() < 240 && name.front() != '/' && name.back() != '/',
            "The converted pack contains an invalid path.");
    for (unsigned char c : name)
        require((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-' || c == '.' || c == '/', "The converted pack contains an invalid path.");
    fs::path result(name);
    require(!result.is_absolute() && !result.has_root_path(), "An absolute asset path is not allowed.");
    for (const auto& part : result) {
        require(part != "." && part != ".." && !part.empty(), "A parent asset path is not allowed.");
        const auto component = part.generic_string();
        require(component.back() != '.', "A trailing dot is not allowed in an asset path.");
        const auto base = component.substr(0, component.find('.'));
        const bool numbered_device = base.size() == 4 && base[3] >= '1' && base[3] <= '9' &&
            (base.starts_with("com") || base.starts_with("lpt"));
        require(base != "con" && base != "prn" && base != "aux" && base != "nul" && !numbered_device,
                "A reserved device name is not allowed in an asset path.");
    }
    require(result.generic_string() == name, "The converted pack contains an ambiguous path.");
    return result;
}
void ordinary_ancestors(const fs::path& root, const fs::path& relative) {
    auto cursor = root;
    require(fs::is_directory(fs::symlink_status(cursor)), "The staging directory is not an ordinary directory.");
    for (const auto& part : relative.parent_path()) {
        cursor /= part;
        require(fs::is_directory(fs::symlink_status(cursor)), "Linked asset directories are not allowed.");
    }
    // Also resolves Windows directory junctions, which need not be classified
    // as symbolic links by every standard-library implementation.
    const auto resolved = fs::canonical(root / relative);
    const auto bounded = resolved.lexically_relative(fs::canonical(root));
    require(!bounded.empty() && *bounded.begin() != ".." && !bounded.has_root_path(),
            "A converted file points outside the staging directory.");
}
}

std::string validate_pack(const fs::path& pack, std::stop_token stop) {
    check_cancel(stop);
    const auto metadata = read(pack / "catalogue.json", 2u * 1024 * 1024);
    const auto manifest = parse_json(metadata);
    require(manifest.at("format") == "rr64-race-pack-catalogue" && manifest.at("version") == 1 &&
            manifest.at("group_id") == "mk64" && manifest.at("group_name") == "MK64",
            "The converter produced an unsupported catalogue.");
    require(manifest.at("base_rom_sha256") == "74e49e863484b5d17dcbe3891b99f2cafe3cf7511aa1dc5427022f699301db73",
            "The converted pack targets a different Road Rash 64 ROM.");
    const std::set<std::string> expected_courses{
        "mario_raceway", "choco_mountain", "bowsers_castle", "banshee_boardwalk",
        "yoshi_valley", "frappe_snowland", "koopa_troopa_beach", "royal_raceway",
        "luigi_raceway", "moo_moo_farm", "toads_turnpike", "kalimari_desert",
        "sherbet_land", "rainbow_road", "wario_stadium", "dks_jungle_parkway"};
    const auto& courses = manifest.at("courses");
    require(courses.is_array() && courses.size() == 16, "The import did not produce all sixteen courses.");
    std::set<std::string> course_ids;
    for (const auto& course : courses)
        require(course_ids.insert(course.at("id").get<std::string>()).second,
                "The converted pack contains duplicate courses.");
    require(course_ids == expected_courses, "The converted pack has missing or unexpected courses.");

    const auto music = manifest.value("course_music_asset", std::string{});
    require(!music.empty() && manifest.contains("course_audio_asset"), "The import is missing course sound or music.");
    require(manifest.value("mk64_items_version", 0u) == 1u &&
                manifest.contains("mk64_items_asset") && manifest.contains("mk64_items_audio_asset"),
            "This converter is missing the full MK64 items. Update the converter and reimport your ROM.");
    const auto& records = manifest.at("files");
    require(records.is_array() && !records.empty() && records.size() <= 4096, "The converted asset list is invalid.");
    std::set<std::string> files;
    std::uintmax_t terrain_bytes = 0;
    for (const auto& entry : records) {
        check_cancel(stop);
        const auto name = entry.at("file").get<std::string>();
        const auto relative = relative_path(name);
        require(name != "catalogue.json" && files.insert(name).second, "The converted pack contains duplicate files.");
        ordinary_ancestors(pack, relative);
        const bool song = name == music;
        const auto bytes = read(pack / relative, (song ? 128u : 8u) * 1024 * 1024);
        require(entry.at("bytes").is_number_unsigned() && bytes.size() == entry.at("bytes").get<std::uint64_t>(),
                "A converted file has the wrong size.");
        require(race_pack::hex_digest(race_pack::sha256(bytes)) == entry.at("sha256").get<std::string>(),
                "A converted file failed its integrity check.");
        if (!song) terrain_bytes += bytes.size();
        require(terrain_bytes <= 64u * 1024 * 1024, "The converted scenery exceeds the supported size.");
    }
    require(files.contains(music) && files.contains(manifest.at("course_audio_asset").get<std::string>()),
            "The imported sound banks are missing.");
    require(files.contains(manifest.at("mk64_items_asset").get<std::string>()) &&
                files.contains(manifest.at("mk64_items_audio_asset").get<std::string>()),
            "The imported item art or sounds are missing.");
    for (const auto& course : courses) {
        for (const char* field : {"route", "preview", "item_boxes", "walls", "surfaces", "hazards"})
            require(files.contains(course.at(field).get<std::string>()), "A course is missing a required file.");
    }
    files.insert("catalogue.json");
    for (const auto& entry : fs::recursive_directory_iterator(pack)) {
        check_cancel(stop);
        const auto status = entry.symlink_status();
        require(fs::is_directory(status) || fs::is_regular_file(status), "The converted pack contains a linked or special file.");
        if (fs::is_regular_file(status))
            require(files.erase(entry.path().lexically_relative(pack).generic_string()) == 1,
                    "The converted pack contains an unexpected file.");
    }
    require(files.empty(), "The converted pack is incomplete.");
    return race_pack::hex_digest(race_pack::sha256(metadata));
}

void install_pack(const fs::path& pack, const fs::path& destination, const fs::path& backup) {
    require(!fs::exists(backup), "An import backup already exists; the previous pack was not changed.");
    require(fs::is_directory(fs::symlink_status(pack)), "The converted pack is missing.");
    const bool previous = fs::exists(destination);
    if (previous) {
        require(fs::is_directory(fs::symlink_status(destination)), "The existing pack is not an ordinary directory.");
        fs::rename(destination, backup);
    }
    try {
        fs::rename(pack, destination);
    } catch (...) {
        if (previous) {
            std::error_code error;
            fs::rename(backup, destination, error);
            if (error)
                throw std::runtime_error("Installation was interrupted. The previous pack is preserved in the import folder as previous-pack; restore it before playing.");
        }
        throw std::runtime_error("Could not install the converted pack. The previous pack is unchanged.");
    }
}
}
