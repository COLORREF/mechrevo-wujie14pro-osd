#pragma once
#include <wbemidl.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <iterator>
#include <mutex>
#include <utility>
namespace HotkeyInput
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        constexpr const wchar_t *kClasses[] = {
            L"HID_EVENT20", L"HID_EVENT21", L"HID_EVENT22", L"HID_EVENT23"
        };
        constexpr DWORD kPollMs = 500;
        constexpr ULONGLONG kRebuildMinGapMs = 5000;
        constexpr UINT kLockDelayMs = 45;
    }
}

#include <windows.h>
#include <memory>

namespace HotkeyInput
{

    inline constexpr UINT kWmiEventMessage = WM_APP + 1;
    inline constexpr UINT kWmiErrorMessage = WM_APP + 4;
    inline constexpr UINT_PTR kLockTimer = 100;

    enum class WmiError : WPARAM
    {
        Start, CreateLocator, Connect
    };

    class WmiWatcher
    {
    public:
        using Logger = void (*)(const wchar_t*, ...);

        WmiWatcher() = default;
        ~WmiWatcher()
        {
            if (m_thread)
            {
                m_state->Stop();
                WaitForSingleObject(m_thread, INFINITE);
                CloseHandle(m_thread);
            }
        }
        WmiWatcher(const WmiWatcher&) = delete;
        WmiWatcher& operator=(const WmiWatcher&) = delete;

        bool Start(HWND target, Logger log)
        {
            m_state = std::make_shared<State>(target, log);
            if (m_state->stop)
                m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
            return m_thread != nullptr;
        }
        void RequestRebuild()
        {
            if (m_state)
                m_state->rebuildRequested = true;
        }

    private:
        struct State
        {
            State(HWND target, Logger logger)
                : stop(CreateEventW(nullptr, TRUE, FALSE, nullptr)), log(logger), window(target)
            {
            }
    
            ~State()
            {
                if (stop)
                    CloseHandle(stop);
            }
    
            void Post(UINT message, WPARAM wp, LPARAM lp)
            {
                const std::lock_guard<std::mutex> lock(windowMutex);
                if (window)
                    PostMessageW(window, message, wp, lp);
            }
    
            void Stop()
            {
                const std::lock_guard<std::mutex> lock(windowMutex);
                window = nullptr;
                SetEvent(stop);
            }
    
            const HANDLE stop;
            const Logger log;
            std::atomic_bool rebuildRequested{false};
    
        private:
            std::mutex windowMutex;
            HWND window;
        };
        class EventSink final : public IWbemObjectSink
        {
        public:
            explicit EventSink(const std::shared_ptr<State> &state) : m_state(state) {}
    
            STDMETHODIMP QueryInterface(REFIID riid, void **object) override
            {
                if (!object)
                    return E_POINTER;
                if (riid == IID_IUnknown || riid == IID_IWbemObjectSink)
                {
                    *object = static_cast<IWbemObjectSink *>(this);
                    AddRef();
                    return S_OK;
                }
                *object = nullptr;
                return E_NOINTERFACE;
            }
    
            STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    
            STDMETHODIMP_(ULONG) Release() override
            {
                const ULONG refs = --m_refs;
                if (refs == 0)
                    delete this;
                return refs;
            }
    
            STDMETHODIMP Indicate(LONG count, IWbemClassObject **objects) override
            {
                for (LONG i = 0; i < count; ++i)
                {
                    if (objects[i])
                        Report(*objects[i]);
                }
                return WBEM_S_NO_ERROR;
            }
    
            STDMETHODIMP SetStatus(LONG, HRESULT hr, BSTR, IWbemClassObject *) override
            {
                if (FAILED(hr) && hr != WBEM_E_CALL_CANCELLED)
                {
                    m_state->rebuildRequested = true;
                    m_state->log(L"SetStatus hr=0x%08X，请求重建订阅", static_cast<unsigned>(hr));
                }
                return WBEM_S_NO_ERROR;
            }
    
        private:
            void Report(IWbemClassObject &object)
            {
                VARIANT detail;
                VariantInit(&detail);
                if (SUCCEEDED(object.Get(L"EventDetail", 0, &detail, nullptr, nullptr)) &&
                    detail.vt == (VT_ARRAY | VT_UI1) && detail.parray &&
                    SafeArrayGetDim(detail.parray) == 1)
                {
                    LONG first = 0;
                    LONG last = 0;
                    void *bytes = nullptr;
                    if (SUCCEEDED(SafeArrayGetLBound(detail.parray, 1, &first)) &&
                        SUCCEEDED(SafeArrayGetUBound(detail.parray, 1, &last)) &&
                        static_cast<LONGLONG>(last) - first >= 2 &&
                        SUCCEEDED(SafeArrayAccessData(detail.parray, &bytes)))
                    {
    
                        const auto *payload = static_cast<const BYTE *>(bytes);
                        const BYTE code = payload[1];
                        const BYTE value = payload[2];
                        SafeArrayUnaccessData(detail.parray);
                        m_state->Post(kWmiEventMessage, code, value);
                    }
                }
                VariantClear(&detail);
            }
    
