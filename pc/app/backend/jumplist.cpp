#include "jumplist.h"

#include <QCoreApplication>
#include <QDir>
#include <QtDebug>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <objectarray.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace {

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

// One task: a shell link to NyxPlay.exe with the given arguments
bool makeTask(const std::wstring& exe, const std::wstring& workDir, const wchar_t* arguments,
              const QString& title, IShellLinkW** out)
{
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        return false;
    }
    link->SetPath(exe.c_str());
    link->SetArguments(arguments);
    link->SetWorkingDirectory(workDir.c_str());
    link->SetIconLocation(exe.c_str(), 0);

    ComPtr<IPropertyStore> store;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(&store)))) {
        return false;
    }
    PROPVARIANT value;
    const std::wstring text = title.toStdWString();
    if (FAILED(InitPropVariantFromString(text.c_str(), &value))) {
        return false;
    }
    HRESULT hr = store->SetValue(PKEY_Title, value);
    PropVariantClear(&value);
    if (FAILED(hr) || FAILED(store->Commit())) {
        return false;
    }
    *out = link.p;
    link.p = nullptr;
    return true;
}

}
#endif

namespace JumpList {

bool update(const QString& pendingVersion)
{
#ifdef Q_OS_WIN
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = SUCCEEDED(init);

    bool ok = false;
    {
        const std::wstring exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
        const std::wstring dir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toStdWString();

        ComPtr<ICustomDestinationList> list;
        ComPtr<IObjectCollection> tasks;
        if (SUCCEEDED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list))) &&
                SUCCEEDED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tasks)))) {
            UINT minSlots = 0;
            ComPtr<IObjectArray> removed;
            if (SUCCEEDED(list->BeginList(&minSlots, IID_PPV_ARGS(&removed)))) {
                ok = true;
                if (!pendingVersion.isEmpty()) {
                    IShellLinkW* install = nullptr;
                    if (makeTask(exe, dir, L"--install-update",
                                 QCoreApplication::translate("JumpList", "Install update %1").arg(pendingVersion), &install)) {
                        tasks->AddObject(install);
                        install->Release();
                    }
                }
                IShellLinkW* check = nullptr;
                if (makeTask(exe, dir, L"--check-update",
                             QCoreApplication::translate("JumpList", "Check for updates"), &check)) {
                    tasks->AddObject(check);
                    check->Release();
                }

                ComPtr<IObjectArray> array;
                ok = SUCCEEDED(tasks->QueryInterface(IID_PPV_ARGS(&array))) &&
                     SUCCEEDED(list->AddUserTasks(array.p)) &&
                     SUCCEEDED(list->CommitList());
                if (!ok) {
                    list->AbortList();
                }
            }
        }
    }

    if (uninit) {
        CoUninitialize();
    }
    if (!ok) {
        qWarning() << "The jump list could not be updated";
    }
    return ok;
#else
    Q_UNUSED(pendingVersion);
    return false;
#endif
}

}
