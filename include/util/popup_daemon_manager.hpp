#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>

namespace waybar::util {

class PopupDaemonManager {
public:
  static PopupDaemonManager& getInstance();
  
  // Start daemon if not already running
  bool ensureDaemonRunning();
  
  // Check if daemon is responsive
  bool isDaemonRunning();
  
  // Stop monitoring (called on waybar exit)
  void stop();
  
  // Get socket path
  static constexpr const char* SOCKET_PATH = "/tmp/waybar-popup.sock";
  
private:
  PopupDaemonManager() = default;
  ~PopupDaemonManager();
  
  PopupDaemonManager(const PopupDaemonManager&) = delete;
  PopupDaemonManager& operator=(const PopupDaemonManager&) = delete;
  
  bool startDaemon();
  void monitorDaemon();
  
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_monitoring{false};
  std::atomic<bool> m_stopped{false};
  std::unique_ptr<std::thread> m_monitor_thread;
  pid_t m_daemon_pid{-1};
};

} // namespace waybar::util
