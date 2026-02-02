#pragma once

#include <json/json.h>
#include <string>
#include <vector>
#include <memory>

namespace waybar::util {

class PopupIPCClient {
public:
  PopupIPCClient();
  ~PopupIPCClient();

  // Connect to daemon (returns false if daemon not running)
  bool connect();
  
  // Show popup at given position with window titles
  bool showPopup(int x, int y, const std::string& monitor, 
                 const std::vector<std::string>& titles);
  
  // Show popup with titles and images
  bool showPopup(int x, int y, const std::string& monitor,
                 const std::vector<std::string>& titles,
                 const std::vector<std::string>& image_paths);
  
  // Hide popup
  bool hidePopup();
  
  // Check if connected to daemon
  bool isConnected() const { return m_connected; }
  
  // Get socket path
  static constexpr const char* SOCKET_PATH = "/tmp/waybar-popup.sock";

private:
  bool sendCommand(const Json::Value& command);
  void disconnect();
  
  int m_socket{-1};
  bool m_connected{false};
};

} // namespace waybar::util
