#include "util/popup_daemon_manager.hpp"

#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <cstring>
#include <thread>
#include <chrono>

namespace waybar::util {

PopupDaemonManager& PopupDaemonManager::getInstance() {
  static PopupDaemonManager instance;
  return instance;
}

PopupDaemonManager::~PopupDaemonManager() {
  spdlog::info("[PopupDaemon] Destructor called");
  stop();
}

bool PopupDaemonManager::isDaemonRunning() {
  // Check if socket exists and is accessible
  struct stat st;
  if (stat(SOCKET_PATH, &st) != 0) {
    return false;
  }
  
  // Try to connect to socket
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    return false;
  }
  
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
  
  bool connected = (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);
  close(sock);
  
  return connected;
}

bool PopupDaemonManager::startDaemon() {
  spdlog::info("[PopupDaemon] Starting waybar-popup-daemon...");
  
  pid_t pid = fork();
  
  if (pid < 0) {
    spdlog::error("[PopupDaemon] Failed to fork: {}", strerror(errno));
    return false;
  }
  
  if (pid == 0) {
    // Child process
    // Pass our PID so daemon can monitor us
    pid_t my_pid = getppid();
    char parent_pid_str[32];
    snprintf(parent_pid_str, sizeof(parent_pid_str), "%d", my_pid);
    
    // Try installed location first
    execlp("waybar-popup-daemon", "waybar-popup-daemon", parent_pid_str, nullptr);
    
    // If not in PATH, try build directory (development mode)
    char waybar_path[4096];
    ssize_t len = readlink("/proc/self/exe", waybar_path, sizeof(waybar_path) - 1);
    if (len != -1) {
      waybar_path[len] = '\0';
      std::string daemon_path = waybar_path;
      size_t pos = daemon_path.rfind("/waybar");
      if (pos != std::string::npos) {
        daemon_path = daemon_path.substr(0, pos) + "/waybar-popup-daemon";
        execl(daemon_path.c_str(), "waybar-popup-daemon", parent_pid_str, nullptr);
      }
    }
    
    spdlog::error("[PopupDaemon] Failed to exec daemon: {}", strerror(errno));
    _exit(1);
  }
  
  // Parent process
  m_daemon_pid = pid;
  spdlog::info("[PopupDaemon] Daemon started with PID {}", pid);
  
  // Wait for daemon to be ready
  for (int i = 0; i < 20; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (isDaemonRunning()) {
      spdlog::info("[PopupDaemon] Daemon is ready");
      m_running = true;
      return true;
    }
  }
  
  spdlog::error("[PopupDaemon] Daemon failed to start (socket not ready)");
  return false;
}

bool PopupDaemonManager::ensureDaemonRunning() {
  if (isDaemonRunning()) {
    return true;
  }
  
  if (!startDaemon()) {
    return false;
  }
  
  // Start monitoring thread if not already running
  if (!m_monitoring) {
    m_monitoring = true;
    m_monitor_thread = std::make_unique<std::thread>([this]() {
      monitorDaemon();
    });
  }
  
  return true;
}

void PopupDaemonManager::monitorDaemon() {
  spdlog::info("[PopupDaemon] Monitor thread started");
  
  while (m_monitoring) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    if (!m_monitoring) break;
    
    // Check if daemon is still running
    if (!isDaemonRunning()) {
      spdlog::warn("[PopupDaemon] Daemon crashed or stopped, restarting...");
      m_running = false;
      
      // Clean up old socket file if it exists
      unlink(SOCKET_PATH);
      
      // Try to restart
      if (startDaemon()) {
        spdlog::info("[PopupDaemon] Daemon restarted successfully");
      } else {
        spdlog::error("[PopupDaemon] Failed to restart daemon, will retry in 2s");
      }
    }
  }
  
  spdlog::info("[PopupDaemon] Monitor thread stopped");
}

void PopupDaemonManager::stop() {
  // Prevent double-stop (can be called explicitly and from destructor)
  if (m_stopped.exchange(true)) {
    return;
  }
  
  try {
    if (m_monitoring) {
      spdlog::info("[PopupDaemon] Stopping daemon manager...");
      m_monitoring = false;
      
      if (m_monitor_thread && m_monitor_thread->joinable()) {
        m_monitor_thread->join();
      }
    }
    
    // Note: No need to kill daemon - it monitors our PID and exits automatically when we die
    spdlog::info("[PopupDaemon] Daemon will exit automatically (parent monitoring)");
  } catch (const std::exception& e) {
    spdlog::error("[PopupDaemon] Exception during stop: {}", e.what());
  } catch (...) {
    spdlog::error("[PopupDaemon] Unknown exception during stop");
  }
}

} // namespace waybar::util
