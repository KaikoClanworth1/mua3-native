// Small system services: lm, apm, set, time, pl, acc, aoc, nifm, pctl, friend, prepo, csrng, fatal...
#include <chrono>
#include <ctime>
#include <random>

#include "core/hle/service.h"
#include "core/system.h"

namespace hle {

namespace {

// ---------------------------------------------------------------- lm
class ILogger final : public Service {
public:
    ILogger() : Service("lm:ILogger") {
        Reg(0, "Log", [](Request& r) {
            auto d = r.ReadIn(0);
            if (d.size() < 0x18)
                return;
            const u8 severity = d[0x11];
            size_t p = 0x18;
            std::string msg, file, func;
            u32 line = 0;
            while (p < d.size()) {
                const u8 key = d[p++];
                u64 len = 0;
                for (int sh = 0; p < d.size(); sh += 7) {
                    const u8 b = d[p++];
                    len |= u64(b & 0x7F) << sh;
                    if (!(b & 0x80))
                        break;
                }
                if (p + len > d.size())
                    break;
                const std::string val(reinterpret_cast<const char*>(&d[p]), size_t(len));
                switch (key) {
                case 2: msg += val; break;
                case 3: line = len >= 4 ? common::Read<u32>(&d[p]) : 0; break;
                case 4: file = val; break;
                case 5: func = val; break;
                default: break;
                }
                p += size_t(len);
            }
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\0'))
                msg.pop_back();
            if (!msg.empty())
                LOG_INFO("GuestLog", "[sev {}] {}{}", severity, msg,
                         file.empty() ? "" : fmt::format(" ({}:{} {})", file, line, func));
        });
        Ok(1, "SetDestination");
        quiet_ = true;
    }
};

class LmService final : public Service {
public:
    LmService() : Service("lm") {
        Reg(0, "OpenLogger", [](Request& r) { r.PushInterface(Make<ILogger>()); });
    }
};

// ---------------------------------------------------------------- apm
class ApmSession final : public Service {
public:
    ApmSession() : Service("apm:ISession") {
        Ok(0, "SetPerformanceConfiguration");
        Reg(1, "GetPerformanceConfiguration", [](Request& r) { r.Push<u32>(0x00010000); });
    }
};

class ApmService final : public Service {
public:
    explicit ApmService(std::string n) : Service(std::move(n)) {
        Reg(0, "OpenSession", [](Request& r) { r.PushInterface(Make<ApmSession>()); });
        Reg(1, "GetPerformanceMode", [](Request& r) { r.Push<u32>(core::Sys().Cfg().docked ? 1 : 0); });
        Ok(6, "SetCpuBoostMode");
    }
};

// ---------------------------------------------------------------- set / set:sys
u64 LanguageCode(int lang) {
    static const char* codes[] = {"ja",    "en-US", "fr",    "de",    "it",      "es",      "zh-CN", "ko",
                                  "nl",    "pt",    "ru",    "zh-TW", "en-GB",   "fr-CA",   "es-419", "zh-Hans",
                                  "zh-Hant"};
    u64 v = 0;
    const char* c = codes[std::clamp(lang, 0, 16)];
    std::memcpy(&v, c, std::strlen(c));
    return v;
}

class SetService final : public Service {
public:
    SetService() : Service("set") {
        Reg(0, "GetLanguageCode", [](Request& r) { r.Push<u64>(LanguageCode(core::Sys().Cfg().language)); });
        auto list = [](Request& r) {
            std::vector<u64> codes;
            for (int i = 0; i < 17; i++)
                codes.push_back(LanguageCode(i));
            const size_t n = r.WriteOut(codes.data(), codes.size() * 8) / 8;
            r.Push<u32>(u32(n));
        };
        Reg(1, "GetAvailableLanguageCodes", list);
        Reg(5, "GetAvailableLanguageCodes2", list);
        Reg(3, "GetAvailableLanguageCodeCount", [](Request& r) { r.Push<u32>(15); });
        Reg(6, "GetAvailableLanguageCodeCount2", [](Request& r) { r.Push<u32>(17); });
        Reg(4, "GetRegionCode", [](Request& r) { r.Push<u32>(1); }); // USA
        Reg(8, "GetQuestFlag", [](Request& r) { r.Push<u32>(0); });
    }
};

class SetSysService final : public Service {
public:
    SetSysService() : Service("set:sys") {
        auto fw = [](Request& r) {
            u8 buf[0x100]{};
            buf[0] = 6;
            buf[1] = 2;
            buf[2] = 0;
            buf[4] = 1;
            buf[5] = 0;
            std::strcpy(reinterpret_cast<char*>(buf + 0x8), "NX");
            std::strcpy(reinterpret_cast<char*>(buf + 0x28), "6.2.0-1.0");
            std::strcpy(reinterpret_cast<char*>(buf + 0x68), "6.2.0");
            std::strcpy(reinterpret_cast<char*>(buf + 0x80), "NintendoSDK Firmware for NX 6.2.0-1.0");
            r.WriteOut(buf, sizeof(buf));
        };
        Reg(3, "GetFirmwareVersion", fw);
        Reg(4, "GetFirmwareVersion2", fw);
        Reg(23, "GetColorSetId", [](Request& r) { r.Push<u32>(0); });
        Reg(37, "GetSettingsItemValueSize", [](Request& r) {
            LOG_DEBUG("set:sys", "GetSettingsItemValueSize {} {}", r.ReadInString(0), r.ReadInString(1));
            r.Push<u64>(4);
        });
        Reg(38, "GetSettingsItemValue", [](Request& r) {
            LOG_DEBUG("set:sys", "GetSettingsItemValue {} {}", r.ReadInString(0), r.ReadInString(1));
            u32 zero = 0;
            r.Push<u64>(r.WriteOut(&zero, 4));
        });
    }
};

// ---------------------------------------------------------------- time
s64 PosixNow() { return s64(std::time(nullptr)); }

class ISystemClock final : public Service {
public:
    ISystemClock() : Service("time:ISystemClock") {
        Reg(0, "GetCurrentTime", [](Request& r) { r.Push<s64>(PosixNow()); });
        Ok(1, "SetCurrentTime");
        Reg(2, "GetSystemClockContext", [](Request& r) {
            u8 ctx[0x20]{};
            const s64 t = PosixNow();
            std::memcpy(ctx, &t, 8);
            r.PushRaw(ctx, sizeof(ctx));
        });
        Ok(3, "SetSystemClockContext");
        Reg(4, "GetOperationEventReadableHandle", [](Request& r) { r.PushCopy(MakeEvent("clock")); });
    }
};

class ISteadyClock final : public Service {
public:
    ISteadyClock() : Service("time:ISteadyClock") {
        Reg(0, "GetCurrentTimePoint", [](Request& r) {
            r.Push<s64>(s64(r.k.GetTicks() / 19200000));
            r.Push<u64>(0x4D554133ull);
            r.Push<u64>(0x0123456789ull);
        });
        Reg(2, "GetTestOffset", [](Request& r) { r.Push<s64>(0); });
        Reg(100, "GetRtcValue", [](Request& r) { r.Push<s64>(PosixNow()); });
        Reg(200, "GetInternalOffset", [](Request& r) { r.Push<s64>(0); });
    }
};

void ToCalendar(Request& r, s64 t) {
    std::tm tm{};
    const time_t tt = time_t(t);
    localtime_s(&tm, &tt);
    struct {
        s16 year;
        u8 month, day, hour, minute, second, pad;
    } ct{s16(tm.tm_year + 1900), u8(tm.tm_mon + 1), u8(tm.tm_mday), u8(tm.tm_hour), u8(tm.tm_min), u8(tm.tm_sec), 0};
    struct {
        u32 weekday, yearday;
        char name[8];
        u32 is_dst;
        s32 utc_offset;
    } info{u32(tm.tm_wday), u32(tm.tm_yday), "UTC", 0, 0};
    long tz = 0;
    _get_timezone(&tz);
    info.utc_offset = s32(-tz);
    r.PushRaw(&ct, 8);
    r.PushRaw(&info, sizeof(info));
}

class ITimeZoneService final : public Service {
public:
    ITimeZoneService() : Service("time:ITimeZoneService") {
        Reg(0, "GetDeviceLocationName", [](Request& r) {
            char name[0x24]{"UTC"};
            r.PushRaw(name, sizeof(name));
        });
        Reg(2, "GetTotalLocationNameCount", [](Request& r) { r.Push<u32>(1); });
        Reg(4, "LoadTimeZoneRule", [](Request& r) {});
        Reg(100, "ToCalendarTime", [](Request& r) { ToCalendar(r, r.ArgAt<s64>(0)); });
        Reg(101, "ToCalendarTimeWithMyRule", [](Request& r) { ToCalendar(r, r.ArgAt<s64>(0)); });
        Reg(201, "ToPosixTime", [](Request& r) {
            const auto ct = r.ArgAt<std::array<u8, 8>>(0);
            std::tm tm{};
            tm.tm_year = common::Read<s16>(&ct[0]) - 1900;
            tm.tm_mon = ct[2] - 1;
            tm.tm_mday = ct[3];
            tm.tm_hour = ct[4];
            tm.tm_min = ct[5];
            tm.tm_sec = ct[6];
            const s64 t = s64(_mkgmtime(&tm));
            r.WriteOut(&t, 8);
            r.Push<u32>(1);
        });
        Reg(202, "ToPosixTimeWithMyRule", [](Request& r) {
            const auto ct = r.ArgAt<std::array<u8, 8>>(0);
            std::tm tm{};
            tm.tm_year = common::Read<s16>(&ct[0]) - 1900;
            tm.tm_mon = ct[2] - 1;
            tm.tm_mday = ct[3];
            tm.tm_hour = ct[4];
            tm.tm_min = ct[5];
            tm.tm_sec = ct[6];
            const s64 t = s64(std::mktime(&tm));
            r.WriteOut(&t, 8);
            r.Push<u32>(1);
        });
    }
};

class TimeService final : public Service {
public:
    explicit TimeService(std::string n) : Service(std::move(n)) {
        Reg(0, "GetStandardUserSystemClock", [](Request& r) { r.PushInterface(Make<ISystemClock>()); });
        Reg(1, "GetStandardNetworkSystemClock", [](Request& r) { r.PushInterface(Make<ISystemClock>()); });
        Reg(2, "GetStandardSteadyClock", [](Request& r) { r.PushInterface(Make<ISteadyClock>()); });
        Reg(3, "GetTimeZoneService", [](Request& r) { r.PushInterface(Make<ITimeZoneService>()); });
        Reg(4, "GetStandardLocalSystemClock", [](Request& r) { r.PushInterface(Make<ISystemClock>()); });
        Reg(5, "GetEphemeralNetworkSystemClock", [](Request& r) { r.PushInterface(Make<ISystemClock>()); });
        Reg(20, "GetSharedMemoryNativeHandle", [](Request& r) {
            static auto shm = std::make_shared<kernel::SharedMemory>(0x1000, "time");
            r.PushCopy(shm);
        });
        Reg(100, "IsStandardUserSystemClockAutomaticCorrectionEnabled", [](Request& r) { r.Push<u8>(1); });
        // (the network clock is the host's clock: accurate)
        Reg(200, "IsStandardNetworkSystemClockAccuracySufficient", [](Request& r) { r.Push<u8>(1); });
        Reg(300, "CalculateMonotonicSystemClockBaseTimePoint", [](Request& r) {
            r.Push<s64>(PosixNow() - s64(r.k.GetTicks() / 19200000));
        });
    }
};

// ---------------------------------------------------------------- pl:u (shared font)
class PlService final : public Service {
public:
    PlService() : Service("pl:u") {
        Ok(0, "RequestLoad");
        Reg(1, "GetLoadState", [](Request& r) { r.Push<u32>(1); });
        Reg(2, "GetSize", [](Request& r) { r.Push<u32>(0); });
        Reg(3, "GetSharedMemoryAddressOffset", [](Request& r) { r.Push<u32>(0); });
        Reg(4, "GetSharedMemoryNativeHandle", [](Request& r) {
            static auto shm = std::make_shared<kernel::SharedMemory>(0x1100000, "font");
            r.PushCopy(shm);
        });
        Reg(5, "GetSharedFontInOrderOfPriority", [](Request& r) {
            r.Push<u8>(1);
            r.Push<u32>(0);
        });
    }
};

// ---------------------------------------------------------------- acc
constexpr u128 kUserId = {0x4D55413355534552ull, 0x0000000000000001ull};

class IProfile final : public Service {
public:
    IProfile() : Service("acc:IProfile") {
        auto base = [](Request& r) {
            u8 pb[0x38]{};
            std::memcpy(pb, kUserId.data(), 16);
            const u64 ts = u64(PosixNow());
            std::memcpy(pb + 0x10, &ts, 8);
            std::strcpy(reinterpret_cast<char*>(pb + 0x18), "Player");
            r.PushRaw(pb, sizeof(pb));
        };
        Reg(0, "Get", [base](Request& r) {
            u8 ud[0x80]{};
            r.WriteOut(ud, sizeof(ud));
            base(r);
        });
        Reg(1, "GetBase", base);
        Reg(10, "GetImageSize", [](Request& r) { r.Push<u32>(0); });
        Reg(11, "LoadImage", [](Request& r) { r.Push<u32>(0); });
    }
};

class IManagerForApplication final : public Service {
public:
    IManagerForApplication() : Service("acc:IManagerForApplication") {
        Reg(0, "CheckAvailability", [](Request& r) { r.Push<u8>(0); });
        Reg(1, "GetAccountId", [](Request& r) { r.Push<u64>(0x4D55413300000001ull); });
        Reg(130, "GetNintendoAccountUserResourceCacheForApplication", [](Request& r) { r.SetError(0x7C7C); });
    }
};

class AccService final : public Service {
public:
    explicit AccService(std::string n) : Service(std::move(n)) {
        Reg(0, "GetUserCount", [](Request& r) { r.Push<u32>(1); });
        Reg(1, "GetUserExistence", [](Request& r) { r.Push<u8>(r.ArgAt<u128>(0) == kUserId); });
        auto list = [](Request& r) {
            u128 ids[8]{};
            ids[0] = kUserId;
            r.WriteOut(ids, sizeof(ids));
        };
        Reg(2, "ListAllUsers", list);
        Reg(3, "ListOpenUsers", list);
        Reg(4, "GetLastOpenedUser", [](Request& r) { r.Push(kUserId); });
        Reg(5, "GetProfile", [](Request& r) { r.PushInterface(Make<IProfile>()); });
        Reg(50, "IsUserRegistrationRequestPermitted", [](Request& r) { r.Push<u8>(0); });
        Reg(51, "TrySelectUserWithoutInteraction", [](Request& r) { r.Push(kUserId); });
        Ok(100, "InitializeApplicationInfo");
        Reg(101, "GetBaasAccountManagerForApplication",
            [](Request& r) { r.PushInterface(Make<IManagerForApplication>()); });
        Ok(110, "StoreSaveDataThumbnail");
        Ok(111, "ClearSaveDataThumbnail");
        Reg(140, "ListQualifiedUsers", list);
    }
};

// ---------------------------------------------------------------- aoc:u
class AocService final : public Service {
public:
    AocService() : Service("aoc:u") {
        Reg(2, "CountAddOnContent", [](Request& r) { r.Push<u32>(1); });
        Reg(3, "ListAddOnContent", [](Request& r) {
            const u32 offset = r.ArgAt<u32>(0);
            u32 idx = 1;
            r.Push<u32>(offset == 0 && r.OutSize() >= 4 ? u32(r.WriteOut(&idx, 4) / 4) : 0);
        });
        Reg(5, "GetAddOnContentBaseId", [](Request& r) { r.Push<u64>(r.k.program_id + 0x1000); });
        Ok(7, "PrepareAddOnContent");
        Reg(8, "GetAddOnContentListChangedEvent", [](Request& r) { r.PushCopy(MakeEvent("aoc")); });
    }
};

// ---------------------------------------------------------------- nifm (always offline)
class INifmRequest final : public Service {
public:
    INifmRequest() : Service("nifm:IRequest") {
        Reg(0, "GetRequestState", [](Request& r) { r.Push<u32>(1); }); // error / not available
        Reg(1, "GetResult", [](Request& r) { r.SetError(0xD46E); });
        Reg(2, "GetSystemEventReadableHandles", [](Request& r) {
            r.PushCopy(MakeEvent("nifm0", true));
            r.PushCopy(MakeEvent("nifm1", true));
        });
        Ok(3, "Cancel");
        Ok(4, "Submit");
        Ok(6, "SetRequirementPreset");
        Ok(11, "SetConnectionConfirmationOption");
        Ok(21, "GetAppletInfo");
    }
};

class INifmGeneral final : public Service {
public:
    INifmGeneral() : Service("nifm:IGeneralService") {
        Reg(1, "GetClientId", [](Request& r) { r.Push<u32>(1); });
        Reg(4, "CreateRequest", [](Request& r) { r.PushInterface(Make<INifmRequest>()); });
        Reg(5, "GetCurrentNetworkProfile", [](Request& r) { r.SetError(0xD46E); });
        Reg(12, "GetCurrentIpAddress", [](Request& r) { r.SetError(0xD46E); });
        Reg(18, "GetInternetConnectionStatus", [](Request& r) { r.SetError(0xD46E); });
        Reg(21, "IsAnyInternetRequestAccepted", [](Request& r) { r.Push<u8>(0); });
        Reg(22, "IsAnyForegroundRequestAccepted", [](Request& r) { r.Push<u8>(0); });
    }
};

class NifmService final : public Service {
public:
    explicit NifmService(std::string n) : Service(std::move(n)) {
        Reg(4, "CreateGeneralServiceOld", [](Request& r) { r.PushInterface(Make<INifmGeneral>()); });
        Reg(5, "CreateGeneralService", [](Request& r) { r.PushInterface(Make<INifmGeneral>()); });
    }
};

// ---------------------------------------------------------------- pctl
class IParentalControl final : public Service {
public:
    IParentalControl() : Service("pctl:IParentalControlService") {
        Ok(1, "Initialize");
        Ok(1001, "CheckFreeCommunicationPermission");
        Reg(1006, "IsRestrictionTemporaryUnlocked", [](Request& r) { r.Push<u8>(0); });
        Ok(1013, "ConfirmStereoVisionPermission");
        Reg(1018, "IsRestrictionEnabled", [](Request& r) { r.Push<u8>(0); });
        Reg(1031, "IsRestrictionEnabled", [](Request& r) { r.Push<u8>(0); });
        Reg(1061, "ConfirmStereoVisionRestrictionConfigurable", [](Request& r) {});
        Reg(1064, "GetStereoVisionRestriction", [](Request& r) { r.Push<u8>(0); });
        Ok(1065, "SetStereoVisionRestriction");
    }
};

class PctlService final : public Service {
public:
    explicit PctlService(std::string n) : Service(std::move(n)) {
        Reg(0, "CreateService", [](Request& r) { r.PushInterface(Make<IParentalControl>()); });
        Reg(1, "CreateServiceWithoutInitialize", [](Request& r) { r.PushInterface(Make<IParentalControl>()); });
    }
};

// ---------------------------------------------------------------- friend / prepo / misc online stubs
class IFriendService final : public Service {
public:
    IFriendService() : Service("friend:IFriendService") {
        Reg(10600, "DeclareOpenOnlinePlaySession", [](Request&) {});
        Reg(10601, "DeclareCloseOnlinePlaySession", [](Request&) {});
        Reg(10610, "UpdateUserPresence", [](Request&) {});
        Reg(0, "GetCompletionEvent", [](Request& r) { r.PushCopy(MakeEvent("friend")); });
    }
};

class FriendService final : public Service {
public:
    explicit FriendService(std::string n) : Service(std::move(n)) {
        Reg(0, "CreateFriendService", [](Request& r) { r.PushInterface(Make<IFriendService>()); });
        Reg(1, "CreateNotificationService", [](Request& r) { r.PushInterface(MakeStub("friend:INotification")); });
    }
};

class CsrngService final : public Service {
public:
    CsrngService() : Service("csrng") {
        Reg(0, "GenerateRandomBytes", [](Request& r) {
            std::vector<u8> b(size_t(r.OutSize()));
            std::random_device rd;
            for (auto& x : b)
                x = u8(rd());
            r.WriteOut(b);
        });
    }
};

class FatalService final : public Service {
public:
    FatalService() : Service("fatal:u") {
        auto fatal = [](Request& r) {
            LOG_ERROR("fatal", "Guest threw fatal error {:#x} (cmd {})", r.ArgAt<u32>(0), r.cmd);
            kernel::K().DumpThreadState(r.thread);
            PANIC("fatal:u ThrowFatal ({:#x})", r.ArgAt<u32>(0));
        };
        Reg(0, "ThrowFatal", fatal);
        Reg(1, "ThrowFatalWithPolicy", fatal);
        Reg(2, "ThrowFatalWithCpuContext", fatal);
    }
};

class SslService final : public Service {
public:
    SslService() : Service("ssl") {
        Reg(0, "CreateContext", [](Request& r) { r.PushInterface(MakeStub("ssl:ISslContext")); });
        Ok(5, "SetInterfaceVersion");
    }
};

class PrepoService final : public Service {
public:
    explicit PrepoService(std::string n) : Service(std::move(n)) {
        quiet_ = true;
        for (u32 id : {10100u, 10101u, 10102u, 10103u, 10104u, 10200u, 10300u, 20100u, 20101u, 30100u})
            Ok(id, "SaveReport");
    }
};

} // namespace

void RegisterMisc(ServiceManager& sm) {
    sm.RegisterService("lm", [] { return Make<LmService>(); });
    for (const char* n : {"apm", "apm:p", "apm:am"})
        sm.RegisterService(n, [n] { return Make<ApmService>(n); });
    sm.RegisterService("set", [] { return Make<SetService>(); });
    sm.RegisterService("set:sys", [] { return Make<SetSysService>(); });
    for (const char* n : {"time:u", "time:a", "time:s"})
        sm.RegisterService(n, [n] { return Make<TimeService>(n); });
    sm.RegisterService("pl:u", [] { return Make<PlService>(); });
    for (const char* n : {"acc:u0", "acc:u1", "acc:su", "acc:aa"})
        sm.RegisterService(n, [n] { return Make<AccService>(n); });
    sm.RegisterService("aoc:u", [] { return Make<AocService>(); });
    for (const char* n : {"nifm:u", "nifm:a", "nifm:s"})
        sm.RegisterService(n, [n] { return Make<NifmService>(n); });
    for (const char* n : {"pctl", "pctl:a", "pctl:s", "pctl:r"})
        sm.RegisterService(n, [n] { return Make<PctlService>(n); });
    for (const char* n : {"friend:u", "friend:a", "friend:v"})
        sm.RegisterService(n, [n] { return Make<FriendService>(n); });
    for (const char* n : {"prepo:u", "prepo:a", "prepo:s"})
        sm.RegisterService(n, [n] { return Make<PrepoService>(n); });
    sm.RegisterService("csrng", [] { return Make<CsrngService>(); });
    sm.RegisterService("fatal:u", [] { return Make<FatalService>(); });
    sm.RegisterService("ssl", [] { return Make<SslService>(); });
}

} // namespace hle
