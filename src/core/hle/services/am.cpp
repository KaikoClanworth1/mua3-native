// appletOE: application proxy and its sub-interfaces.
#include <deque>

#include "core/hle/service.h"
#include "core/system.h"

namespace hle {

namespace {

constexpr Result kNoMessages = 0x680;
constexpr Result kNoDataInChannel = 0x480; // AM 2: no data in channel

enum AppletMessage : u32 {
    ExitRequested = 4,
    FocusStateChanged = 15,
    OperationModeChanged = 30,
    PerformanceModeChanged = 31,
};

struct AppletState {
    std::mutex mutex;
    std::deque<u32> messages;
    std::shared_ptr<kernel::Event> message_event = MakeEvent("am:message");
    std::shared_ptr<kernel::Event> launchable_event = MakeEvent("am:launchable");
    std::shared_ptr<kernel::Event> gpu_error_event = MakeEvent("am:gpuerror");
    std::shared_ptr<kernel::Event> suspended_tick_event = MakeEvent("am:suspendtick");
    std::shared_ptr<kernel::Event> display_res_event = MakeEvent("am:displayres");
    std::shared_ptr<kernel::Event> friend_invite_event = MakeEvent("am:friendinvite");
    std::shared_ptr<kernel::Event> health_warning_event = MakeEvent("am:health");

