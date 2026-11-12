#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#include "recomp.h"
#include "helpers.hpp"
#include "librecomp/files.hpp"

namespace {

// RR64_FIX_VIRTUAL_CONTROLLER_PAK
constexpr s32 kPfsOk = 0;
constexpr s32 kPfsErrNoPack = 1;
constexpr s32 kPfsErrInvalid = 5;
constexpr s32 kPfsDataFull = 7;
constexpr s32 kPfsDirFull = 8;
constexpr s32 kPfsErrExist = 9;
constexpr s32 kPfsErrDevice = 11;
constexpr u8 kPfsRead = 0;
constexpr size_t kDirectoryEntries = 16;
constexpr size_t kUsableBytes = 123 * 256;
constexpr size_t kGameNameBytes = 16;
constexpr size_t kExtensionBytes = 4;
constexpr std::array<char, 8> kDiskMagic{'R', 'R', '6', '4', 'P', 'F', 'S', '1'};
constexpr uint32_t kDiskVersion = 1;

struct VirtualPakFile {
    bool used = false;
    u16 company_code = 0;
    u32 game_code = 0;
    std::array<u8, kGameNameBytes> game_name{};
    std::array<u8, kExtensionBytes> extension{};
    std::vector<u8> data{};
};

std::array<VirtualPakFile, kDirectoryEntries> g_files{};
std::mutex g_pak_mutex;
bool g_loaded = false;

std::filesystem::path pak_path() {
    std::filesystem::path path = ultramodern::get_save_file_path();
    if (path.empty()) {
        path = std::filesystem::current_path() / "saves" / "rr64.n64.us.1.0.bin";
    }
    path.replace_extension(".mpk");
    return path;
}

template <typename T>
bool read_value(std::istream& stream, T& value) {
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    return stream.good();
}

template <typename T>
bool write_value(std::ostream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return stream.good();
}

bool load_stream(std::istream& stream, std::array<VirtualPakFile, kDirectoryEntries>& files) {
    std::array<char, 8> magic{};
    uint32_t version = 0;
    uint32_t count = 0;
    stream.read(magic.data(), magic.size());
    if (!stream.good() || magic != kDiskMagic || !read_value(stream, version) ||
        !read_value(stream, count) || version != kDiskVersion || count > kDirectoryEntries) {
        return false;
    }

    size_t total_bytes = 0;
    for (uint32_t entry = 0; entry < count; ++entry) {
        uint32_t slot = 0;
        uint32_t data_size = 0;
        VirtualPakFile file{};
        if (!read_value(stream, slot) || !read_value(stream, file.company_code) ||
            !read_value(stream, file.game_code) || !read_value(stream, data_size) ||
            slot >= kDirectoryEntries || files[slot].used || data_size > kUsableBytes) {
            return false;
        }

        stream.read(reinterpret_cast<char*>(file.game_name.data()), file.game_name.size());
        stream.read(reinterpret_cast<char*>(file.extension.data()), file.extension.size());
        file.data.resize(data_size);
        if (data_size != 0) {
            stream.read(reinterpret_cast<char*>(file.data.data()), file.data.size());
        }
        total_bytes += file.data.size();
        if (!stream.good() || total_bytes > kUsableBytes) {
            return false;
        }

        file.used = true;
        files[slot] = std::move(file);
    }
    return true;
}

void load_once() {
    if (g_loaded) {
        return;
    }
    g_loaded = true;

    const std::filesystem::path path = pak_path();
    std::array<VirtualPakFile, kDirectoryEntries> loaded_files{};
    std::ifstream input = recomp::open_input_file_with_backup(path, std::ios_base::binary);
    bool loaded = input.good() && load_stream(input, loaded_files);
    if (!loaded) {
        input.close();
        loaded_files = {};
        std::ifstream backup = recomp::open_input_backup_file(path, std::ios_base::binary);
        loaded = backup.good() && load_stream(backup, loaded_files);
    }

    if (loaded) {
        g_files = std::move(loaded_files);
        std::fprintf(stderr, "[RR64-PFS] Loaded virtual Controller Pak: %s\n", path.string().c_str());
    } else {
        std::fprintf(stderr, "[RR64-PFS] Starting with an empty virtual Controller Pak: %s\n", path.string().c_str());
    }
}

bool save_files() {
    const std::filesystem::path path = pak_path();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }

    std::ofstream output = recomp::open_output_file_with_backup(path, std::ios_base::binary);
    if (!output.good()) {
        return false;
    }

