#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <chrono>
#include <optional>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <spdlog/spdlog.h>

class HyprlandIPC {
public:
    HyprlandIPC();
    ~HyprlandIPC();
    
    // Synchronous operations - wait for Hyprland to confirm
    bool moveWindow(const std::string& address, int x, int y, int timeout_ms = 1000);
    bool resizeWindow(const std::string& address, int w, int h, int timeout_ms = 1000);
    bool moveToWorkspace(const std::string& address, const std::string& workspace, int timeout_ms = 1000);
    
    // Query window state
    struct WindowState {
        int x, y, w, h;
        std::string workspace;
    };
    std::optional<WindowState> getWindowState(const std::string& address);
    
    // Start listening to events
    bool startEventListener();
    void stopEventListener();
    
private:
    std::string getSocketPath(const char* socket_name);
    bool sendCommand(const std::string& command);
    void eventListenerThread();
    bool waitForEvent(const std::function<bool(const std::string&)>& predicate, int timeout_ms);
    
    int m_command_socket{-1};
    int m_event_socket{-1};
    std::thread m_event_thread;
    std::atomic<bool> m_running{false};
    
    // Circular buffer for recent events (last 100)
    std::string m_recent_events[100];
    std::atomic<int> m_event_write_index{0};
};
#include <optional>