            std::atomic<ULONG> m_refs{1};
            const std::shared_ptr<State> m_state;
        };
        static DWORD WINAPI ThreadProc(void* context)
        {
            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
                return 0;
            CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                                 RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
            Run(static_cast<WmiWatcher *>(context)->m_state);
            CoUninitialize();
            return 0;
        }
        static void Run(const std::shared_ptr<State>& state)
        {
            ComPtr<IWbemLocator> locator;
            HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(locator.GetAddressOf()));
            if (FAILED(hr))
            {
                state->Post(kWmiErrorMessage, static_cast<WPARAM>(WmiError::CreateLocator), hr);
                return;
            }
        
            ComPtr<IWbemServices> service;
            BSTR root = SysAllocString(L"ROOT\\WMI");
            hr = root ? locator->ConnectServer(root, nullptr, nullptr, nullptr,
                                              WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr,
                                              service.GetAddressOf())
                      : E_OUTOFMEMORY;
            SysFreeString(root);
            if (FAILED(hr))
            {
                state->Post(kWmiErrorMessage, static_cast<WPARAM>(WmiError::Connect), hr);
                return;
            }
            CoSetProxyBlanket(service.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                              RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        
            std::array<ComPtr<IWbemObjectSink>, std::size(kClasses)> sinks;
            const auto cancel = [&] {
                for (auto &sink : sinks)
                {
                    if (sink)
                    {
                        service->CancelAsyncCall(sink.Get());
                        sink.Reset();
                    }
                }
            };
            const auto subscribe = [&] {
                int count = 0;
                BSTR language = SysAllocString(L"WQL");
                for (size_t i = 0; language && i < sinks.size(); ++i)
                {
                    if (WaitForSingleObject(state->stop, 0) != WAIT_TIMEOUT)
                        break;
                    const std::wstring queryText = std::wstring(L"SELECT * FROM ") + kClasses[i];
                    BSTR query = SysAllocString(queryText.c_str());
                    if (!query)
                        continue;
                    sinks[i].Attach(new EventSink(state));
                    const HRESULT result = service->ExecNotificationQueryAsync(
                        language, query, WBEM_FLAG_SEND_STATUS, nullptr, sinks[i].Get());
                    SysFreeString(query);
                    if (SUCCEEDED(result))
                        ++count;
                    else
                        sinks[i].Reset();
                }
                SysFreeString(language);
                state->log(L"订阅完成：%d/4 个事件类", count);
                return count > 0;
            };
        
            if (!subscribe())
                return;
            state->log(L"WMI 订阅建立完成");
        
            ULONGLONG nextRebuild = 0;
            while (WaitForSingleObject(state->stop, kPollMs) == WAIT_TIMEOUT)
            {
                const ULONGLONG now = GetTickCount64();
                if (now < nextRebuild || !state->rebuildRequested.exchange(false))
                    continue;
                nextRebuild = now + kRebuildMinGapMs;
                state->log(L"开始重建 WMI 订阅");
                cancel();
                if (!subscribe())
                    state->log(L"重建失败，等下一次机会");
            }
            cancel();
        }

        HANDLE m_thread = nullptr;
        std::shared_ptr<State> m_state;
    };

    class LockKeys
    {
    public:
        LockKeys(HWND target, HINSTANCE instance)
            : m_target(target), m_instance(instance)
        {
            s_current = this;
        }
        ~LockKeys()
        {
            if (m_hook)
                UnhookWindowsHookEx(m_hook);
            s_current = nullptr;
        }
        LockKeys(const LockKeys&) = delete;
        LockKeys& operator=(const LockKeys&) = delete;

        bool Reinstall()
        {
            if (m_hook)
                UnhookWindowsHookEx(m_hook);
            m_hook = SetWindowsHookExW(WH_KEYBOARD_LL, HookProc, m_instance, 0);
            return m_hook != nullptr;
        }
        UINT TakePendingKey()
        {
            KillTimer(m_target, kLockTimer);
            return std::exchange(m_pendingKey, 0u);
        }

    private:
        static LRESULT CALLBACK HookProc(int code, WPARAM wp, LPARAM lp)
        {
            if (s_current && code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN))
            {
                const auto *key = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lp);
                if (key->vkCode == VK_CAPITAL || key->vkCode == VK_NUMLOCK)
                {
                    s_current->m_pendingKey = key->vkCode;
        
                    SetTimer(s_current->m_target, kLockTimer, kLockDelayMs, nullptr);
                }
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        inline static thread_local LockKeys* s_current = nullptr;

        HWND m_target;
        HINSTANCE m_instance;
        HHOOK m_hook = nullptr;
        UINT m_pendingKey = 0;
    };
}
