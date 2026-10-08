#include "rr64_rider_skin_mod_ui.hpp"
#include "rr64_diagnostic_options.hpp"
#include "rr64_rider_skins.hpp"
#include "rr64_netplay.hpp"
#include "librecomp/mods.hpp"
#include "ultramodern/ultramodern.hpp"

#include <cstdio>
#include <exception>
#include <string_view>
#include <unordered_set>

namespace rr64::rider_skin_mod_ui {
namespace {
bool can_change() {
    return rider_skins::can_change() && !ultramodern::is_game_started() &&
           !netplay::get_physics_rules().active;
}
// Inspect uncompressed entry sizes before read_file allocates. Texture filenames
// are derived from bounded ASCII identities; paths are never accepted.
bool read_bounded(const recomp::mods::ModFileHandle &file, const char *filename, std::size_t limit,
                  std::vector<char> &result) {
    const auto *zip = dynamic_cast<const recomp::mods::ZipModFileHandle *>(&file);
    if (!zip || !zip->archive)
        return false;
    // Duplicate ZIP names must not make selection depend on reader ordering.
    const auto files = mz_zip_reader_get_num_files(zip->archive.get());
    if (files > 256)
        return false;
    unsigned matches = 0;
    for (mz_uint32 file_index = 0; file_index < files; ++file_index) {
        mz_zip_archive_file_stat candidate{};
        if (!mz_zip_reader_file_stat(zip->archive.get(), file_index, &candidate))
            return false;
        if (std::string_view(candidate.m_filename) == filename)
            ++matches;
    }
    if (matches != 1)
        return false;
    mz_uint32 index = 0;
    if (!mz_zip_reader_locate_file_v2(zip->archive.get(), filename, nullptr,
                                      MZ_ZIP_FLAG_CASE_SENSITIVE, &index))
        return false;
    mz_zip_archive_file_stat entry{};
    if (!mz_zip_reader_file_stat(zip->archive.get(), index, &entry) || entry.m_is_directory ||
        entry.m_is_encrypted || entry.m_uncomp_size == 0 || entry.m_uncomp_size > limit)
        return false;
    bool exists = false;
    result = file.read_file(filename, exists);
    return exists && result.size() == entry.m_uncomp_size;
}
void fail(const std::string &reason) {
    rider_skins::unavailable(reason + " Using the original riders.");
    std::fprintf(stderr, "[RR64-RIDER-MOD] %s\n", reason.c_str());
}
void enabled(recomp::mods::ModContext &, const recomp::mods::ModHandle &mod) {
    if (mod.manifest.file_handle)
        load_archive(*mod.manifest.file_handle, mod.manifest.mod_id);
    else if (mod.manifest.mod_id == rider_skins::mod_id)
        fail("Rider archive is unavailable.");
}
void disabled(recomp::mods::ModContext &, const recomp::mods::ModHandle &mod) {
    if (mod.manifest.mod_id == rider_skins::mod_id)
        rider_skins::unavailable("Disabled. Enable before starting the game. Local play only.");
}
} // namespace
bool load_archive(const recomp::mods::ModFileHandle &file, std::string_view id) {
    // This catalog has one stable mod identity. Other mods cannot
    // replace its payload simply by including the same content marker.
    if (id != rider_skins::mod_id)
        return false;
    try {
        std::vector<char> marker;
        if (!read_bounded(file, "rr64-rider-skins.json", 32768, marker)) {
            fail("Rider descriptor is missing, oversized or unreadable.");
            return false;
        }
        bool duplicate_key = false;
        std::vector<std::unordered_set<std::string>> object_keys;
        const auto callback = [&](int, nlohmann::json::parse_event_t event,
                                  nlohmann::json &parsed) {
            // JSON's usual last-key-wins behavior would make repeated donor
            // metadata ambiguous. Reject duplicate keys at every object level.
            if (event == nlohmann::json::parse_event_t::object_start)
                object_keys.emplace_back();
            else if (event == nlohmann::json::parse_event_t::key && !object_keys.empty())
                duplicate_key |= !object_keys.back().insert(parsed.get<std::string>()).second;
            else if (event == nlohmann::json::parse_event_t::object_end && !object_keys.empty())
                object_keys.pop_back();
            return true;
        };
        const auto json = nlohmann::json::parse(marker.begin(), marker.end(), callback, false);
        if (duplicate_key || !json.is_object() || !json.contains("format") ||
            !json["format"].is_string() || json["format"] != "rr64-rider-skins" ||
            !json.contains("version") || !json["version"].is_number_unsigned() ||
            json["version"] != 1 || !json.contains("characters") ||
            !json["characters"].is_array() || json["characters"].empty() ||
            json["characters"].size() > rider_skins::maximum_characters) {
            fail("Rider descriptor format is unsupported.");
            return false;
        }
        std::vector<rider_skins::Appearance> assets;
        assets.reserve(json["characters"].size());
        for (const auto &entry : json["characters"]) {
            if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string() ||
                !entry.contains("name") || !entry["name"].is_string() || !entry.contains("donor") ||
                !entry["donor"].is_number_unsigned() || !entry.contains("textures") ||
                !entry["textures"].is_array() || entry["textures"].size() != 4) {
                fail("Character catalog entry is incomplete.");
                return false;
            }
            const auto donor = entry["donor"].get<std::uint64_t>();
            if (donor != 0 && donor != 10) {
                fail("Character native donor is unsupported.");
                return false;
            }
            rider_skins::Appearance asset{entry["id"].get<std::string>(),
                                          entry["name"].get<std::string>(),
                                          unsigned(donor),
                                          {}};
            for (const auto &[key, unused] : entry.items()) {
                (void)unused;
                if (key != "id" && key != "name" && key != "donor" && key != "textures" && key != "turtle_shell" && key != "dual_head") {
                    fail("Character catalog entry contains unsupported metadata.");
                    return false;
                }
            }
            if (entry.contains("turtle_shell")) {
                if (!entry["turtle_shell"].is_boolean() || donor != 0) {
                    fail("Shell attachment requires a male donor and a boolean flag.");
                    return false;
                }
                asset.turtle_shell = entry["turtle_shell"].get<bool>();
            }
            if (entry.contains("dual_head")) {
                if (!entry["dual_head"].is_boolean() || donor != 0) {
                    fail("Asymmetric head requires a male donor and a boolean flag.");
                    return false;
                }
                asset.dual_head = entry["dual_head"].get<bool>();
            }
            if (asset.id.empty() || asset.id.size() > 32 ||
                asset.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") !=
                    std::string::npos ||
                asset.name.empty() || asset.name.size() > 32 ||
                asset.name.find_first_not_of(
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_") !=
                    std::string::npos) {
                fail("Character catalog identity, filename or name is invalid.");
                return false;
            }
            for (const auto &prior : assets) {
                if (prior.id == asset.id || prior.name == asset.name) {
                    fail("Character catalog repeats an identity or display name.");
                    return false;
                }
            }
            constexpr const char *slots[] = {"head", "torso", "body", "far"};
            for (unsigned slot = 0; slot < 4; ++slot) {
                const std::string filename = asset.id + "-" + slots[slot] + ".ci8";
                if (!entry["textures"][slot].is_string() || entry["textures"][slot] != filename) {
                    fail("Character texture filename is invalid.");
                    return false;
                }
                auto &texture = asset.textures[slot];
                texture.width = slot == 3 ? 32 : 64;
                texture.height = slot == 3 ? 16 : 32;
                std::vector<char> bytes;
                const unsigned required = texture.width * texture.height + 512;
                if (!read_bounded(file, filename.c_str(), required, bytes) ||
                    bytes.size() != required) {
                    fail("Character texture is missing, oversized or unreadable.");
                    return false;
                }
                texture.bytes.assign(bytes.begin(), bytes.end());
            }
            assets.push_back(std::move(asset));
        }
        if (!rider_skins::install_catalog(assets)) {
            std::fprintf(stderr, "[RR64-RIDER-MOD] %s\n", rider_skins::status().c_str());
            return false;
        }
        if (rr64::diagnostics::routine_enabled()) {
            std::fprintf(stderr, "[RR64-RIDER-SKINS] Loaded More Characters (%zu native skins).\n",
                         assets.size());
        }
        return true;
    } catch (const std::exception &) {
        fail("Rider archive could not be loaded.");
        return false;
    }
}
void install() {
    static bool registered = false;
    if (registered)
        return;
    recomp::mods::register_mod_content_type(
        {"rr64-rider-skins.json", false, enabled, disabled, nullptr});
    recomp::mods::register_mod_toggle_policy(rider_skins::mod_id,
                                             {can_change, [](bool) { return can_change(); },
                                              [] { return rider_skins::status(); }, can_change});
    registered = true;
}
} // namespace rr64::rider_skin_mod_ui