    uint32_t count = 0;
    for (const VirtualPakFile& file : g_files) {
        count += file.used ? 1u : 0u;
    }

    output.write(kDiskMagic.data(), kDiskMagic.size());
    if (!write_value(output, kDiskVersion) || !write_value(output, count)) {
        return false;
    }

    for (uint32_t slot = 0; slot < g_files.size(); ++slot) {
        const VirtualPakFile& file = g_files[slot];
        if (!file.used) {
            continue;
        }

        const uint32_t data_size = static_cast<uint32_t>(file.data.size());
        if (!write_value(output, slot) || !write_value(output, file.company_code) ||
            !write_value(output, file.game_code) || !write_value(output, data_size)) {
            return false;
        }
        output.write(reinterpret_cast<const char*>(file.game_name.data()), file.game_name.size());
        output.write(reinterpret_cast<const char*>(file.extension.data()), file.extension.size());
        if (!file.data.empty()) {
            output.write(reinterpret_cast<const char*>(file.data.data()), file.data.size());
        }
        if (!output.good()) {
            return false;
        }
    }

    output.close();
    return recomp::finalize_output_file_with_backup(path);
}

gpr stack_pointer_arg(uint8_t* rdram, recomp_context* ctx, int offset) {
    return static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(MEM_W(offset, ctx->r29))));
}

template <size_t Size>
std::array<u8, Size> read_guest_bytes(uint8_t* rdram, gpr address) {
    std::array<u8, Size> result{};
    if (address == 0) {
        return result;
    }
    for (size_t index = 0; index < Size; ++index) {
        result[index] = MEM_BU(index, address);
    }
    return result;
}

bool valid_pfs(uint8_t* rdram, PTR(OSPfs) pfs) {
    if (pfs == 0) {
        return false;
    }
    OSPfs* native_pfs = TO_PTR(OSPfs, pfs);
    return native_pfs->channel == 0;
}

size_t used_bytes() {
    size_t result = 0;
    for (const VirtualPakFile& file : g_files) {
        if (file.used) {
            result += file.data.size();
        }
    }
    return result;
}

int find_file(u16 company_code, u32 game_code,
              const std::array<u8, kGameNameBytes>& game_name,
              const std::array<u8, kExtensionBytes>& extension) {
    for (size_t slot = 0; slot < g_files.size(); ++slot) {
        const VirtualPakFile& file = g_files[slot];
        if (file.used && file.company_code == company_code && file.game_code == game_code &&
            file.game_name == game_name && file.extension == extension) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

} // namespace

extern "C" void osPfsInitPak_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSPfs) pfs = _arg<1, PTR(OSPfs)>(rdram, ctx);
    const s32 channel = _arg<2, s32>(rdram, ctx);
    if (pfs == 0 || channel != 0) {
        _return<s32>(ctx, kPfsErrNoPack);
        return;
    }

    std::lock_guard lock{g_pak_mutex};
    load_once();
    OSPfs* native_pfs = TO_PTR(OSPfs, pfs);
    native_pfs->status = 0;
    native_pfs->channel = channel;
    native_pfs->version = 1;
    native_pfs->dir_size = static_cast<int>(kDirectoryEntries);
    native_pfs->banks = 1;
    _return<s32>(ctx, kPfsOk);
}

extern "C" void osPfsFreeBlocks_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSPfs) pfs = _arg<0, PTR(OSPfs)>(rdram, ctx);
    PTR(s32) bytes_not_used = _arg<1, PTR(s32)>(rdram, ctx);
    std::lock_guard lock{g_pak_mutex};
    load_once();
    if (!valid_pfs(rdram, pfs)) {
        _return<s32>(ctx, kPfsErrNoPack);
        return;
    }
    if (bytes_not_used != 0) {
        MEM_W(0, bytes_not_used) = static_cast<s32>(kUsableBytes - used_bytes());
    }
    _return<s32>(ctx, kPfsOk);
}

