#include "Autostart.h"
#include "WinIncludes.h"

#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>
#include <cstdio>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {

constexpr wchar_t kTaskName[] = L"Latenci";
constexpr wchar_t kLegacyTaskName[] = L"RoutingCrumbs";

// Connecting to the Task Scheduler service is a cross-process RPC that used to
// run on every context-menu open. The app owns this task exclusively, so one
// query per process is enough.
enum class CacheState { Unknown, Enabled, Disabled };
CacheState g_cache = CacheState::Unknown;

struct ComScope {
    bool active = false;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        active = SUCCEEDED(hr);
        // RPC_E_CHANGED_MODE: already initialized on another model; proceed.
        if (hr == RPC_E_CHANGED_MODE) {
            active = false;
        }
    }
    ~ComScope() {
        if (active) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

struct BStr {
    BSTR value = nullptr;
    explicit BStr(const wchar_t* s) : value(SysAllocString(s)) {}
    ~BStr() {
        if (value) {
            SysFreeString(value);
        }
    }
    BStr(const BStr&) = delete;
    BStr& operator=(const BStr&) = delete;
};

VARIANT EmptyVariant() {
    VARIANT v;
    VariantInit(&v);
    return v;
}

bool GetExePath(wchar_t* out, DWORD outChars) {
    const DWORD n = GetModuleFileNameW(nullptr, out, outChars);
    return n > 0 && n < outChars;
}

bool GetExeDirectory(wchar_t* out, DWORD outChars) {
    if (!GetExePath(out, outChars)) {
        return false;
    }
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) {
        return false;
    }
    *slash = L'\0';
    return true;
}

bool GetCurrentUserId(wchar_t* out, DWORD outChars) {
    wchar_t domain[128]{};
    wchar_t name[128]{};
    const DWORD d = GetEnvironmentVariableW(L"USERDOMAIN", domain, 128);
    const DWORD n = GetEnvironmentVariableW(L"USERNAME", name, 128);
    if (d > 0 && d < 128 && n > 0 && n < 128) {
        return swprintf_s(out, outChars, L"%s\\%s", domain, name) > 0;
    }
    DWORD len = outChars;
    return GetUserNameW(out, &len) != FALSE && out[0] != L'\0';
}

bool GetRootFolder(ITaskService** serviceOut, ITaskFolder** folderOut) {
    *serviceOut = nullptr;
    *folderOut = nullptr;

    ITaskService* service = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_TaskScheduler,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_ITaskService,
        reinterpret_cast<void**>(&service));
    if (FAILED(hr) || !service) {
        return false;
    }

    VARIANT empty = EmptyVariant();
    hr = service->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) {
        service->Release();
        return false;
    }

    BStr rootPath(L"\\");
    ITaskFolder* root = nullptr;
    hr = service->GetFolder(rootPath.value, &root);
    if (FAILED(hr) || !root) {
        service->Release();
        return false;
    }

    *serviceOut = service;
    *folderOut = root;
    return true;
}

bool QueryTaskEnabled(ITaskFolder* root, const wchar_t* name) {
    BStr taskName(name);
    IRegisteredTask* task = nullptr;
    const HRESULT hr = root->GetTask(taskName.value, &task);
    bool enabled = false;
    if (SUCCEEDED(hr) && task) {
        VARIANT_BOOL on = VARIANT_FALSE;
        if (SUCCEEDED(task->get_Enabled(&on))) {
            enabled = (on == VARIANT_TRUE);
        }
        task->Release();
    }
    return enabled;
}

bool QueryEnabled() {
    ComScope com;
    ITaskService* service = nullptr;
    ITaskFolder* root = nullptr;
    if (!GetRootFolder(&service, &root)) {
        return false;
    }
    const bool enabled =
        QueryTaskEnabled(root, kTaskName) || QueryTaskEnabled(root, kLegacyTaskName);
    root->Release();
    service->Release();
    return enabled;
}

void DeleteTaskByName(ITaskFolder* root, const wchar_t* name) {
    BStr taskName(name);
    root->DeleteTask(taskName.value, 0);
}

}  // namespace

bool Autostart::IsEnabled() {
    if (g_cache == CacheState::Unknown) {
        g_cache = QueryEnabled() ? CacheState::Enabled : CacheState::Disabled;
    }
    return g_cache == CacheState::Enabled;
}

