#include "core/hle/service.h"

namespace hle {

void RegisterSm(ServiceManager& sm);
void RegisterMisc(ServiceManager& sm);
void RegisterAm(ServiceManager& sm);
void RegisterFs(ServiceManager& sm);
void RegisterHid(ServiceManager& sm);
void RegisterNvdrv(ServiceManager& sm);
void RegisterVi(ServiceManager& sm);
void RegisterAudio(ServiceManager& sm);

void RegisterAllServices(ServiceManager& sm) {
    RegisterSm(sm);
    RegisterMisc(sm);
    RegisterAm(sm);
    RegisterFs(sm);
    RegisterHid(sm);
    RegisterNvdrv(sm);
    RegisterVi(sm);
    RegisterAudio(sm);
}

} // namespace hle
