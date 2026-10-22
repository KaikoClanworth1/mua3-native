// Command-line installer: decrypts the dumps in Isos/ into the game data folder.
//   nxextract [isos_dir] [keys_dir] [output_dir] [--no-romfs] [--scan]
#include <chrono>
#include <thread>

#include "loader/installer.h"

int main(int argc, char** argv) {
    loader::InstallOptions o;
    o.isos_dir = "Isos";
    o.keys_dir = "keys";
    o.output_dir = "game";
    bool scan_only = false;
    int pos = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--no-romfs")
            o.extract_romfs = false;
        else if (a == "--scan")
            scan_only = true;
        else if (a == "--debug")
            common::SetLogLevel(common::LogLevel::Debug);
        else if (pos == 0)
            o.isos_dir = fs::u8path(a), pos++;
        else if (pos == 1)
            o.keys_dir = fs::u8path(a), pos++;
        else
            o.output_dir = fs::u8path(a), pos++;
    }

    auto summary = loader::ScanDumps(o.isos_dir);
    for (auto& l : summary.lines)
        fmt::print("  {}\n", l);
    if (scan_only)
        return 0;

    loader::InstallProgress prog;
    std::string error;
    bool ok = false, finished = false;
    std::thread worker([&] {
        ok = loader::InstallGame(o, prog, error);
        finished = true;
    });
    std::string last;
    while (!finished) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const u64 t = prog.bytes_total, d = prog.bytes_done;
        const std::string stage = prog.Stage();
        if (t)
            fmt::print("\r{:<40} {:6.2f}% ({:.2f} / {:.2f} GB)   ", stage, 100.0 * d / t, d / 1073741824.0,
                       t / 1073741824.0);
        else if (stage != last)
            fmt::print("\r{:<60}", stage);
        last = stage;
        std::fflush(stdout);
    }
    worker.join();
    fmt::print("\n");
    if (!ok) {
        fmt::print("Install failed: {}\n", error);
        return 1;
    }
    fmt::print("Installed to {}\n", fs::absolute(o.output_dir).string());
    return 0;
}