extern "C" void osPfsAllocateFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSPfs) pfs = _arg<0, PTR(OSPfs)>(rdram, ctx);
    const u16 company_code = _arg<1, u16>(rdram, ctx);
    const u32 game_code = _arg<2, u32>(rdram, ctx);
    const gpr game_name_address = _arg<3, PTR(u8)>(rdram, ctx);
    const gpr extension_address = stack_pointer_arg(rdram, ctx, 0x10);
    const s32 file_size = MEM_W(0x14, ctx->r29);
    const gpr file_no_address = stack_pointer_arg(rdram, ctx, 0x18);
    const auto game_name = read_guest_bytes<kGameNameBytes>(rdram, game_name_address);
    const auto extension = read_guest_bytes<kExtensionBytes>(rdram, extension_address);

    std::lock_guard lock{g_pak_mutex};
    load_once();
    if (!valid_pfs(rdram, pfs)) {
        _return<s32>(ctx, kPfsErrNoPack);
        return;
    }
    if (file_size <= 0 || static_cast<size_t>(file_size) > kUsableBytes) {
        _return<s32>(ctx, kPfsErrInvalid);
        return;
    }
    if (find_file(company_code, game_code, game_name, extension) >= 0) {
        _return<s32>(ctx, kPfsErrExist);
        return;
    }
    if (used_bytes() + static_cast<size_t>(file_size) > kUsableBytes) {
        _return<s32>(ctx, kPfsDataFull);
        return;
    }

    auto free_slot = std::find_if(g_files.begin(), g_files.end(), [](const VirtualPakFile& file) {
        return !file.used;
    });
    if (free_slot == g_files.end()) {
        _return<s32>(ctx, kPfsDirFull);
        return;
    }

    free_slot->used = true;
    free_slot->company_code = company_code;
    free_slot->game_code = game_code;
    free_slot->game_name = game_name;
    free_slot->extension = extension;
    free_slot->data.assign(static_cast<size_t>(file_size), 0);
    const s32 slot = static_cast<s32>(std::distance(g_files.begin(), free_slot));
    if (file_no_address != 0) {
        MEM_W(0, file_no_address) = slot;
    }
    _return<s32>(ctx, save_files() ? kPfsOk : kPfsErrDevice);
}

extern "C" void osPfsDeleteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSPfs) pfs = _arg<0, PTR(OSPfs)>(rdram, ctx);
    const u16 company_code = _arg<1, u16>(rdram, ctx);
    const u32 game_code = _arg<2, u32>(rdram, ctx);
    const gpr game_name_address = _arg<3, PTR(u8)>(rdram, ctx);
    const gpr extension_address = stack_pointer_arg(rdram, ctx, 0x10);
    const auto game_name = read_guest_bytes<kGameNameBytes>(rdram, game_name_address);
    const auto extension = read_guest_bytes<kExtensionBytes>(rdram, extension_address);

    std::lock_guard lock{g_pak_mutex};
    load_once();
    if (!valid_pfs(rdram, pfs)) {
        _return<s32>(ctx, kPfsErrNoPack);
        return;
    }
    const int slot = find_file(company_code, game_code, game_name, extension);
    if (slot < 0) {
        _return<s32>(ctx, kPfsErrInvalid);
        return;
    }
    g_files[slot] = {};
    _return<s32>(ctx, save_files() ? kPfsOk : kPfsErrDevice);
}

extern "C" void osPfsFileState_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSPfs) pfs = _arg<0, PTR(OSPfs)>(rdram, ctx);
    const s32 file_no = _arg<1, s32>(rdram, ctx);
    const gpr state_address = _arg<2, PTR(u8)>(rdram, ctx);
    std::lock_guard lock{g_pak_mutex};
    load_once();
    if (!valid_pfs(rdram, pfs)) {
        _return<s32>(ctx, kPfsErrNoPack);
        return;
    }
    if (file_no < 0 || static_cast<size_t>(file_no) >= g_files.size() ||
        !g_files[file_no].used || state_address == 0) {
        _return<s32>(ctx, kPfsErrInvalid);
        return;
    }

    const VirtualPakFile& file = g_files[file_no];
    for (size_t offset = 0; offset < 32; ++offset) {
        MEM_B(offset, state_address) = 0;
    }
    MEM_W(0, state_address) = static_cast<s32>(file.data.size());
    MEM_W(4, state_address) = static_cast<s32>(file.game_code);
    MEM_H(8, state_address) = static_cast<s16>(file.company_code);
    for (size_t index = 0; index < file.extension.size(); ++index) {
        MEM_B(10 + index, state_address) = file.extension[index];
    }
    for (size_t index = 0; index < file.game_name.size(); ++index) {
        MEM_B(14 + index, state_address) = file.game_name[index];
    }
    _return<s32>(ctx, kPfsOk);
}

