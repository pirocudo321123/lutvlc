// VlcMsvcCompat.h
// Fills the three POSIX-shaped holes VLC 3.0.x's plugin headers assume are already
// plugged by the time they're included. VLC's own build plugs them from the
// configure-generated config.h (which ends with `#include <vlc_fixups.h>`), and the
// Windows SDK ships neither. Building out-of-tree against the SDK with MSVC there-
// fore hits all three at once:
//
//   ssize_t      vlc_arrays.h's vlc_array_index_of_item() and vlc_configuration.h's
//                config_Get*Choices() return it. MSVC has no ssize_t, only SSIZE_T
//                from <BaseTsd.h>, so those declarations parse as implicit-int and
//                cascade into C4430/C2146/C2059/C2143/C2447.
//   poll()       vlc_threads.h's Win32 branch defines a static inline vlc_poll()
//                whose body calls poll(). Winsock's equivalent is WSAPoll(), which
//                is spelled differently but takes the same struct pollfd.
//   N_() / _()   the gettext no-op/lookup macros, defined in vlc_fixups.h. Used by
//                every module's set_description/add_* strings.
//
// INCLUDE THIS FIRST, before any vlc_*.h, in every TU that touches the VLC SDK.
// Both lut_hdr_vlc.cpp and ColorSpaceVLC.h do so at the top of their include lists.
//
// VLC 4.x is not a target here (see build.yml's VLC_VERSION and the filter_t API
// note next to it), so none of this attempts to be version-adaptive.

#pragma once

#ifndef _WIN32
#error "VlcMsvcCompat.h is a Windows/MSVC shim; it should not be reachable elsewhere."
#endif

// ---------------------------------------------------------------------------
// ssize_t
// ---------------------------------------------------------------------------
// _SSIZE_T_DEFINED is the guard MinGW and several third-party headers use, so
// honoring it keeps us from double-typedef'ing if something else got here first.
#include <BaseTsd.h>
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif

// ---------------------------------------------------------------------------
// poll()
// ---------------------------------------------------------------------------
// winsock2.h must precede windows.h or the old winsock.h gets pulled in first and
// the two collide; including it here (before anything else includes windows.h)
// gets the ordering right for the whole TU.
//
// struct pollfd / WSAPoll require _WIN32_WINNT >= 0x0600. The VS2022 SDK defaults
// well above that, so this is a "fail loudly if someone lowers it" check rather
// than something that trips in a normal build.
#include <winsock2.h>
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#error "VLC's vlc_threads.h needs poll(); WSAPoll requires _WIN32_WINNT >= 0x0600."
#endif

// A function, deliberately, not `#define poll WSAPoll`: vlc_threads.h itself does
// `#define poll(u,n,t) vlc_poll(u,n,t)` right after defining vlc_poll, and a macro
// here would collide with that (C4005). A real function is simply found by name
// lookup inside vlc_poll's body and then quietly shadowed by VLC's macro for all
// later call sites, which is exactly the intended arrangement.
static inline int poll(struct pollfd* fds, unsigned long nfds, int timeout)
{
    return WSAPoll(fds, static_cast<ULONG>(nfds), timeout);
}

// ---------------------------------------------------------------------------
// gettext no-ops
// ---------------------------------------------------------------------------
// Matches vlc_fixups.h's definitions. N_() marks a string for extraction without
// translating it (module option text is translated later, by VLC, at display time);
// _() does the lookup immediately.
//
// _ is guarded because it's a short enough identifier to plausibly collide. This
// plugin only uses N_, so if some future include really does define _ first, that
// wins and nothing here breaks.
#ifndef gettext_noop
#define gettext_noop(str) (str)
#endif
#ifndef N_
#define N_(str) gettext_noop(str)
#endif
#ifndef _
#define _(str) vlc_gettext(str)
#endif
