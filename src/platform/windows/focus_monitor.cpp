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
 * nothing else. IUIAutomation2 timeouts bound those calls as well, and the detector refuses
 * to run without them, so stopping it never waits on a hung application for long.
 *
 * Running a UI Automation client makes Chromium and Electron apps build their full
 * accessibility tree, which costs them some CPU and memory, possibly until they restart. That
 * is one reason the option is off by default and the detector only runs while a client that
 * asked for hints is streaming.
 *
 * Based on drunkitguy/apollo2-vibe 306f8f42 (GPLv3).
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
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

    /// Guards `monitor_thread` against one session starting while another stops. Held while
    /// joining, so it is never taken by the detector thread itself.
    std::mutex monitor_mutex;
    std::thread monitor_thread;
    std::atomic<bool> shutdown_requested {false};
    std::atomic<DWORD> monitor_thread_id {0};

    /// Guards the subscribers and the last hint, which the detector thread reads to deliver.
    std::mutex subscribers_mutex;
    std::map<subscription_t, callback_t> subscribers;
    subscription_t next_subscription = 1;
    kind_t last_hint = kind_t::none;

    void deliver(const kind_t kind) {
      std::lock_guard lock {subscribers_mutex};
      last_hint = kind;
      for (const auto &subscriber : subscribers) {
        subscriber.second(kind);
      }
    }

    bool stopping() {
      return shutdown_requested.load(std::memory_order_acquire);
    }

    /// Set by the hook and consumed by the loop, both on the detector thread.
    bool focus_event_pending = false;
    focus_target_t focus_event_target;

    void CALLBACK on_focus_event(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
      // Only note it. Classifying here would put a UI Automation call, which can block for
      // seconds, inside the hook callback.
      focus_event_pending = true;
      focus_event_target = {static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd)), static_cast<std::int32_t>(object), static_cast<std::int32_t>(child)};
    }

    /// The name of a desktop, or nothing when it can't be read.
    std::optional<std::wstring> desktop_name(HDESK desktop) {
      wchar_t name[256] {};
      DWORD needed = 0;
      if (!GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed)) {
        return std::nullopt;
      }
      return std::wstring {name};
    }

    /**
     * @brief Whether the desktop taking input is the one the hook watches.
     * @details It isn't while the lock screen or a UAC prompt is up, and the hook sees nothing
     *          there. Nothing when it can't be told, which leaves the last hint standing.
     */
    std::optional<bool> on_watched_desktop(const std::wstring &watched) {
      HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
      if (!input) {
        return std::nullopt;
      }
      auto input_guard = util::fail_guard([input]() {
        CloseDesktop(input);
      });

      const auto name = desktop_name(input);
      if (!name) {
        return std::nullopt;
      }
      return _wcsicmp(name->c_str(), watched.c_str()) == 0;
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

      // The style bits only mean what classify_edit_style() expects on the stock classes.
      // RichEdit reuses ES_NUMBER's bit for ES_DISABLENOSCROLL.
      const bool rich_edit = _wcsnicmp(class_name, L"RichEdit", 8) == 0;
      if (_wcsicmp(class_name, L"Edit") != 0 && !rich_edit) {
        return std::nullopt;
      }

      return classify_edit_style(static_cast<std::uint32_t>(GetWindowLongPtrW(info.hwndFocus, GWL_STYLE)), rich_edit);
    }

    /**
     * @brief Classify the focused element through UI Automation.
     * @return Nothing when it couldn't be read, or the detector is stopping. Every call below
     *         is a bounded round trip into the focused application, so a stop request is
     *         checked between them rather than after all of them.
     */
    std::optional<kind_t> classify_from_automation(IUIAutomation *uia) {
      IUIAutomationElement *element = nullptr;
      if (FAILED(uia->GetFocusedElement(&element)) || !element) {
        return std::nullopt;
      }
      auto element_guard = util::fail_guard([element]() {
        element->Release();
      });

      automation_facts_t facts;

      CONTROLTYPEID type = 0;
      if (stopping() || FAILED(element->get_CurrentControlType(&type))) {
        return std::nullopt;
      }
      facts.control_type = type;

      if (stopping()) {
        return std::nullopt;
      }

      BOOL is_password = FALSE;
      if (SUCCEEDED(element->get_CurrentIsPassword(&is_password))) {
        facts.is_password = is_password != FALSE;
      }

      // Each property is a round trip into the focused application, so read-only is only
      // asked of the controls whose classification depends on it.
      if (type == UIA_EditControlTypeId || type == UIA_DocumentControlTypeId || type == UIA_ComboBoxControlTypeId || type == UIA_SpinnerControlTypeId) {
        if (stopping()) {
          return std::nullopt;
        }
        IUIAutomationValuePattern *value = nullptr;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern), reinterpret_cast<void **>(&value))) && value) {
          facts.has_value_pattern = true;

          BOOL read_only = FALSE;
          if (!stopping() && SUCCEEDED(value->get_CurrentIsReadOnly(&read_only))) {
            facts.value_read_only = read_only != FALSE;
          }
          value->Release();
        }
      }

      if (stopping()) {
        return std::nullopt;
      }
      return classify_automation(facts);
    }

    std::optional<kind_t> classify_focus(IUIAutomation *uia) {
      // The cheap path first: when it answers, UI Automation is never consulted, and it is
      // the only path that can tell a numeric field apart.
      if (auto kind = classify_from_window_style()) {
        return *kind;
      }
      return classify_from_automation(uia);
    }

    IUIAutomation *create_automation() {
      // Only the interface that can bound a blocking call. Without the timeouts one hung
      // application could hold the detector, and a session waiting to stop it, for 20 seconds.
      IUIAutomation2 *uia2 = nullptr;
      if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation2), reinterpret_cast<void **>(&uia2))) || !uia2) {
        return nullptr;
      }
      if (FAILED(uia2->put_ConnectionTimeout(uia_timeout_ms)) || FAILED(uia2->put_TransactionTimeout(uia_timeout_ms))) {
        uia2->Release();
        return nullptr;
      }
      return uia2;
    }

    /**
     * @brief The detector thread.
     * @details Owns the COM apartment, the UI Automation client, the hook and the message
     *          loop that delivers it. Other threads only touch `shutdown_requested` and, under
     *          `subscribers_mutex`, the subscribers it delivers to.
     */
    void monitor_loop() {
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
        BOOST_LOG(error) << "Focus hints: couldn't create a UI Automation client with timeouts"sv;
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

      // The hook only sees the desktop this thread started on
      const auto watched_desktop = desktop_name(GetThreadDesktop(GetCurrentThreadId())).value_or(L"Default");

      BOOST_LOG(info) << "Focus hints: watching for text field focus"sv;

      debouncer_t debouncer;
      focus_event_pending = false;
      bool away = false;

      // Classify what already has focus, so a stream that starts in a text field gets its
      // keyboard without the user having to click away and back.
      debouncer.focus_changed(debouncer_t::clock::now());

      while (!stopping()) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          DispatchMessageW(&msg);
        }

        if (focus_event_pending) {
          focus_event_pending = false;
          debouncer.focus_changed(debouncer_t::clock::now(), focus_event_target);
        }

        // The lock screen and UAC prompts are on another desktop, where the hook sees no focus
        // at all, so a keyboard raised for a field on this one would stay up through them.
        if (const auto here = on_watched_desktop(watched_desktop); here && *here == away) {
          away = !*here;
          if (away) {
            if (debouncer.classified(kind_t::none, debouncer_t::clock::now())) {
              BOOST_LOG(debug) << "Focus hints: none (input desktop switched)"sv;
              deliver(kind_t::none);
            }
          } else {
            // Back again: whatever has focus now is news
            debouncer.focus_changed(debouncer_t::clock::now());
          }
        }

        if (!away && debouncer.classify_due(debouncer_t::clock::now())) {
          if (const auto kind = classify_focus(uia)) {
            if (debouncer.classified(*kind, debouncer_t::clock::now())) {
              BOOST_LOG(debug) << "Focus hints: "sv << to_string(*kind);
              deliver(*kind);
            }
          } else if (!stopping()) {
            // A timeout says nothing about the field, so the last hint stands
            BOOST_LOG(debug) << "Focus hints: couldn't classify the focused element"sv;
            debouncer.classification_failed(debouncer_t::clock::now());
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

  subscription_t start_monitor(callback_t on_change) {
    std::lock_guard lock {monitor_mutex};

    subscription_t subscription;
    {
      std::lock_guard subscribers_lock {subscribers_mutex};
      subscription = next_subscription++;
      if (monitor_thread.joinable() && last_hint != kind_t::none) {
        // One detector serves the host, so a session that joins it, such as a client
        // reconnecting while its old session times out, would otherwise wait for the next
        // focus change to hear about the field that already has focus.
        on_change(last_hint);
      }
      subscribers.emplace(subscription, std::move(on_change));
    }

    if (!monitor_thread.joinable()) {
      shutdown_requested = false;
      monitor_thread_id = 0;
      monitor_thread = std::thread {monitor_loop};
    }
    return subscription;
  }

  void stop_monitor(const subscription_t subscription) {
    std::lock_guard lock {monitor_mutex};
    {
      std::lock_guard subscribers_lock {subscribers_mutex};
      if (subscribers.erase(subscription) == 0 || !subscribers.empty()) {
        // Unknown, or another session still watches
        return;
      }
    }
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

    std::lock_guard subscribers_lock {subscribers_mutex};
    last_hint = kind_t::none;
  }

}  // namespace focus_hints
