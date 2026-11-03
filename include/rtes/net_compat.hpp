#pragma once
// Small portability layer for sockets (Linux and macOS).
//
// Writing to a socket whose peer has closed raises SIGPIPE, which kills the
// process by default. Linux lets each send() opt out with MSG_NOSIGNAL;
// macOS/BSD do not have that flag and use the SO_NOSIGPIPE socket option
// instead. Use both helpers: whichever applies on this platform takes effect.

#include <sys/socket.h>

namespace rtes::net {

#ifdef MSG_NOSIGNAL
inline constexpr int kSendNoSignal = MSG_NOSIGNAL;
#else
inline constexpr int kSendNoSignal = 0;
#endif

// Call once on every socket this process writes to.
inline void disable_sigpipe([[maybe_unused]] int fd) noexcept {
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

} // namespace rtes::net
