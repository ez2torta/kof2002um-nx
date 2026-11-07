/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <chrono>

#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;

using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

constexpr uint32_t XINPUT_FLAG_GAMEPAD = 0x01;
constexpr uint32_t XINPUT_FLAG_ANY_USER = 1 << 30;

rex::input::InputSystem* input_system() {
  return static_cast<rex::input::InputSystem*>(REX_KERNEL_STATE()->emulator()->input_system());
}

// kof2002um-nx (2026-11-04): input diagnostics. The title screen ignored every button and the debug log had no
// input line at all, so each export logs its first call and then a summary every 2 s of wall time: calls, how many
// succeeded, the last user/flags/result and the buttons seen pressed (OR of every successful state/keystroke).
struct InputDiag {
  const char* name;
  std::atomic<uint32_t> calls{0}, ok{0}, buttons{0}, last_user{0}, last_flags{0}, last_result{0};
  std::atomic<int64_t> next_ms{0};
  std::atomic<bool> first{true};
};
InputDiag g_diag_state{"XamInputGetState"}, g_diag_caps{"XamInputGetCapabilities"},
    g_diag_key{"XamInputGetKeystroke"}, g_diag_keyex{"XamInputGetKeystrokeEx"};

void DiagNote(InputDiag& d, uint32_t user, uint32_t flags, uint32_t result, uint32_t buttons) {
  d.calls.fetch_add(1, std::memory_order_relaxed);
  if (result == X_ERROR_SUCCESS) d.ok.fetch_add(1, std::memory_order_relaxed);
  d.buttons.fetch_or(buttons, std::memory_order_relaxed);
  d.last_user.store(user, std::memory_order_relaxed);
  d.last_flags.store(flags, std::memory_order_relaxed);
  d.last_result.store(result, std::memory_order_relaxed);
  if (d.first.exchange(false)) {
    REXKRNL_INFO("[input-diag] first {}: user=0x{:X} flags=0x{:X} -> 0x{:X}", d.name, user, flags, result);
  }
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  int64_t next = d.next_ms.load(std::memory_order_relaxed);
  if (now < next || !d.next_ms.compare_exchange_strong(next, now + 2000)) return;
  REXKRNL_INFO("[input-diag] {}: {} calls, {} ok, last user=0x{:X} flags=0x{:X} -> 0x{:X}, buttons seen 0x{:04X}",
               d.name, d.calls.exchange(0), d.ok.exchange(0), d.last_user.load(), d.last_flags.load(),
               d.last_result.load(), d.buttons.exchange(0));
}

void XamResetInactivity_entry() {
  // Do we need to do anything?
}

u32 XamEnableInactivityProcessing_entry(u32 unk, u32 enable) {
  return X_ERROR_SUCCESS;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetcapabilities(v=vs.85).aspx
u32 XamInputGetCapabilities_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  REXKRNL_TRACE("[XAM] XamInputGetCapabilities called: user={}, flags=0x{:X}", (uint32_t)user_index,
                (uint32_t)flags);
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  const X_RESULT r = is->GetCapabilities(actual_user_index, flags, caps);
  DiagNote(g_diag_caps, user_index, flags, r, 0);
  return r;
}

