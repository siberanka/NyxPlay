/**
 * @file src/text_input.cpp
 * @brief Detection of focused text input fields on the host (NyxPlay protocol extension).
 */
// standard includes
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

// local includes
#include "logging.h"
#include "text_input.h"
#include "utility.h"

#ifdef _WIN32
  // platform includes
  #include <windows.h>
  #include <ole2.h>
  #include <uiautomation.h>

  // local includes
  #include "platform/windows/misc.h"
#endif

using namespace std::literals;

namespace text_input {
  namespace {
    std::mutex state_mutex;
    state_t published_state;

    std::mutex monitor_mutex;
    std::weak_ptr<void> monitor_guard;
  }  // namespace

  state_t classify(const element_info_t &info) {
    state_t state;

    if (!info.enabled) {
      return state;
    }

    const bool explicitly_editable = info.value_read_only.has_value() && !*info.value_read_only;
    const bool explicitly_read_only = info.value_read_only.value_or(false);

    switch (info.control_type) {
      case control_type::edit:
        // Edit controls are text boxes unless they announce themselves as read-only.
        state.active = !explicitly_read_only;
        break;
      case control_type::document:
        // Rich editors, browsers' content editable regions, Word, new Notepad...
        // A read-only value pattern marks a viewer (e.g. the page of a web browser).
        state.active = explicitly_editable || (!info.value_read_only.has_value() && info.has_text_pattern && info.keyboard_focusable);
        if (state.active) {
          state.flags |= FLAG_MULTILINE;
        }
        break;
      case control_type::combo_box:
        // Only editable combo boxes accept free text.
        state.active = explicitly_editable;
        break;
      case control_type::custom:
      case control_type::pane:
      case control_type::group:
        // Custom widgets (game launchers, Electron apps...) occasionally expose an
        // editable value pattern on generic containers.
        state.active = explicitly_editable && info.keyboard_focusable;
        break;
      default:
        state.active = false;
        break;
    }

    if (info.is_password && (state.active || info.control_type == control_type::edit)) {
      state.active = true;
      state.flags |= FLAG_PASSWORD;
    }

    if (!state.active) {
      state.flags = FLAG_NONE;
    }

    return state;
  }

  state_t current() {
    std::lock_guard lg {state_mutex};
    return published_state;
  }

  bool publish(bool active, std::uint8_t flags) {
    if (!active) {
      flags = FLAG_NONE;
    }

    std::lock_guard lg {state_mutex};
    if (published_state.generation != 0 && published_state.active == active && published_state.flags == flags) {
      return false;
    }

    published_state.active = active;
    published_state.flags = flags;
    // Never wrap back to the "unknown" generation 0
    published_state.generation = published_state.generation == UINT32_MAX ? 1 : published_state.generation + 1;

    BOOST_LOG(debug) << "Text input focus changed: "sv << (active ? "active"sv : "inactive"sv)
                     << " (flags: "sv << (int) flags << ", generation: "sv << published_state.generation << ')';
    return true;
  }

  payload_t to_payload(const state_t &state) {
    payload_t payload {};
    payload.version = PAYLOAD_VERSION;
    payload.active = state.active ? 1 : 0;
    payload.flags = state.active ? state.flags : FLAG_NONE;
    payload.reserved = 0;
    return payload;
  }

  std::optional<state_t> from_payload(std::string_view payload) {
    if (payload.size() < 2) {
      return std::nullopt;
    }

    const auto version = (std::uint8_t) payload[0];
    if (version == 0) {
      return std::nullopt;
    }

    state_t state;
    state.active = payload[1] != 0;
    state.flags = payload.size() >= 3 && state.active ? (std::uint8_t) payload[2] : FLAG_NONE;
    return state;
  }

#ifdef _WIN32
  namespace {
    // UI Automation identifiers (kept local to avoid depending on libuuid exports)
    const CLSID clsid_cuiautomation = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
    const IID iid_iuiautomation = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
    const IID iid_focus_changed_handler = {0xc270f6b5, 0x5c69, 0x4290, {0x97, 0x45, 0x7a, 0x7f, 0x97, 0x16, 0x94, 0x68}};
    const IID iid_value_pattern = {0xa94cd8b1, 0x0844, 0x4cd6, {0x9d, 0x2d, 0x64, 0x05, 0x37, 0xab, 0x39, 0xe9}};

    constexpr PATTERNID value_pattern_id = 10002;
    constexpr PATTERNID text_pattern_id = 10014;

    /// Inactive states are delayed a little so focus hopping (e.g. edit -> suggestion popup) does not flicker.
    constexpr auto inactive_grace_period = 250ms;
    /// How often the input desktop is checked (UAC prompts, lock screen, user switching).
    constexpr auto desktop_check_interval = 1s;
    /// Poll interval for the caret fallback used when UI Automation is unavailable.
    constexpr auto caret_poll_interval = 200ms;

