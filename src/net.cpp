#include "net.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace linkstate::detail {
namespace {

#ifdef _WIN32

class WinsockGuard {
 public:
  WinsockGuard() {
    WSADATA data;
    ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }
  ~WinsockGuard() {
    if (ok_) {
      WSACleanup();
    }
  }
  WinsockGuard(const WinsockGuard&) = delete;
  WinsockGuard& operator=(const WinsockGuard&) = delete;
  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
};

bool runtime_ok() {
  static WinsockGuard guard;
  return guard.ok();
}

std::string socket_error_text(const char* prefix) {
  const int code = WSAGetLastError();
  std::string text = prefix;
  text += " (winsock error ";
  text += std::to_string(code);
  text.push_back(')');
  return text;
}

#endif

}  // namespace

bool net_available() {
#ifdef _WIN32
  return runtime_ok();
#else
  return false;
#endif
}

stop_handle net_create_stop_handle() {
#ifdef _WIN32
  return static_cast<stop_handle>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
#else
  return -1;
#endif
}

void net_signal_stop(stop_handle handle) {
#ifdef _WIN32
  if (handle != nullptr) {
    SetEvent(static_cast<HANDLE>(handle));
  }
#else
  (void)handle;
#endif
}

void net_destroy_stop_handle(stop_handle handle) {
#ifdef _WIN32
  if (handle != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle));
  }
#else
  (void)handle;
#endif
}

socket_handle net_listen(const std::string& address, std::uint16_t port, std::string& error,
                         std::uint16_t& bound_port) {
  bound_port = 0;
#ifdef _WIN32
  if (!runtime_ok()) {
    error = "the socket runtime is unavailable on this host";
    return invalid_socket;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  if (getaddrinfo(address.c_str(), service.c_str(), &hints, &result) != 0 || result == nullptr) {
    error = "the listening address could not be resolved";
    return invalid_socket;
  }
  socket_handle listener = invalid_socket;
  for (addrinfo* candidate = result; candidate != nullptr; candidate = candidate->ai_next) {
    socket_handle socket = ::socket(candidate->ai_family, candidate->ai_socktype,
                                    candidate->ai_protocol);
    if (socket == invalid_socket) {
      continue;
    }
    const BOOL exclusive = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    if (::bind(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) != 0) {
      ::closesocket(socket);
      continue;
    }
    if (::listen(socket, SOMAXCONN) != 0) {
      ::closesocket(socket);
      continue;
    }
    listener = socket;
    break;
  }
  freeaddrinfo(result);
  if (listener == invalid_socket) {
    error = socket_error_text("the coordinator could not bind its listening socket");
    return invalid_socket;
  }
  sockaddr_storage local{};
  int local_length = static_cast<int>(sizeof(local));
  if (getsockname(listener, reinterpret_cast<sockaddr*>(&local), &local_length) != 0) {
    error = socket_error_text("the listening socket address could not be read");
    ::closesocket(listener);
    return invalid_socket;
  }
  bound_port = ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
  return listener;
#else
  (void)address;
  (void)port;
  error =
      "the distributed transport is implemented for Windows in this release; the capability is "
      "UNSUPPORTED on this platform";
  return invalid_socket;
#endif
}

socket_handle net_accept(socket_handle listener, stop_handle stop, bool& stopped,
                         std::string& error) {
  stopped = false;
#ifdef _WIN32
  const WSAEVENT accept_event = WSACreateEvent();
  if (accept_event == WSA_INVALID_EVENT) {
    error = socket_error_text("the accept event could not be created");
    return invalid_socket;
  }
  if (WSAEventSelect(listener, accept_event, FD_ACCEPT) == SOCKET_ERROR) {
    error = socket_error_text("the listening socket could not be armed for accept");
    WSACloseEvent(accept_event);
    return invalid_socket;
  }
  HANDLE handles[2] = {accept_event, static_cast<HANDLE>(stop)};
  const DWORD index = WSAWaitForMultipleEvents(2, handles, FALSE, WSA_INFINITE, FALSE);
  socket_handle client = invalid_socket;
  if (index == WSA_WAIT_EVENT_0) {
    WSANETWORKEVENTS events{};
    if (WSAEnumNetworkEvents(listener, accept_event, &events) == SOCKET_ERROR) {
      error = socket_error_text("the accept event could not be inspected");
    } else if ((events.lNetworkEvents & FD_ACCEPT) != 0) {
      client = ::accept(listener, nullptr, nullptr);
      if (client == invalid_socket) {
        error = socket_error_text("an inbound connection could not be accepted");
      } else {
        // The accepted socket inherits the non-blocking mode implied by the
        // listening socket's event association. Clear the association and force
        // the session socket back to blocking mode, otherwise the first receive
        // would fail with WSAEWOULDBLOCK.
        WSAEventSelect(client, nullptr, 0);
        u_long blocking = 0;
        if (ioctlsocket(client, FIONBIO, &blocking) != 0) {
          error = socket_error_text("a session socket could not be set to blocking mode");
          ::closesocket(client);
          client = invalid_socket;
        } else {
          const BOOL no_delay = TRUE;
          setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
        }
      }
    } else {
      error = "the accept event did not report an inbound connection";
    }
  } else if (index == WSA_WAIT_EVENT_0 + 1) {
    stopped = true;
  } else {
    error = socket_error_text("waiting for an inbound connection failed");
  }
  WSAEventSelect(listener, nullptr, 0);
  WSACloseEvent(accept_event);
  return client;
#else
  (void)listener;
  (void)stop;
  error =
      "the distributed transport is implemented for Windows in this release; the capability is "
      "UNSUPPORTED on this platform";
  return invalid_socket;
#endif
}

socket_handle net_connect(const std::string& address, std::uint16_t port, std::string& error) {
#ifdef _WIN32
  if (!runtime_ok()) {
    error = "the socket runtime is unavailable on this host";
    return invalid_socket;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  if (getaddrinfo(address.c_str(), service.c_str(), &hints, &result) != 0 || result == nullptr) {
    error = "the coordinator address could not be resolved";
    return invalid_socket;
  }
  socket_handle client = invalid_socket;
  for (addrinfo* candidate = result; candidate != nullptr; candidate = candidate->ai_next) {
    socket_handle socket = ::socket(candidate->ai_family, candidate->ai_socktype,
                                    candidate->ai_protocol);
    if (socket == invalid_socket) {
      continue;
    }
    if (::connect(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) != 0) {
      ::closesocket(socket);
      continue;
    }
    const BOOL no_delay = TRUE;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay),
               sizeof(no_delay));
    client = socket;
    break;
  }
  freeaddrinfo(result);
  if (client == invalid_socket) {
    error = socket_error_text("the coordinator could not be reached");
  }
  return client;
#else
  (void)address;
  (void)port;
  error =
      "the distributed transport is implemented for Windows in this release; the capability is "
      "UNSUPPORTED on this platform";
  return invalid_socket;
#endif
}

