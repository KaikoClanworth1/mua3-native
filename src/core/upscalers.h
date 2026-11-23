#pragma once
// Which upscalers this PC can run, for the menus and the launcher (mua3.ini [video] upscaler: 1 FSR 1, 2 DLSS, 3 FSR 3.1).
// The renderer fills it in: FSR 1 always; DLSS when mua3_dlss.dll is installed and the GPU is NVIDIA, corrected once
// NGX itself has been asked (the first frame with upscaler=2); FSR 3.1 on feature level 11_1 GPUs with typed UAV
// loads (nearly all DirectX 12 class ones).
#include <atomic>

namespace core {

enum UpscalerKind : int { kUpscalerFsr1 = 1, kUpscalerDlss = 2, kUpscalerFsr3 = 3 };

inline std::atomic<int>& UpscalerMask() {
    static std::atomic<int> mask{1 << kUpscalerFsr1};
    return mask;
}

// `kind` is the upscaler= value (1 FSR 1, 2 DLSS, 3 FSR 3.1); 0 (off) is always available.
inline bool UpscalerAvailable(int kind) { return kind == 0 || (UpscalerMask().load() >> kind & 1) != 0; }

inline void SetUpscalerAvailable(int kind, bool available) {
    if (available)
        UpscalerMask() |= 1 << kind;
    else
        UpscalerMask() &= ~(1 << kind);
}

} // namespace core
