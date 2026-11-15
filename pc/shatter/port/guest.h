// Shatter on the lifted game (RunnerRecompiled port): the guest/host boundary.
//
// Upstream Shatter is a 32-bit Windows DLL living in the game's own address space: its "pointers" are the guest's
// 32-bit addresses, it calls the PhysX core through vtables with __fastcall casts (== __thiscall), and the core and the
// game call its functions through vtables it patched. Here the solver runs natively (arm64, SSE through sse2neon) and
// the PhysX core is lifted code in guest memory, so this header provides:
//   ptr32 / fromPtr32     guest address <-> host pointer (guest memory is one host mapping at x86_mem_base)
//   GuestThis<R(A...)>    a guest __thiscall function (an original vtable slot of the core) callable from the host
//   guestThiscall         the same for a one-off call
//   GuestScratch          bytes on the guest stack for out-parameters of such calls
//   SHATTER_THUNK(fn)     a guest address (HLE thunk) that runs the host function `R fn(Self, void* edx, A...)`
//                         as __thiscall: this in ECX, A on the stack (popped by the callee), result in EAX or ST(0)
// Guest code only runs on guest threads: every guest call must come from a thread that is inside a call from the
// guest (the game thread in NxScene::simulate/fetchResults, the hooked vtable slots). The fluid thread and the
// worker pool never call the guest.
#pragma once

#include <stdint.h>
#include <string.h>

#include <cstddef>
#include <type_traits>
#include <utility>

extern "C" {
#include "rt.h"
}

namespace shim {

inline uint32_t ptr32(const void* p) {
  if (!p) return 0;
  const uintptr_t a = reinterpret_cast<uintptr_t>(p) - static_cast<uintptr_t>(x86_mem_base);
  if (a >> 32) rt_fatal("shatter: host pointer %p is not in guest memory", p);
  return static_cast<uint32_t>(a);
}
template <class T>
inline T* fromPtr32(uint32_t p) {
  return p ? reinterpret_cast<T*>(static_cast<uintptr_t>(x86_mem_base) + p) : nullptr;
}
inline bool inGuestMemory(const void* p) {
  return p && ((reinterpret_cast<uintptr_t>(p) - static_cast<uintptr_t>(x86_mem_base)) >> 32) == 0;
}

inline X86Ctx* guestCtx() {
  RtThread* t = rt_thread_self();
  if (!t) rt_fatal("shatter: guest call from a thread without a guest context");
  return &t->ctx;
}

// ---- one 32-bit stack slot <-> a host value ----
template <class T>
inline T argIn(uint32_t v) {
  if constexpr (std::is_pointer_v<T>) {
    return fromPtr32<std::remove_pointer_t<T>>(v);
  } else if constexpr (std::is_same_v<T, float>) {
    float f;
    memcpy(&f, &v, 4);
    return f;
  } else if constexpr (std::is_same_v<T, bool>) {
    return (v & 0xff) != 0;  // MSVC bool: AL / the low byte of the slot
  } else {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 4, "one 32-bit stack slot per argument");
    return static_cast<T>(v);
  }
}
template <class T>
inline uint32_t argOut(T v) {
  if constexpr (std::is_pointer_v<T>) {
    return ptr32(v);
  } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
    return 0;
  } else if constexpr (std::is_same_v<T, float>) {
    uint32_t u;
    memcpy(&u, &v, 4);
    return u;
  } else if constexpr (std::is_same_v<T, bool>) {
    return v ? 1u : 0u;
  } else {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 4, "one 32-bit stack slot per argument");
    return static_cast<uint32_t>(v);
  }
}