bool net_send_all(socket_handle socket, std::span<const std::byte> bytes, std::string& error) {
#ifdef _WIN32
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    const int chunk = static_cast<int>((std::min)(remaining, static_cast<std::size_t>(1u << 20)));
    const int sent = ::send(socket, reinterpret_cast<const char*>(bytes.data() + offset), chunk, 0);
    if (sent <= 0) {
      error = socket_error_text("a frame could not be sent");
      return false;
    }
    offset += static_cast<std::size_t>(sent);
  }
  return true;
#else
  (void)socket;
  (void)bytes;
  error =
      "the distributed transport is implemented for Windows in this release; the capability is "
      "UNSUPPORTED on this platform";
  return false;
#endif
}

int net_receive(socket_handle socket, std::byte* buffer, std::size_t size, std::string& error) {
#ifdef _WIN32
  const int received =
      ::recv(socket, reinterpret_cast<char*>(buffer), static_cast<int>(size), 0);
  if (received == SOCKET_ERROR) {
    error = socket_error_text("a frame could not be received");
    return -1;
  }
  return received;
#else
  (void)socket;
  (void)buffer;
  (void)size;
  error =
      "the distributed transport is implemented for Windows in this release; the capability is "
      "UNSUPPORTED on this platform";
  return -1;
#endif
}

void net_shutdown(socket_handle socket) {
#ifdef _WIN32
  if (socket != invalid_socket) {
    ::shutdown(socket, SD_BOTH);
  }
#else
  (void)socket;
#endif
}

void net_close(socket_handle socket) {
#ifdef _WIN32
  if (socket != invalid_socket) {
    ::closesocket(socket);
  }
#else
  (void)socket;
#endif
}

}  // namespace linkstate::detail