    AppletState() {
        messages.push_back(FocusStateChanged);
        message_event->signaled = true;
    }
    void Push(u32 m) {
        std::lock_guard lk(mutex);
        messages.push_back(m);
        kernel::K().SignalEvent(*message_event);
    }
};

AppletState& State() {
    static AppletState s;
    return s;
}

class IStorageAccessor final : public Service {
public:
    explicit IStorageAccessor(std::shared_ptr<std::vector<u8>> d) : Service("am:IStorageAccessor"), data_(d) {
        Reg(0, "GetSize", [this](Request& r) { r.Push<s64>(s64(data_->size())); });
        Reg(10, "Write", [this](Request& r) {
            const u64 off = r.ArgAt<u64>(0);
            auto in = r.ReadIn(0);
            if (off + in.size() > data_->size()) {
                r.SetError(0x1FA80);
                return;
            }
            std::memcpy(data_->data() + off, in.data(), in.size());
        });
        Reg(11, "Read", [this](Request& r) {
            const u64 off = r.ArgAt<u64>(0);
            if (off > data_->size()) {
                r.SetError(0x1FA80);
                return;
            }
            r.WriteOut(data_->data() + off, size_t(data_->size() - off));
        });
    }

private:
    std::shared_ptr<std::vector<u8>> data_;
};

class IStorage final : public Service {
public:
    explicit IStorage(std::vector<u8> d) : Service("am:IStorage"), data_(std::make_shared<std::vector<u8>>(std::move(d))) {
        Reg(0, "Open", [this](Request& r) { r.PushInterface(Make<IStorageAccessor>(data_)); });
    }

private:
    std::shared_ptr<std::vector<u8>> data_;
};

class ICommonStateGetter final : public Service {
public:
    ICommonStateGetter() : Service("am:ICommonStateGetter") {
        Reg(0, "GetEventHandle", [](Request& r) { r.PushCopy(State().message_event); });
        Reg(1, "ReceiveMessage", [](Request& r) {
            auto& s = State();
            std::lock_guard lk(s.mutex);
            if (s.messages.empty()) {
                r.SetError(kNoMessages);
                return;
            }
            const u32 m = s.messages.front();
            s.messages.pop_front();
            if (s.messages.empty()) {
                std::lock_guard klk(r.k.lock);
                s.message_event->signaled = false;
            }
            LOG_DEBUG("am", "ReceiveMessage -> {}", m);
            r.Push<u32>(m);
        });
        Reg(5, "GetOperationMode", [](Request& r) { r.Push<u8>(core::Sys().Cfg().docked ? 1 : 0); });
        Reg(6, "GetPerformanceMode", [](Request& r) { r.Push<u32>(core::Sys().Cfg().docked ? 1 : 0); });
        Reg(8, "GetBootMode", [](Request& r) { r.Push<u8>(0); });
        Reg(9, "GetCurrentFocusState", [](Request& r) { r.Push<u8>(1); });
        Reg(10, "RequestToAcquireSleepLock", [](Request&) {});
        Reg(13, "GetAcquiredSleepLockEvent", [](Request& r) { r.PushCopy(MakeEvent("sleeplock")); });
        Reg(50, "IsVrModeEnabled", [](Request& r) { r.Push<u8>(0); });
        Ok(51, "SetVrModeEnabled");
        Ok(52, "SetLcdBacklighOffEnabled");
        Reg(60, "GetDefaultDisplayResolution", [](Request& r) {
            const bool docked = core::Sys().Cfg().docked;
            r.Push<s32>(docked ? 1920 : 1280);
            r.Push<s32>(docked ? 1080 : 720);
        });
        Reg(61, "GetDefaultDisplayResolutionChangeEvent", [](Request& r) { r.PushCopy(State().display_res_event); });
        Ok(66, "SetCpuBoostMode");
        Reg(90, "GetCurrentPerformanceConfiguration", [](Request& r) { r.Push<u32>(0x00010000); });
    }
};

class ISelfController final : public Service {
public:
    ISelfController() : Service("am:ISelfController") {
        Reg(0, "Exit", [](Request&) { kernel::K().ExitProcess(0); });
        Ok(1, "LockExit");
        Ok(2, "UnlockExit");
        Ok(3, "EnterFatalSection");
        Ok(4, "LeaveFatalSection");
        Reg(9, "GetLibraryAppletLaunchableEvent", [](Request& r) {
            State().launchable_event->signaled = true;
            r.PushCopy(State().launchable_event);
        });
        Ok(10, "SetScreenShotPermission");
        Ok(11, "SetOperationModeChangedNotification");
        Ok(12, "SetPerformanceModeChangedNotification");
        Ok(13, "SetFocusHandlingMode");
        Ok(14, "SetRestartMessageEnabled");
        Ok(15, "SetScreenShotAppletIdentityInfo");
        Ok(16, "SetOutOfFocusSuspendingEnabled");
        Ok(17, "SetControllerFirmwareUpdateSection");
        Ok(18, "SetRequiresCaptureButtonShortPressedMessage");
        Ok(19, "SetAlbumImageOrientation");
        Ok(20, "SetDesirableKeyboardLayout");
        Reg(40, "CreateManagedDisplayLayer", [](Request& r) { r.Push<u64>(1); });
        Ok(41, "IsSystemBufferSharingEnabled");
        Ok(44, "CreateManagedDisplaySeparableLayer");
        Ok(50, "SetHandlesRequestToDisplay");
        Ok(51, "ApproveToDisplay");
        Ok(62, "SetIdleTimeDetectionExtension");
        Reg(63, "GetIdleTimeDetectionExtension", [](Request& r) { r.Push<u32>(0); });
        Ok(64, "SetInputDetectionSourceSet");
        Ok(65, "ReportUserIsActive");
        Ok(68, "SetAutoSleepDisabled");
        Reg(69, "IsAutoSleepDisabled", [](Request& r) { r.Push<u8>(0); });
        Reg(90, "GetAccumulatedSuspendedTickValue", [](Request& r) { r.Push<u64>(0); });
        Reg(91, "GetAccumulatedSuspendedTickChangedEvent", [](Request& r) {
            r.PushCopy(State().suspended_tick_event);
        });
        Ok(100, "SetAlbumImageTakenNotificationEnabled");
        Ok(110, "SetApplicationAlbumUserData");
    }
};

class IWindowController final : public Service {
public:
    IWindowController() : Service("am:IWindowController") {
        Reg(1, "GetAppletResourceUserId", [](Request& r) { r.Push<u64>(0x51); });
        Ok(10, "AcquireForegroundRights");
    }
};

class IAudioController final : public Service {
public:
    IAudioController() : Service("am:IAudioController") {
        Ok(0, "SetExpectedMasterVolume");
        Reg(1, "GetMainAppletExpectedMasterVolume", [](Request& r) { r.Push<f32>(1.0f); });
        Reg(2, "GetLibraryAppletExpectedMasterVolume", [](Request& r) { r.Push<f32>(1.0f); });
        Ok(3, "ChangeMainAppletMasterVolume");
        Ok(4, "SetTransparentVolumeRate");
    }
};

class IApplicationFunctions final : public Service {
public:
    IApplicationFunctions() : Service("am:IApplicationFunctions") {
        Reg(1, "PopLaunchParameter", [this](Request& r) {
            const u32 kind = r.ArgAt<u32>(0);
            if (kind == 2 && !popped_user_) { // PreselectedUser
                popped_user_ = true;
                std::vector<u8> d(0x88);
                const u32 magic = 0xC79497CA, unk = 1;
                std::memcpy(&d[0], &magic, 4);
                std::memcpy(&d[4], &unk, 4);
                const u64 uid[2] = {0x4D55413355534552ull, 1};
                std::memcpy(&d[8], uid, 16);
                r.PushInterface(Make<IStorage>(std::move(d)));
                return;
            }
            r.SetError(kNoDataInChannel);
        });
        Reg(20, "EnsureSaveData", [](Request& r) { r.Push<u64>(0); });
        Reg(21, "GetDesiredLanguage", [](Request& r) {
            u64 v = 0;
            std::memcpy(&v, "en-US", 5);
            r.Push<u64>(v);
        });
        Ok(22, "SetTerminateResult");
        Reg(23, "GetDisplayVersion", [](Request& r) {
            char ver[0x10] = "3.0.0";
            r.PushRaw(ver, sizeof(ver));
        });
        Reg(25, "ExtendSaveData", [](Request& r) { r.Push<u64>(0); });
        Reg(26, "GetSaveDataSize", [](Request& r) {
            r.Push<u64>(0x4000000);
            r.Push<u64>(0x4000000);
        });
        Reg(40, "NotifyRunning", [](Request& r) { r.Push<u8>(0); });
        Reg(50, "GetPseudoDeviceId", [](Request& r) {
            r.Push<u64>(0x4D554133ull);
            r.Push<u64>(0x0001ull);
        });
        Ok(60, "SetMediaPlaybackStateForApplication");
        Reg(65, "IsGamePlayRecordingSupported", [](Request& r) { r.Push<u8>(0); });
        Ok(66, "InitializeGamePlayRecording");
        Ok(67, "SetGamePlayRecordingState");
        Ok(90, "EnableApplicationCrashReport");
        Ok(100, "InitializeApplicationCopyrightFrameBuffer");
        Ok(101, "SetApplicationCopyrightImage");
        Ok(102, "SetApplicationCopyrightVisibility");
        Reg(110, "QueryApplicationPlayStatistics", [](Request& r) { r.Push<u32>(0); });
        Reg(130, "GetGpuErrorDetectedSystemEvent", [](Request& r) { r.PushCopy(State().gpu_error_event); });
        Reg(140, "GetFriendInvitationStorageChannelEvent", [](Request& r) {
            r.PushCopy(State().friend_invite_event);
        });
        Reg(150, "GetHealthWarningDisappearedSystemEvent", [](Request& r) {
            r.PushCopy(State().health_warning_event);
        });
    }

private:
    bool popped_user_ = false;
};

// Library applets (controller support, error viewer, software keyboard...) complete immediately with a
// sensible result instead of showing system UI.
class ILibraryAppletAccessor final : public Service {
public:
    explicit ILibraryAppletAccessor(u32 applet_id) : Service("am:ILibraryAppletAccessor"), id_(applet_id) {
        Reg(0, "GetAppletStateChangedEvent", [this](Request& r) { r.PushCopy(state_event_); });
        Reg(1, "IsCompleted", [this](Request& r) { r.Push<u8>(completed_); });
        Reg(10, "Start", [this](Request& r) {
            Complete();
            kernel::K().SignalEvent(*state_event_);
        });
        Reg(20, "RequestExit", [this](Request&) { kernel::K().SignalEvent(*state_event_); });
        Reg(25, "Terminate", [this](Request&) {});
        Reg(30, "GetResult", [](Request&) {});
        Reg(50, "SetOutOfFocusApplicationSuspendingEnabled", [](Request&) {});
        Reg(100, "PushInData", [this](Request&) {});
        Reg(101, "PopOutData", [this](Request& r) {
            if (out_.empty()) {
                r.SetError(kNoDataInChannel);
                return;
            }
            r.PushInterface(Make<IStorage>(std::move(out_.front())));
            out_.erase(out_.begin());
        });
        Reg(105, "GetPopOutDataEvent", [this](Request& r) { r.PushCopy(pop_event_); });
        Reg(106, "GetPopInteractiveOutDataEvent", [this](Request& r) { r.PushCopy(MakeEvent("am:interactive")); });
        Reg(150, "NeedsToExitProcess", [](Request& r) { r.Push<u8>(0); });
    }

private:
    void Complete() {
        LOG_INFO("am", "Library applet {:#x} started; completing immediately", id_);
        if (id_ == 0xC) { // Controller: one player on Npad No1
            std::vector<u8> res(0xC);
            res[0] = 1;
            out_.push_back(std::move(res));
        } else if (id_ == 0x11) { // Software keyboard: cancelled
            std::vector<u8> res(0x7D8);
            out_.push_back(std::move(res));
        }
        completed_ = true;
        kernel::K().SignalEvent(*pop_event_);
    }
    u32 id_;
    bool completed_ = false;
    std::vector<std::vector<u8>> out_;
    std::shared_ptr<kernel::Event> state_event_ = MakeEvent("am:appletstate");
    std::shared_ptr<kernel::Event> pop_event_ = MakeEvent("am:popout");
};

class ILibraryAppletCreator final : public Service {
public:
    ILibraryAppletCreator() : Service("am:ILibraryAppletCreator") {
        Reg(0, "CreateLibraryApplet", [](Request& r) {
            r.PushInterface(Make<ILibraryAppletAccessor>(r.ArgAt<u32>(0)));
        });
        Reg(10, "CreateStorage", [](Request& r) {
            r.PushInterface(Make<IStorage>(std::vector<u8>(size_t(r.ArgAt<u64>(0)))));
        });
    }
};

class IApplicationProxy final : public Service {
public:
    IApplicationProxy() : Service("am:IApplicationProxy") {
        Reg(0, "GetCommonStateGetter", [](Request& r) { r.PushInterface(Make<ICommonStateGetter>()); });
        Reg(1, "GetSelfController", [](Request& r) { r.PushInterface(Make<ISelfController>()); });
        Reg(2, "GetWindowController", [](Request& r) { r.PushInterface(Make<IWindowController>()); });
        Reg(3, "GetAudioController", [](Request& r) { r.PushInterface(Make<IAudioController>()); });
        Reg(4, "GetDisplayController", [](Request& r) { r.PushInterface(MakeStub("am:IDisplayController")); });
        Reg(10, "GetProcessWindingController", [](Request& r) { r.PushInterface(MakeStub("am:IProcessWinding")); });
        Reg(11, "GetLibraryAppletCreator", [](Request& r) { r.PushInterface(Make<ILibraryAppletCreator>()); });
        Reg(20, "GetApplicationFunctions", [](Request& r) { r.PushInterface(Make<IApplicationFunctions>()); });
        Reg(1000, "GetDebugFunctions", [](Request& r) { r.PushInterface(MakeStub("am:IDebugFunctions")); });
    }
};

class AppletOE final : public Service {
public:
    AppletOE() : Service("appletOE") {
        Reg(0, "OpenApplicationProxy", [](Request& r) { r.PushInterface(Make<IApplicationProxy>()); });
    }
};

} // namespace

void RegisterAm(ServiceManager& sm) {
    sm.RegisterService("appletOE", [] { return Make<AppletOE>(); });
}

} // namespace hle
