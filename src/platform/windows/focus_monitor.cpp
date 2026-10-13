/**
 * @file src/platform/windows/focus_monitor.cpp
 * @brief Watches what kind of text field has focus on the Windows desktop.
 *
 * A WinEvent hook on EVENT_OBJECT_FOCUS is the trigger: event driven, out of process and
 * free of COM, so nothing is polled. Once focus settles, a classic edit control is
 * classified from its window style alone; anything else (browsers, Electron, WPF, UWP) goes
 * through UI Automation.
 *
 * All of it runs on one dedicated thread. That is not tidiness: GetFocusedElement() has been
 * measured blocking for over a second against an application with a busy UI thread, so it
 * must never run on the capture, encode or control threads. The thread only hands its result
 * to a callback that queues it for the control thread, so a blocked call delays a hint and
 * nothing else. IUIAutomation2 timeouts bound those calls as well.
 *
 * Based on drunkitguy/apollo2-vibe 306f8f42 (GPLv3).
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

// platform includes
#include <windows.h>

// uiautomation.h expects windows.h to come first
#include <uiautomation.h>

// local includes
#include "src/focus_hints.h"
#include "src/logging.h"
#include "src/utility.h"

using namespace std::literals;

namespace focus_hints {

  static_assert(edit_style::password == ES_PASSWORD);
  static_assert(edit_style::read_only == ES_READONLY);
  static_assert(edit_style::number == ES_NUMBER);
  static_assert(control_type::combo_box == UIA_ComboBoxControlTypeId);
  static_assert(control_type::edit == UIA_EditControlTypeId);
  static_assert(control_type::spinner == UIA_SpinnerControlTypeId);
  static_assert(control_type::document == UIA_DocumentControlTypeId);

  namespace {

    /// Bound on one UI Automation call. The thread can afford to wait; this keeps it from
    /// waiting on a hung application for the default 20 seconds.
    constexpr DWORD uia_timeout_ms = 500;

    /// Longest the loop sleeps, so a stop request that raced the thread's start still lands.
    constexpr auto idle_wait = 250ms;

    /// Guards `monitor_thread` against one session starting while another stops.
    std::mutex monitor_mutex;
    std::thread monitor_thread;
    std::atomic<bool> shutdown_requested {false};
    std::atomic<DWORD> monitor_thread_id {0};

    /// Set by the hook and consumed by the loop, both on the detector thread.
    bool focus_event_pending = false;

    void CALLBACK on_focus_event(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
      // Only note it. Classifying here would put a UI Automation call, which can block for
      // seconds, inside the hook callback.
      focus_event_pending = true;
    }

    /**
     * @brief Classify the focused window if it is a classic edit control.
     * @return Nothing when it is not one, and UI Automation has to be asked instead.
     */
    std::optional<kind_t> classify_from_window_style() {
      GUITHREADINFO info {};
      info.cbSize = sizeof(info);
      if (!GetGUIThreadInfo(0, &info) || !info.hwndFocus) {
        return std::nullopt;
      }

      wchar_t class_name[64] {};
      if (!GetClassNameW(info.hwndFocus, class_name, _countof(class_name))) {
        return std::nullopt;
      }

      // The style bits only mean what classify_edit_style() expects on the stock classes
      if (_wcsicmp(class_name, L"Edit") != 0 && _wcsnicmp(class_name, L"RichEdit", 8) != 0) {
        return std::nullopt;
      }

      return classify_edit_style(static_cast<std::uint32_t>(GetWindowLongPtrW(info.hwndFocus, GWL_STYLE)));
    }

    kind_t classify_from_automation(IUIAutomation *uia) {
      IUIAutomationElement *element = nullptr;
      if (FAILED(uia->GetFocusedElement(&element)) || !element) {
        return kind_t::none;
      }
      auto element_guard = util::fail_guard([element]() {
        element->Release();
      });

      automation_facts_t facts;

      CONTROLTYPEID type = 0;
      if (SUCCEEDED(element->get_CurrentControlType(&type))) {
        facts.control_type = type;
      }

      BOOL is_password = FALSE;
      if (SUCCEEDED(element->get_CurrentIsPassword(&is_password))) {
        facts.is_password = is_password != FALSE;
      }

      // Each property is a round trip into the focused application, so read-only is only
      // asked of the controls whose classification depends on it.
      if (type == UIA_EditControlTypeId || type == UIA_DocumentControlTypeId || type == UIA_ComboBoxControlTypeId || type == UIA_SpinnerControlTypeId) {
        IUIAutomationValuePattern *value = nullptr;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern), reinterpret_cast<void **>(&value))) && value) {
          facts.has_value_pattern = true;

          BOOL read_only = FALSE;
          if (SUCCEEDED(value->get_CurrentIsReadOnly(&read_only))) {
            facts.value_read_only = read_only != FALSE;
          }
          value->Release();
        }
      }

      return classify_automation(facts);
    }

    kind_t classify_focus(IUIAutomation *uia) {
      // The cheap path first: when it answers, UI Automation is never consulted, and it is
      // the only path that can tell a numeric field apart.
      if (auto kind = classify_from_window_style()) {
        return *kind;
      }
      return classify_from_automation(uia);
    }

    IUIAutomation *create_automation() {
      // Prefer the interface that can bound a blocking call
      IUIAutomation2 *uia2 = nullptr;
      if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation2), reinterpret_cast<void **>(&uia2))) && uia2) {
        uia2->put_ConnectionTimeout(uia_timeout_ms);
        uia2->put_TransactionTimeout(uia_timeout_ms);
        return uia2;
      }

      IUIAutomation *uia = nullptr;
      if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), reinterpret_cast<void **>(&uia))) && uia) {
        return uia;
      }
      return nullptr;
    }

    /**
     * @brief The detector thread.
     * @details Owns the COM apartment, the UI Automation client, the hook and the message
     *          loop that delivers it. Other threads only touch `shutdown_requested`.
     */
    void monitor_loop(const callback_t on_change) {
      // Create the message queue before publishing the thread id, so a stop request posted
      // right away is not lost.
      MSG msg;
      PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
      monitor_thread_id = GetCurrentThreadId();

      if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        BOOST_LOG(error) << "Focus hints: CoInitializeEx() failed"sv;
        return;
      }
      auto com_guard = util::fail_guard([]() {
        CoUninitialize();
      });

      auto uia = create_automation();
      if (!uia) {
        BOOST_LOG(error) << "Focus hints: couldn't create a UI Automation client"sv;
        return;
      }
      auto uia_guard = util::fail_guard([uia]() {
        uia->Release();
      });

      // Out of context keeps our code out of other processes; skipping our own process keeps
      // the host's own windows from producing hints.
      auto hook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, on_focus_event, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
      if (!hook) {
        BOOST_LOG(error) << "Focus hints: SetWinEventHook() failed: "sv << GetLastError();
        return;
      }
      auto hook_guard = util::fail_guard([hook]() {
        UnhookWinEvent(hook);
      });

      BOOST_LOG(info) << "Focus hints: watching for text field focus"sv;

      debouncer_t debouncer;
      focus_event_pending = false;

      // Classify what already has focus, so a stream that starts in a text field gets its
      // keyboard without the user having to click away and back.
      debouncer.focus_changed(debouncer_t::clock::now());

      while (!shutdown_requested.load(std::memory_order_acquire)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          DispatchMessageW(&msg);
        }

        if (focus_event_pending) {
          focus_event_pending = false;
          debouncer.focus_changed(debouncer_t::clock::now());
        }

        if (debouncer.classify_due(debouncer_t::clock::now())) {
          const auto kind = classify_focus(uia);
          if (debouncer.classified(kind, debouncer_t::clock::now())) {
            BOOST_LOG(debug) << "Focus hints: "sv << to_string(kind);
            on_change(kind);
          }
        }

        // The hook delivers through the message queue, so wait on it rather than spinning,
        // and wake up again when a pending event is due for classification.
        const auto wait = std::min<debouncer_t::clock::duration>(debouncer.time_until_due(debouncer_t::clock::now()).value_or(idle_wait), idle_wait);
        const auto wait_ms = std::chrono::ceil<std::chrono::milliseconds>(wait).count();
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(wait_ms), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      }
    }

  }  // namespace

  bool start_monitor(callback_t on_change) {
    std::lock_guard lock {monitor_mutex};
    if (monitor_thread.joinable()) {
      // One detector serves the host. A second session would otherwise stand up a second UI
      // Automation client for the same desktop.
      BOOST_LOG(info) << "Focus hints: already watching for another session"sv;
      return false;
    }

    shutdown_requested = false;
    monitor_thread_id = 0;
    monitor_thread = std::thread {monitor_loop, std::move(on_change)};
    return true;
  }

  void stop_monitor() {
    std::lock_guard lock {monitor_mutex};
    if (!monitor_thread.joinable()) {
      return;
    }

    shutdown_requested.store(true, std::memory_order_release);

    // Wake the loop if it is parked in MsgWaitForMultipleObjectsEx. If the thread has not
    // published its id yet, it will see the request within idle_wait.
    if (const auto thread_id = monitor_thread_id.load()) {
      PostThreadMessageW(thread_id, WM_NULL, 0, 0);
    }

    monitor_thread.join();
    monitor_thread_id = 0;
  }

}  // namespace focus_hints
