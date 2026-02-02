#include "util/popup_ipc_client.hpp"

#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

namespace waybar::util {

PopupIPCClient::PopupIPCClient() = default;

PopupIPCClient::~PopupIPCClient() {
  disconnect();
}

bool PopupIPCClient::connect() {
  if (m_connected) {
    return true;
  }
  
  // Create socket
  m_socket = socket(AF_UNIX, SOCK_STREAM, 0);
  if (m_socket < 0) {
    spdlog::error("[PopupIPC] Failed to create socket: {}", strerror(errno));
    return false;
  }
  
  // Connect to daemon
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
  
  if (::connect(m_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    spdlog::debug("[PopupIPC] Failed to connect to daemon: {}", strerror(errno));
    close(m_socket);
    m_socket = -1;
    return false;
  }
  
  m_connected = true;
  spdlog::debug("[PopupIPC] Connected to daemon");
  return true;
}

void PopupIPCClient::disconnect() {
  if (m_socket >= 0) {
    close(m_socket);
    m_socket = -1;
  }
  m_connected = false;
}

bool PopupIPCClient::sendCommand(const Json::Value& command) {
  if (!m_connected) {
    spdlog::warn("[PopupIPC] Not connected to daemon");
    return false;
  }
  
  // Serialize to JSON string
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";  // Compact output
  std::string json_str = Json::writeString(builder, command);
  
  // Send to daemon
  ssize_t written = write(m_socket, json_str.c_str(), json_str.length());
  if (written < 0) {
    spdlog::error("[PopupIPC] Failed to write to socket: {}", strerror(errno));
    disconnect();
    return false;
  }
  
  if (static_cast<size_t>(written) != json_str.length()) {
    spdlog::warn("[PopupIPC] Partial write to socket");
    disconnect();
    return false;
  }
  
  spdlog::debug("[PopupIPC] Sent command: {}", json_str);
  return true;
}

bool PopupIPCClient::showPopup(int x, int y, const std::string& monitor,
                                const std::vector<std::string>& titles) {
  return showPopup(x, y, monitor, titles, {});
}

bool PopupIPCClient::showPopup(int x, int y, const std::string& monitor,
                                const std::vector<std::string>& titles,
                                const std::vector<std::string>& image_paths) {
  // Disconnect first to force fresh connection (daemon closes after each command)
  disconnect();
  
  // Connect
  if (!connect()) {
    return false;
  }
  
  // Build JSON command
  Json::Value command;
  command["type"] = "show";
  command["x"] = x;
  command["y"] = y;
  command["monitor"] = monitor;
  
  Json::Value titles_array(Json::arrayValue);
  for (const auto& title : titles) {
    titles_array.append(title);
  }
  command["titles"] = titles_array;
  
  if (!image_paths.empty()) {
    Json::Value images_array(Json::arrayValue);
    for (const auto& path : image_paths) {
      images_array.append(path);
    }
    command["images"] = images_array;
  }
  
  bool result = sendCommand(command);
  
  // Disconnect after send (daemon will close its end anyway)
  disconnect();
  
  return result;
}

bool PopupIPCClient::hidePopup() {
  // Disconnect first to force fresh connection (daemon closes after each command)
  disconnect();
  
  // Connect
  if (!connect()) {
    return false;
  }
  
  // Build JSON command
  Json::Value command;
  command["type"] = "hide";
  
  bool result = sendCommand(command);
  
  // Disconnect after send (daemon will close its end anyway)
  disconnect();
  
  return result;
}

} // namespace waybar::util