bool Autostart::SetEnabled(bool enable, ErrorMsg& error) {
    error.Clear();
    ComScope com;

    ITaskService* service = nullptr;
    ITaskFolder* root = nullptr;
    if (!GetRootFolder(&service, &root)) {
        error.Set(L"Could not open Task Scheduler.");
        return false;
    }

    // Always clear the legacy task name when toggling.
    DeleteTaskByName(root, kLegacyTaskName);

    BStr taskName(kTaskName);

    if (!enable) {
        const HRESULT hr = root->DeleteTask(taskName.value, 0);
        root->Release();
        service->Release();
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            error.Set(L"Could not remove the startup task.");
            return false;
        }
        g_cache = CacheState::Disabled;
        return true;
    }

    wchar_t exePath[MAX_PATH]{};
    wchar_t workDir[MAX_PATH]{};
    wchar_t userId[257]{};
    if (!GetExePath(exePath, MAX_PATH) || !GetExeDirectory(workDir, MAX_PATH)) {
        root->Release();
        service->Release();
        error.Set(L"Could not resolve the application path.");
        return false;
    }
    if (!GetCurrentUserId(userId, 257)) {
        root->Release();
        service->Release();
        error.Set(L"Could not resolve the current user.");
        return false;
    }

    DeleteTaskByName(root, kTaskName);

    ITaskDefinition* task = nullptr;
    HRESULT hr = service->NewTask(0, &task);
    if (FAILED(hr) || !task) {
        root->Release();
        service->Release();
        error.Set(L"Could not create the startup task definition.");
        return false;
    }

    IRegistrationInfo* info = nullptr;
    if (SUCCEEDED(task->get_RegistrationInfo(&info)) && info) {
        BStr author(L"Latenci");
        info->put_Author(author.value);
        info->Release();
    }

    BStr user(userId);
    IPrincipal* principal = nullptr;
    if (SUCCEEDED(task->get_Principal(&principal)) && principal) {
        principal->put_UserId(user.value);
        principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
        principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        principal->Release();
    }

    ITaskSettings* settings = nullptr;
    if (SUCCEEDED(task->get_Settings(&settings)) && settings) {
        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        settings->put_AllowDemandStart(VARIANT_TRUE);
        settings->put_Enabled(VARIANT_TRUE);
        settings->put_Hidden(VARIANT_FALSE);
        BStr noLimit(L"PT0S");
        settings->put_ExecutionTimeLimit(noLimit.value);
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        settings->Release();
    }

    ITriggerCollection* triggers = nullptr;
    hr = task->get_Triggers(&triggers);
    if (FAILED(hr) || !triggers) {
        task->Release();
        root->Release();
        service->Release();
        error.Set(L"Could not configure the logon trigger.");
        return false;
    }
    ITrigger* trigger = nullptr;
    hr = triggers->Create(TASK_TRIGGER_LOGON, &trigger);
    triggers->Release();
    if (FAILED(hr) || !trigger) {
        task->Release();
        root->Release();
        service->Release();
        error.Set(L"Could not create the logon trigger.");
        return false;
    }

    // Delay past explorer/shell init so the tray icon can register.
    ILogonTrigger* logon = nullptr;
    if (SUCCEEDED(trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(&logon)))
        && logon) {
        logon->put_UserId(user.value);
        BStr delay(L"PT30S");
        logon->put_Delay(delay.value);
        logon->put_Enabled(VARIANT_TRUE);
        logon->Release();
    }
    trigger->Release();

    IActionCollection* actions = nullptr;
    hr = task->get_Actions(&actions);
    if (FAILED(hr) || !actions) {
        task->Release();
        root->Release();
        service->Release();
        error.Set(L"Could not configure the startup action.");
        return false;
    }
    IAction* action = nullptr;
    hr = actions->Create(TASK_ACTION_EXEC, &action);
    actions->Release();
    if (FAILED(hr) || !action) {
        task->Release();
        root->Release();
        service->Release();
        error.Set(L"Could not create the startup action.");
        return false;
    }
    IExecAction* exec = nullptr;
    hr = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&exec));
    action->Release();
    if (FAILED(hr) || !exec) {
        task->Release();
        root->Release();
        service->Release();
        error.Set(L"Could not configure the executable path.");
        return false;
    }
    BStr path(exePath);
    BStr dir(workDir);
    exec->put_Path(path.value);
    exec->put_WorkingDirectory(dir.value);
    exec->Release();

    VARIANT empty = EmptyVariant();
    VARIANT userVar;
    VariantInit(&userVar);
    userVar.vt = VT_BSTR;
    userVar.bstrVal = SysAllocString(userId);

    IRegisteredTask* registered = nullptr;
    hr = root->RegisterTaskDefinition(
        taskName.value,
        task,
        TASK_CREATE_OR_UPDATE,
        userVar,
        empty,
        TASK_LOGON_INTERACTIVE_TOKEN,
        empty,
        &registered);
    VariantClear(&userVar);
    if (registered) {
        registered->Release();
    }
    task->Release();
    root->Release();
    service->Release();

    if (FAILED(hr)) {
        error.Set(L"Could not register the startup task.");
        return false;
    }
    g_cache = CacheState::Enabled;
    return true;
}
