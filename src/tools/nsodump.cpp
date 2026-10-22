// Decompresses ExeFS NSOs into flat images for analysis / static recompilation.
//   nsodump <exefs_dir> <out_dir>
#include "loader/formats.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fmt::print("usage: nsodump <exefs_dir> <out_dir>\n");
        return 1;
    }
    const fs::path in = fs::u8path(argv[1]), out = fs::u8path(argv[2]);
    fs::create_directories(out);
    for (const char* name : {"rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "sdk"}) {
        auto data = common::ReadFile(in / name);
        if (data.empty())
            continue;
        auto img = loader::LoadNso(data);
        if (!img) {
            fmt::print("{}: failed to load\n", name);
            continue;
        }
        common::WriteFile(out / (std::string(name) + ".bin"), img->image);
        fmt::print("{:8} image {:#10x} text {:#x}+{:#x} ro {:#x}+{:#x} data {:#x}+{:#x} bss {:#x} id {}\n", name,
                   img->image.size(), img->text_off, img->text_size, img->ro_off, img->ro_size, img->data_off,
                   img->data_size, img->bss_size, common::ToHex(std::span(img->module_id).first(8)));
    }
}
