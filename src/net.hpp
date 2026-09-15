#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

namespace linkstate::detail {

#ifdef _WIN32
using socket_handle = SOCKET;
inline constexpr socket_handle invalid_socket = INVALID_SOCKET;
using stop_handle = void*;
#else
using socket_handle = int;
inline constexpr socket_handle invalid_socket = -1;
using stop_handle = int;
#endif

/// True when a usable socket runtime exists on this platform.
bool net_available();

/// Creates a stop handle used to interrupt a blocking accept without a timeout.
stop_handle net_create_stop_handle();
void net_signal_stop(stop_handle handle);
void net_destroy_stop_handle(stop_handle handle);

/// Binds and listens on the given address and port. Port 0 selects an ephemeral
/// port, which is reported through bound_port.
socket_handle net_listen(const std::string& address, std::uint16_t port, std::string& error,
                         std::uint16_t& bound_port);

/// Waits for a connection or for the stop handle to be signalled. Returns
/// invalid_socket and sets stopped when the stop handle was signalled.
socket_handle net_accept(socket_handle listener, stop_handle stop, bool& stopped,
                         std::string& error);

/// Connects to a listening endpoint using a blocking connect.
socket_handle net_connect(const std::string& address, std::uint16_t port, std::string& error);

/// Sends the whole buffer. Returns false on failure.
bool net_send_all(socket_handle socket, std::span<const std::byte> bytes, std::string& error);

/// Receives available bytes. Returns the number of bytes read, 0 when the peer
/// closed the connection and -1 on failure.
int net_receive(socket_handle socket, std::byte* buffer, std::size_t size, std::string& error);

/// Shuts the connection down in both directions. Safe to call repeatedly.
void net_shutdown(socket_handle socket);

/// Closes the socket. Safe to call repeatedly.
void net_close(socket_handle socket);

}  // namespace linkstate::detail