u32 XamInputGetCapabilitiesEx_entry(u32 unk, u32 user_index, u32 flags,
                                    ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto* is = input_system();
  return is->GetCapabilities(actual_user_index, flags, caps);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetstate(v=vs.85).aspx
u32 XamInputGetState_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_STATE> input_state) {
  // Games call this with a NULL state ptr, probably as a query.
  static int call_count = 0;
  if (++call_count <= 5) {
    REXKRNL_TRACE("[XAM] XamInputGetState called: user={}, flags=0x{:X}", (uint32_t)user_index,
                  (uint32_t)flags);
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    DiagNote(g_diag_state, user_index, flags, X_ERROR_DEVICE_NOT_CONNECTED, 0);
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  const X_RESULT r = is->GetState(actual_user_index, input_state);
  DiagNote(g_diag_state, user_index, flags, r,
           (r == X_ERROR_SUCCESS && input_state) ? uint32_t(uint16_t(input_state->gamepad.buttons)) : 0u);
  return r;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputsetstate(v=vs.85).aspx
u32 XamInputSetState_entry(u32 user_index, u32 unk, ppc_ptr_t<X_INPUT_VIBRATION> vibration) {
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  uint32_t actual_user_index = user_index;
  if ((user_index & 0xFF) == 0xFF) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto* is = input_system();
  return is->SetState(actual_user_index, vibration);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetkeystroke(v=vs.85).aspx
u32 XamInputGetKeystroke_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_KEYSTROKE> keystroke) {
  // https://github.com/CodeAsm/ffplay360/blob/master/Common/AtgXime.cpp
  // user index = index or XUSER_INDEX_ANY
  // flags = XINPUT_FLAG_GAMEPAD (| _ANYUSER | _ANYDEVICE)

  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  const X_RESULT r = is->GetKeystroke(actual_user_index, flags, keystroke);
  DiagNote(g_diag_key, user_index, flags, r, r == X_ERROR_SUCCESS ? uint32_t(uint16_t(keystroke->virtual_key)) : 0u);
  return r;
}

// Same as non-ex, just takes a pointer to user index.
u32 XamInputGetKeystrokeEx_entry(mapped_u32 user_index_ptr, u32 flags,
                                 ppc_ptr_t<X_INPUT_KEYSTROKE> keystroke) {
  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t user_index = *user_index_ptr;
  if ((user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    user_index = 0;
  }

  auto* is = input_system();
  auto result = is->GetKeystroke(user_index, flags, keystroke);
  DiagNote(g_diag_keyex, uint32_t(*user_index_ptr), flags, result,
           result == X_ERROR_SUCCESS ? uint32_t(uint16_t(keystroke->virtual_key)) : 0u);
  if (XSUCCEEDED(result)) {
    *user_index_ptr = keystroke->user_index;
  }
  return result;
}

i32 XamUserGetDeviceContext_entry(u32 user_index, u32 unk, mapped_u32 out_ptr) {
  // Games check the result - usually with some masking.
  // If this function fails they assume zero, so let's fail AND
  // set zero just to be safe.
  *out_ptr = 0;
  if (!user_index || (user_index & 0xFF) == 0xFF) {
    return X_E_SUCCESS;
  } else {
    return X_E_DEVICE_NOT_CONNECTED;
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamResetInactivity, rex::kernel::xam::XamResetInactivity_entry)
REX_EXPORT(__imp__XamEnableInactivityProcessing,
           rex::kernel::xam::XamEnableInactivityProcessing_entry)
REX_EXPORT(__imp__XamInputGetCapabilities, rex::kernel::xam::XamInputGetCapabilities_entry)
REX_EXPORT(__imp__XamInputGetCapabilitiesEx, rex::kernel::xam::XamInputGetCapabilitiesEx_entry)
REX_EXPORT(__imp__XamInputGetState, rex::kernel::xam::XamInputGetState_entry)
REX_EXPORT(__imp__XamInputSetState, rex::kernel::xam::XamInputSetState_entry)
REX_EXPORT(__imp__XamInputGetKeystroke, rex::kernel::xam::XamInputGetKeystroke_entry)
REX_EXPORT(__imp__XamInputGetKeystrokeEx, rex::kernel::xam::XamInputGetKeystrokeEx_entry)
REX_EXPORT(__imp__XamUserGetDeviceContext, rex::kernel::xam::XamUserGetDeviceContext_entry)

REX_EXPORT_STUB(__imp__XamInputControl);
REX_EXPORT_STUB(__imp__XamInputEnableAutobind);
REX_EXPORT_STUB(__imp__XamInputGetDeviceStats);
REX_EXPORT_STUB(__imp__XamInputGetFailedConnectionOrBind);
REX_EXPORT_STUB(__imp__XamInputGetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHud);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHudEx);
REX_EXPORT_STUB(__imp__XamInputGetUserVibrationLevel);
REX_EXPORT_STUB(__imp__XamInputNonControllerGetRaw);
REX_EXPORT_STUB(__imp__XamInputNonControllerGetRawEx);
REX_EXPORT_STUB(__imp__XamInputNonControllerSetRaw);
REX_EXPORT_STUB(__imp__XamInputNonControllerSetRawEx);
REX_EXPORT_STUB(__imp__XamInputRawState);
REX_EXPORT_STUB(__imp__XamInputResetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSendStayAliveRequest);
REX_EXPORT_STUB(__imp__XamInputSendXenonButtonPress);
REX_EXPORT_STUB(__imp__XamInputSetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputSetKeyboardTranslationHud);
REX_EXPORT_STUB(__imp__XamInputSetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSetMinMaxAuthDelay);
REX_EXPORT_STUB(__imp__XamInputSetTextMessengerIndicator);
REX_EXPORT_STUB(__imp__XamInputToggleKeyLocks);