extern "C" void osPfsFindFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSPfs) pfs = _arg<0, PTR(OSPfs)>(rdram, ctx);
    const u16 company_code = _arg<1, u16>(rdram, ctx);
    const u32 game_code = _arg<2, u32>(rdram, ctx);
    const gpr game_name_address = _arg<3, PTR(u8)>(rdram, ctx);
    const gpr extension_address = stack_pointer_arg(rdram, ctx, 0x10);
    const gpr file_no_address = stack_pointer_arg(rdram, ctx, 0x14);
    const auto game_name = read_guest_bytes<kGameNameBytes>(rdram, game_name_address);
    const auto extension = read_guest_bytes<kExtensionBytes>(rdram, extension_address);

    std::lock_guard lock{g_pak_mutex};
    load_once();
    if (!valid_pfs(rdram, pfs)) {
        _return<s32>(ctx, kPfsErrNoPack);
        return;
    }
    const int slot = find_file(company_code, game_code, game_name, extension);
    if (slot < 0) {
        _return<s32>(ctx, kPfsErrInvalid);
        return;
    }
    if (file_no_address != 0) {
        MEM_W(0, file_no_address) = slot;
    }
    _return<s32>(ctx, kPfsOk);
}

extern "C" void osPfsReadWriteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSPfs) pfs = _arg<0, PTR(OSPfs)>(rdram, ctx);
    const s32 file_no = _arg<1, s32>(rdram, ctx);
    const u8 flag = _arg<2, u8>(rdram, ctx);
    const s32 offset = _arg<3, s32>(rdram, ctx);
    const s32 size = MEM_W(0x10, ctx->r29);
    const gpr data_address = stack_pointer_arg(rdram, ctx, 0x14);

    std::lock_guard lock{g_pak_mutex};
    load_once();
    if (!valid_pfs(rdram, pfs)) {
        _return<s32>(ctx, kPfsErrNoPack);
        return;
    }
    if (file_no < 0 || static_cast<size_t>(file_no) >= g_files.size() ||
        !g_files[file_no].used || offset < 0 || size < 0 || data_address == 0) {
        _return<s32>(ctx, kPfsErrInvalid);
        return;
    }

    VirtualPakFile& file = g_files[file_no];
    const size_t first = static_cast<size_t>(offset);
    const size_t byte_count = static_cast<size_t>(size);
    if (first > file.data.size() || byte_count > file.data.size() - first) {
        _return<s32>(ctx, kPfsErrInvalid);
        return;
    }

    if (flag == kPfsRead) {
        for (size_t index = 0; index < byte_count; ++index) {
            MEM_B(index, data_address) = file.data[first + index];
        }
        _return<s32>(ctx, kPfsOk);
        return;
    }

    for (size_t index = 0; index < byte_count; ++index) {
        file.data[first + index] = MEM_BU(index, data_address);
    }
    _return<s32>(ctx, save_files() ? kPfsOk : kPfsErrDevice);
}

extern "C" void osPfsChecker_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSPfs) pfs = _arg<0, PTR(OSPfs)>(rdram, ctx);
    std::lock_guard lock{g_pak_mutex};
    load_once();
    _return<s32>(ctx, valid_pfs(rdram, pfs) ? kPfsOk : kPfsErrNoPack);
}

extern "C" void osPfsNumFiles_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSPfs) pfs = _arg<0, PTR(OSPfs)>(rdram, ctx);
    PTR(s32) max_files = _arg<1, PTR(s32)>(rdram, ctx);
    PTR(s32) files_used = _arg<2, PTR(s32)>(rdram, ctx);
    std::lock_guard lock{g_pak_mutex};
    load_once();
    if (!valid_pfs(rdram, pfs)) {
        _return<s32>(ctx, kPfsErrNoPack);
        return;
    }

    s32 used = 0;
    for (const VirtualPakFile& file : g_files) {
        used += file.used ? 1 : 0;
    }
    if (max_files != 0) {
        MEM_W(0, max_files) = static_cast<s32>(kDirectoryEntries);
    }
    if (files_used != 0) {
        MEM_W(0, files_used) = used;
    }
    _return<s32>(ctx, kPfsOk);
}

extern "C" void osPfsRepairId_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSPfs) pfs = _arg<0, PTR(OSPfs)>(rdram, ctx);
    std::lock_guard lock{g_pak_mutex};
    load_once();
    _return<s32>(ctx, valid_pfs(rdram, pfs) ? kPfsOk : kPfsErrNoPack);
}

