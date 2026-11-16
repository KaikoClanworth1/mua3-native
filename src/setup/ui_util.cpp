#include "setup/ui_util.h"

#include <d3d11.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windows.h>

namespace ui {

HWND__* g_hwnd = nullptr;

Texture LoadImageTexture(ID3D11Device* dev, const fs::path& path) {
    Texture t;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return t;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        std::vector<u8> px(size_t(w) * h * 4);
        conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data());
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{px.data(), w * 4, 0};
        ID3D11Texture2D* tex = nullptr;
        if (w > 0 && h > 0 && SUCCEEDED(dev->CreateTexture2D(&td, &sd, &tex))) {
            dev->CreateShaderResourceView(tex, nullptr, &t.srv);
            tex->Release();
            t.w = int(w), t.h = int(h);
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    wic->Release();
    return t;
}

void Release(Texture& t) {
    if (t.srv)
        t.srv->Release();
    t = {};
}

std::vector<fs::path> FileDialog(bool folder, bool multi, const wchar_t* title,
                                 std::vector<std::pair<std::wstring, std::wstring>> filters, const fs::path& start) {
    std::vector<fs::path> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST) | (multi ? FOS_ALLOWMULTISELECT : 0);
    dlg->SetOptions(opts);
    dlg->SetTitle(title);
    std::vector<COMDLG_FILTERSPEC> spec;
    for (auto& f : filters)
        spec.push_back({f.first.c_str(), f.second.c_str()});
    if (!spec.empty())
        dlg->SetFileTypes(UINT(spec.size()), spec.data());
    std::error_code ec;
    fs::path dir = start;
    while (!dir.empty() && !fs::is_directory(dir, ec) && dir != dir.parent_path())
        dir = dir.parent_path();
    IShellItem* item = nullptr;
    if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(fs::absolute(dir).wstring().c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        dlg->SetFolder(item);
        item->Release();
    }
    if (SUCCEEDED(dlg->Show(reinterpret_cast<HWND>(g_hwnd)))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; i++) {
                IShellItem* it = nullptr;
                PWSTR path = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out.emplace_back(path);
                    CoTaskMemFree(path);
                }
                if (it)
                    it->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    return out;
}

bool Recycle(const fs::path& p) {
    std::error_code ec;
    std::wstring from = fs::absolute(p, ec).wstring();
    from.push_back(L'\0'); // (a double-null-terminated list)
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted && !fs::exists(p, ec);
}

} // namespace ui
