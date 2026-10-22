// Translates dumped Maxwell shaders (shader_dump/*.vs.bin|*.ps.bin) to HLSL and compiles them with FXC.
//   shadertool <file.bin> [--print]        translate one shader and print the HLSL
//   shadertool <dir>                       translate + compile every dump, report failures/unimplemented ops
#include <d3dcompiler.h>
#include <map>

#include "gpu/shader/maxwell_isa.h"
#include "gpu/shader/translator.h"

#pragma comment(lib, "d3dcompiler.lib")

using namespace gpu::shader;

static UINT g_flags = D3DCOMPILE_OPTIMIZATION_LEVEL1;
static bool CompileHlsl(const std::string& src, const char* target, std::string& err) {
    ID3DBlob *code = nullptr, *errors = nullptr;
    const HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", target, g_flags, 0, &code,
                                  &errors);
    if (errors) {
        err.assign((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
        errors->Release();
    }
    if (code)
        code->Release();
    return SUCCEEDED(hr);
}

static void Disassemble(const std::vector<u8>& data) {
    for (size_t off = 0x50; off + 8 <= data.size(); off += 8) {
        const u32 pc = u32(off - 0x50);
        if (pc % 32 == 0)
            continue;
        u64 v;
        std::memcpy(&v, &data[off], 8);
        fmt::print("{:06x}: {:016x}  {}\n", pc, v, OpName(Decode(v)));
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fmt::print("usage: shadertool <file.bin|dir> [--print|--disasm]\n");
        return 1;
    }
    if (std::string(argv[1]) == "--hlsl" && argc >= 4) { // compile a raw HLSL file: --hlsl <file> <target>
        const auto d = common::ReadFile(fs::u8path(argv[2]));
        std::string err;
        const bool ok = CompileHlsl(std::string(d.begin(), d.end()), argv[3], err);
        fmt::print("{} {}\n", ok ? "OK" : "FAIL", err.substr(0, 800));
        return ok ? 0 : 1;
    }
    const fs::path in = fs::u8path(argv[1]);
    const bool print = argc > 2 && std::string(argv[2]) == "--print";
    const bool disasm = argc > 2 && std::string(argv[2]) == "--disasm";
    const bool no_compile = argc > 2 && std::string(argv[2]) == "--print-only";
    if (argc > 2 && std::string(argv[2]).rfind("--flags=", 0) == 0)
        g_flags = UINT(std::stoul(std::string(argv[2]).substr(8), nullptr, 0));
    std::vector<fs::path> files;
    if (fs::is_directory(in)) {
        for (auto& e : fs::directory_iterator(in))
            if (e.path().extension() == ".bin")
                files.push_back(e.path());
    } else {
        files.push_back(in);
    }
    std::map<std::string, int> unimpl;
    int ok = 0, fail = 0;
    for (auto& f : files) {
        auto data = common::ReadFile(f);
        const bool vs = f.string().find(".vs.") != std::string::npos;
        if (disasm) {
            Disassemble(data);
            continue;
        }
        const bool cs = f.filename().string().rfind("compute_", 0) == 0;
        const Stage stage = cs ? Stage::Compute : vs ? Stage::Vertex : Stage::Fragment;
        TranslateOptions copt;
        if (cs) { // shape is not in the program; good enough for inspection
            copt.block[0] = 64;
            copt.shared_bytes = 0x400;
        }
        auto r = Translate(stage, data.data(), data.size(), copt);
        if (r.uses_loop) { // like the game: loop shaders sample with explicit LOD 0
            TranslateOptions opt = copt;
            opt.force_lod0 = true;
            r = Translate(stage, data.data(), data.size(), opt);
        }
        for (auto& u : r.unimplemented)
            unimpl[u]++;
        std::string err;
        if (no_compile) {
            fmt::print("{}\n", r.hlsl);
            continue;
        }
        const bool compiled = r.ok && CompileHlsl(r.hlsl, cs ? "cs_5_0" : vs ? "vs_5_0" : "ps_5_0", err);
        if (print)
            fmt::print("{}\n", r.hlsl);
        if (compiled)
            ok++;
        else {
            fail++;
            fmt::print("FAIL {}: {}\n", f.filename().string(), (r.ok ? err : r.error).substr(0, 400));
        }
    }
    fmt::print("\n{} ok, {} failed\n", ok, fail);
    for (auto& [k, v] : unimpl)
        fmt::print("  {:24} {}\n", k, v);
}