// ---- host -> guest ----
template <class R, class... A>
R guestThiscall(uint32_t fn, uint32_t self, A... a) {
  X86Ctx* c = guestCtx();
  const uint32_t args[sizeof...(A) + 1] = {argOut(a)..., 0};
  const uint32_t ecx = c->ecx, edx = c->edx;
  c->ecx = self;
  rt_call(c, fn, (int)sizeof...(A), args);
  c->ecx = ecx, c->edx = edx;
  if constexpr (std::is_void_v<R>) {
    return;
  } else if constexpr (std::is_same_v<R, float>) {
    const float r = (float)c->st[c->ftop & 7];  // returned in ST(0): pop it
    x87_pop(c);
    return r;
  } else {
    return argIn<R>(c->eax);
  }
}

template <class Sig>
class GuestThis;
template <class R, class... A>
class GuestThis<R(A...)> {
 public:
  GuestThis() = default;
  explicit GuestThis(uint32_t fn) : fn_(fn) {}
  uint32_t addr() const { return fn_; }
  explicit operator bool() const { return fn_ != 0; }
  // (self, edx, args): the shape of the upstream __fastcall casts, so their call sites stay as they are.
  R operator()(const void* self, const void*, A... a) const { return guestThiscall<R>(fn_, ptr32(self), a...); }

 private:
  uint32_t fn_ = 0;
};

// Bytes below the guest stack pointer for out-parameters of guest calls; released (ESP restored) at scope exit.
class GuestScratch {
 public:
  GuestScratch() : c_(guestCtx()), saved_(c_->esp) {}
  ~GuestScratch() { c_->esp = saved_; }
  GuestScratch(const GuestScratch&) = delete;
  GuestScratch& operator=(const GuestScratch&) = delete;
  uint32_t alloc(uint32_t bytes) {
    c_->esp = (c_->esp - bytes) & ~15u;
    memset(fromPtr32<void>(c_->esp), 0, bytes);
    return c_->esp;
  }

 private:
  X86Ctx* c_;
  uint32_t saved_;
};

// ---- guest -> host ----
template <class R>
inline void retToGuest(X86Ctx* c, R v) {
  if constexpr (std::is_same_v<R, float> || std::is_same_v<R, double>)
    x87_push(c, (double)v);
  else
    c->eax = argOut(v);
}

template <auto F>
struct HostMethod;
template <class R, class S, class... A, R (*F)(S, void*, A...)>
struct HostMethod<F> {
  static constexpr uint8_t kArgs = (uint8_t)sizeof...(A);
  static void call(X86Ctx* c) { invoke(c, std::index_sequence_for<A...>{}); }
  template <size_t... I>
  static void invoke(X86Ctx* c, std::index_sequence<I...>) {
    const uint32_t raw[sizeof...(A) + 1] = {rt_arg(c, (int)I)..., 0};
    (void)raw;
    if constexpr (std::is_void_v<R>)
      F(argIn<S>(c->ecx), nullptr, argIn<A>(raw[I])...);
    else
      retToGuest<R>(c, F(argIn<S>(c->ecx), nullptr, argIn<A>(raw[I])...));
  }
};

// __cdecl exports (the caller pops the arguments).
template <auto F>
struct HostCdecl;
template <class R, class... A, R (*F)(A...)>
struct HostCdecl<F> {
  static constexpr uint8_t kArgs = 0;
  static void call(X86Ctx* c) { invoke(c, std::index_sequence_for<A...>{}); }
  template <size_t... I>
  static void invoke(X86Ctx* c, std::index_sequence<I...>) {
    const uint32_t raw[sizeof...(A) + 1] = {rt_arg(c, (int)I)..., 0};
    (void)raw;
    if constexpr (std::is_void_v<R>)
      F(argIn<A>(raw[I])...);
    else
      retToGuest<R>(c, F(argIn<A>(raw[I])...));
  }
};

}  // namespace shim

#define SHATTER_THUNK(fn) rt_hle_thunk_fn("shatter", #fn, &::shim::HostMethod<&fn>::call, ::shim::HostMethod<&fn>::kArgs)
#define SHATTER_CDECL(fn) rt_hle_thunk_fn("shatter", #fn, &::shim::HostCdecl<&fn>::call, 0)