    element_info_t describe(IUIAutomationElement *element) {
      element_info_t info;

      CONTROLTYPEID control_type = 0;
      if (SUCCEEDED(element->get_CurrentControlType(&control_type))) {
        info.control_type = control_type;
      }

      BOOL flag = FALSE;
      if (SUCCEEDED(element->get_CurrentIsEnabled(&flag))) {
        info.enabled = flag != FALSE;
      }
      if (SUCCEEDED(element->get_CurrentIsKeyboardFocusable(&flag))) {
        info.keyboard_focusable = flag != FALSE;
      }
      if (SUCCEEDED(element->get_CurrentIsPassword(&flag))) {
        info.is_password = flag != FALSE;
      }

      IUIAutomationValuePattern *value_pattern = nullptr;
      if (SUCCEEDED(element->GetCurrentPatternAs(value_pattern_id, iid_value_pattern, (void **) &value_pattern)) && value_pattern) {
        BOOL read_only = TRUE;
        if (SUCCEEDED(value_pattern->get_CurrentIsReadOnly(&read_only))) {
          info.value_read_only = read_only != FALSE;
        }
        value_pattern->Release();
      }

      IUnknown *text_pattern = nullptr;
      if (SUCCEEDED(element->GetCurrentPattern(text_pattern_id, &text_pattern)) && text_pattern) {
        info.has_text_pattern = true;
        text_pattern->Release();
      }

      return info;
    }

    std::wstring input_desktop_name() {
      auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
      if (!desktop) {
        return {};
      }

      wchar_t name[256] {};
      DWORD needed = 0;
      std::wstring result;
      if (GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name) - sizeof(wchar_t), &needed)) {
        result = name;
      }
      CloseDesktop(desktop);
      return result;
    }

    class monitor_t;

    /**
     * @brief COM sink receiving UI Automation focus change events.
     */
    class focus_handler_t: public IUIAutomationFocusChangedEventHandler {
    public:
      explicit focus_handler_t(monitor_t *monitor):
          _monitor {monitor} {
      }

      ULONG STDMETHODCALLTYPE AddRef() override {
        return ++_refs;
      }

      ULONG STDMETHODCALLTYPE Release() override {
        auto refs = --_refs;
        if (refs == 0) {
          delete this;
        }
        return refs;
      }

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
        if (!object) {
          return E_POINTER;
        }

        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, iid_focus_changed_handler)) {
          *object = static_cast<IUIAutomationFocusChangedEventHandler *>(this);
          AddRef();
          return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
      }

      HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement *sender) override;

      /**
       * @brief Stop forwarding events to the monitor and wait for callbacks that are already running.
       * @note After this returns the monitor may be destroyed safely, even if UI Automation still
       *       delivers late events on its worker threads.
       */
      void detach() {
        _monitor.store(nullptr);
        while (_inflight.load() > 0) {
          std::this_thread::yield();
        }
      }

    private:
      virtual ~focus_handler_t() = default;

      std::atomic<ULONG> _refs {1};
      std::atomic<monitor_t *> _monitor;
      std::atomic<int> _inflight {0};
    };

    /**
     * @brief Background thread owning the UI Automation registration.
     */
    class monitor_t {
    public:
      monitor_t() {
        _wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        _thread = std::thread {&monitor_t::run, this};
      }

      ~monitor_t() {
        _stop.store(true);
        if (_wake) {
          SetEvent(_wake);
        }
        if (_thread.joinable()) {
          _thread.join();
        }
        if (_wake) {
          CloseHandle(_wake);
        }

        // Nobody is listening anymore, forget the last focus target.
        publish(false, FLAG_NONE);
      }

      monitor_t(const monitor_t &) = delete;
      monitor_t &operator=(const monitor_t &) = delete;

      void on_focus(const state_t &state) {
        {
          std::lock_guard lg {_pending_mutex};
          _pending = state;
          _pending_since = std::chrono::steady_clock::now();
        }
        SetEvent(_wake);
      }

    private:
      bool register_uia() {
        if (!_automation) {
          auto hr = CoCreateInstance(clsid_cuiautomation, nullptr, CLSCTX_INPROC_SERVER, iid_iuiautomation, (void **) &_automation);
          if (FAILED(hr) || !_automation) {
            BOOST_LOG(warning) << "Text input monitor: UI Automation unavailable [0x"sv << util::hex(hr).to_string_view() << "], using caret fallback"sv;
            _automation = nullptr;
            return false;
          }
        }

        _handler = new focus_handler_t {this};
        auto hr = _automation->AddFocusChangedEventHandler(nullptr, _handler);
        if (FAILED(hr)) {
          BOOST_LOG(warning) << "Text input monitor: failed to subscribe to focus changes [0x"sv << util::hex(hr).to_string_view() << ']';
          _handler->detach();
          _handler->Release();
          _handler = nullptr;
          return false;
        }

        evaluate_focused_element();
        return true;
      }

      void unregister_uia() {
        if (_handler) {
          // Detach first so late events can no longer reach this monitor
          _handler->detach();
        }
        if (_automation && _handler) {
          _automation->RemoveFocusChangedEventHandler(_handler);
        }
        if (_handler) {
          _handler->Release();
          _handler = nullptr;
        }
      }

      void release_uia() {
        unregister_uia();
        if (_automation) {
          _automation->Release();
          _automation = nullptr;
        }
      }

      void evaluate_focused_element() {
        if (!_automation) {
          return;
        }

        IUIAutomationElement *element = nullptr;
        if (SUCCEEDED(_automation->GetFocusedElement(&element)) && element) {
          on_focus(classify(describe(element)));
          element->Release();
        } else {
          on_focus(state_t {});
        }
      }

      void poll_caret() {
        GUITHREADINFO info {};
        info.cbSize = sizeof(info);
        bool active = false;
        if (GetGUIThreadInfo(0, &info)) {
          active = info.hwndCaret != nullptr;
        }

        state_t state;
        state.active = active;
        on_focus(state);
      }

      void flush_pending() {
        std::optional<state_t> pending;
        std::chrono::steady_clock::time_point since;
        {
          std::lock_guard lg {_pending_mutex};
          pending = _pending;
          since = _pending_since;
        }

        if (!pending) {
          return;
        }

        if (!pending->active && std::chrono::steady_clock::now() - since < inactive_grace_period) {
          // Wait for the grace period before announcing that the field lost focus.
          return;
        }

        {
          std::lock_guard lg {_pending_mutex};
          if (_pending && _pending_since == since) {
            _pending.reset();
          }
        }

        publish(pending->active, pending->flags);
      }

      void run() {
        auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com_initialized = SUCCEEDED(hr);

        platf::syncThreadDesktop();
        auto desktop = input_desktop_name();
        bool uia_ok = register_uia();

        BOOST_LOG(info) << "Text input monitor started ("sv << (uia_ok ? "UI Automation"sv : "caret fallback"sv) << ')';

        auto next_desktop_check = std::chrono::steady_clock::now() + desktop_check_interval;
        auto next_caret_poll = std::chrono::steady_clock::now();

        while (!_stop.load()) {
          WaitForSingleObject(_wake, 50);
          if (_stop.load()) {
            break;
          }

          auto now = std::chrono::steady_clock::now();

          if (now >= next_desktop_check) {
            next_desktop_check = now + desktop_check_interval;

            auto current_desktop = input_desktop_name();
            if (!current_desktop.empty() && current_desktop != desktop) {
              BOOST_LOG(debug) << "Text input monitor: input desktop changed, re-subscribing"sv;
              desktop = current_desktop;
              unregister_uia();
              platf::syncThreadDesktop();
              uia_ok = register_uia();
            } else if (!uia_ok) {
              // Retry periodically, UI Automation may become available later (e.g. after login)
              uia_ok = register_uia();
            }
          }

          if (!uia_ok && now >= next_caret_poll) {
            next_caret_poll = now + caret_poll_interval;
            poll_caret();
          }

          flush_pending();
        }

        release_uia();
        if (com_initialized) {
          CoUninitialize();
        }

        BOOST_LOG(info) << "Text input monitor stopped"sv;
      }

      HANDLE _wake = nullptr;
      std::thread _thread;
      std::atomic<bool> _stop {false};

      IUIAutomation *_automation = nullptr;
      focus_handler_t *_handler = nullptr;

      std::mutex _pending_mutex;
      std::optional<state_t> _pending;
      std::chrono::steady_clock::time_point _pending_since;
    };

    HRESULT STDMETHODCALLTYPE focus_handler_t::HandleFocusChangedEvent(IUIAutomationElement *sender) {
      if (!sender) {
        return S_OK;
      }

      // Inspect the element before touching the monitor (cross process UIA calls can be slow)
      auto state = classify(describe(sender));

      ++_inflight;
      if (auto monitor = _monitor.load()) {
        monitor->on_focus(state);
      }
      --_inflight;
      return S_OK;
    }
  }  // namespace

  std::optional<state_t> classify_window(void *hwnd) {
    auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_initialized = SUCCEEDED(hr);

    std::optional<state_t> result;
    IUIAutomation *automation = nullptr;
    if (SUCCEEDED(CoCreateInstance(clsid_cuiautomation, nullptr, CLSCTX_INPROC_SERVER, iid_iuiautomation, (void **) &automation)) && automation) {
      IUIAutomationElement *element = nullptr;
      if (SUCCEEDED(automation->ElementFromHandle((UIA_HWND) hwnd, &element)) && element) {
        result = classify(describe(element));
        element->Release();
      }
      automation->Release();
    }

    if (com_initialized) {
      CoUninitialize();
    }
    return result;
  }

  std::shared_ptr<void> acquire() {
    std::lock_guard lg {monitor_mutex};
    if (auto existing = monitor_guard.lock()) {
      return existing;
    }

    auto monitor = std::make_shared<monitor_t>();
    monitor_guard = monitor;
    return monitor;
  }
#else
  std::shared_ptr<void> acquire() {
    // No focus monitor is implemented for this platform yet.
    std::lock_guard lg {monitor_mutex};
    if (auto existing = monitor_guard.lock()) {
      return existing;
    }

    auto guard = std::make_shared<int>(0);
    monitor_guard = guard;
    return guard;
  }
#endif
}  // namespace text_input
