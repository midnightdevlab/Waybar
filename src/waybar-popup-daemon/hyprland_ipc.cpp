#include "hyprland_ipc.hpp"
#include <cstring>
#include <sstream>

HyprlandIPC::HyprlandIPC() {
    startEventListener();
}

HyprlandIPC::~HyprlandIPC() {
    stopEventListener();
    if (m_command_socket >= 0) close(m_command_socket);
    if (m_event_socket >= 0) close(m_event_socket);
}

std::string HyprlandIPC::getSocketPath(const char* socket_name) {
    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig) {
        spdlog::error("[HyprIPC] HYPRLAND_INSTANCE_SIGNATURE not set");
        return "";
    }
    
    // Try XDG_RUNTIME_DIR first (preferred location)
    const char* xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime) {
        return std::string(xdg_runtime) + "/hypr/" + sig + "/" + socket_name;
    }
    
    // Fallback to /tmp
    return std::string("/tmp/hypr/") + sig + "/" + socket_name;
}

bool HyprlandIPC::sendCommand(const std::string& command) {
    std::string socket_path = getSocketPath(".socket.sock");
    if (socket_path.empty()) return false;
    
    // Create new socket for each command
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        spdlog::error("[HyprIPC] Failed to create socket");
        return false;
    }
    
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        spdlog::error("[HyprIPC] Failed to connect to Hyprland");
        close(sock);
        return false;
    }
    
    // Send command
    if (send(sock, command.c_str(), command.length(), 0) < 0) {
        spdlog::error("[HyprIPC] Failed to send command");
        close(sock);
        return false;
    }
    
    // Read response
    char buffer[8192];
    ssize_t n = recv(sock, buffer, sizeof(buffer) - 1, 0);
    close(sock);
    
    if (n < 0) {
        spdlog::error("[HyprIPC] Failed to read response");
        return false;
    }
    
    buffer[n] = '\0';
    std::string response(buffer);
    spdlog::debug("[HyprIPC] Response: {}", response);
    
    return response.find("ok") != std::string::npos;
}

bool HyprlandIPC::startEventListener() {
    std::string socket_path = getSocketPath(".socket2.sock");
    if (socket_path.empty()) return false;
    
    m_event_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_event_socket < 0) {
        spdlog::error("[HyprIPC] Failed to create event socket");
        return false;
    }
    
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    
    if (connect(m_event_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        spdlog::error("[HyprIPC] Failed to connect to event socket");
        close(m_event_socket);
        m_event_socket = -1;
        return false;
    }
    
    m_running = true;
    m_event_thread = std::thread(&HyprlandIPC::eventListenerThread, this);
    spdlog::info("[HyprIPC] Event listener started");
    return true;
}

void HyprlandIPC::stopEventListener() {
    m_running = false;
    if (m_event_thread.joinable()) {
        m_event_thread.join();
    }
}

void HyprlandIPC::eventListenerThread() {
    char buffer[4096];
    std::string accumulated;
    
    while (m_running) {
        ssize_t n = recv(m_event_socket, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) {
            if (m_running) {
                spdlog::error("[HyprIPC] Event socket disconnected");
            }
            break;
        }
        
        buffer[n] = '\0';
        accumulated += buffer;
        
        // Process complete lines
        size_t pos;
        while ((pos = accumulated.find('\n')) != std::string::npos) {
            std::string event = accumulated.substr(0, pos);
            accumulated = accumulated.substr(pos + 1);
            
            // Store in circular buffer
            int idx = m_event_write_index.fetch_add(1) % 100;
            m_recent_events[idx] = event;
            
            // Log ALL events to figure out what Hyprland sends
            spdlog::info("[HyprIPC] Event: {}", event);
        }
    }
}

bool HyprlandIPC::waitForEvent(const std::function<bool(const std::string&)>& predicate, int timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    int last_checked_index = m_event_write_index.load();
    
    while (true) {
        // Check elapsed time
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (elapsed > timeout_ms) {
            spdlog::warn("[HyprIPC] Wait timeout after {}ms", timeout_ms);
            return false;
        }
        
        // Check new events
        int current_index = m_event_write_index.load();
        if (current_index > last_checked_index) {
            // Check events from last_checked to current
            for (int i = last_checked_index; i < current_index; i++) {
                const std::string& event = m_recent_events[i % 100];
                if (predicate(event)) {
                    spdlog::debug("[HyprIPC] Event matched");
                    return true;
                }
            }
            last_checked_index = current_index;
        }
        
        // Small sleep to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool HyprlandIPC::moveWindow(const std::string& address, int x, int y, int timeout_ms) {
    std::string cmd = "/dispatch movewindowpixel exact " + std::to_string(x) + " " + 
                      std::to_string(y) + ",address:0x" + address;
    
    spdlog::debug("[HyprIPC] Moving window {} to ({},{})", address, x, y);
    
    if (!sendCommand(cmd)) {
        return false;
    }
    
    // Poll window state until position matches (with tolerance of 5px)
    auto start = std::chrono::steady_clock::now();
    int poll_count = 0;
    while (true) {
        auto state = getWindowState(address);
        poll_count++;
        
        if (state && abs(state->x - x) < 5 && abs(state->y - y) < 5) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            spdlog::info("[HyprIPC] Window moved to ({},{}) confirmed in {}ms ({} polls)", 
                        state->x, state->y, elapsed, poll_count);
            return true;
        }
        
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeout_ms) {
            spdlog::warn("[HyprIPC] Move timeout after {}ms ({} polls) - window at ({},{}) expected ({},{})", 
                        elapsed, poll_count, state ? state->x : -1, state ? state->y : -1, x, y);
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool HyprlandIPC::resizeWindow(const std::string& address, int w, int h, int timeout_ms) {
    std::string cmd = "/dispatch resizewindowpixel exact " + std::to_string(w) + " " + 
                      std::to_string(h) + ",address:0x" + address;
    
    spdlog::debug("[HyprIPC] Resizing window {} to {}x{}", address, w, h);
    
    if (!sendCommand(cmd)) {
        return false;
    }
    
    // Poll window state until size matches (with tolerance of 5px)
    auto start = std::chrono::steady_clock::now();
    int poll_count = 0;
    while (true) {
        auto state = getWindowState(address);
        poll_count++;
        
        if (state && abs(state->w - w) < 5 && abs(state->h - h) < 5) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            spdlog::info("[HyprIPC] Window resized to {}x{} confirmed in {}ms ({} polls)", 
                        state->w, state->h, elapsed, poll_count);
            return true;
        }
        
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeout_ms) {
            spdlog::warn("[HyprIPC] Resize timeout after {}ms ({} polls) - window at {}x{} expected {}x{}", 
                        elapsed, poll_count, state ? state->w : -1, state ? state->h : -1, w, h);
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool HyprlandIPC::moveToWorkspace(const std::string& address, const std::string& workspace, int timeout_ms) {
    std::string cmd = "/dispatch movetoworkspacesilent " + workspace + ",address:0x" + address;
    
    spdlog::debug("[HyprIPC] Moving window {} to workspace {}", address, workspace);
    
    if (!sendCommand(cmd)) {
        return false;
    }
    
    // movetoworkspacesilent DOES emit movewindow event
    return waitForEvent([&address](const std::string& event) {
        return event.find("movewindow>>0x" + address) != std::string::npos;
    }, timeout_ms);
}

std::optional<HyprlandIPC::WindowState> HyprlandIPC::getWindowState(const std::string& address) {
    std::string socket_path = getSocketPath(".socket.sock");
    if (socket_path.empty()) return std::nullopt;
    
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return std::nullopt;
    
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return std::nullopt;
    }
    
    std::string cmd = "/clients";
    send(sock, cmd.c_str(), cmd.length(), 0);
    
    char buffer[32768];
    ssize_t n = recv(sock, buffer, sizeof(buffer) - 1, 0);
    close(sock);
    
    if (n <= 0) return std::nullopt;
    
    buffer[n] = '\0';
    std::string response(buffer);
    
    // Parse to find our window - looking for "Window 0xADDRESS"
    size_t pos = response.find("Window " + address);
    if (pos == std::string::npos) {
        pos = response.find("Window 0x" + address);
        if (pos == std::string::npos) return std::nullopt;
    }
    
    // Extract position and size
    WindowState state{};
    
    // Find "at: X,Y"
    size_t at_pos = response.find("at: ", pos);
    if (at_pos != std::string::npos) {
        sscanf(response.c_str() + at_pos + 4, "%d,%d", &state.x, &state.y);
    }
    
    // Find "size: W,H"
    size_t size_pos = response.find("size: ", pos);
    if (size_pos != std::string::npos) {
        sscanf(response.c_str() + size_pos + 6, "%d,%d", &state.w, &state.h);
    }
    
    // Find workspace name
    size_t ws_pos = response.find("workspace: ", pos);
    if (ws_pos != std::string::npos) {
        size_t paren = response.find("(", ws_pos);
        size_t close_paren = response.find(")", paren);
        if (paren != std::string::npos && close_paren != std::string::npos) {
            state.workspace = response.substr(paren + 1, close_paren - paren - 1);
        }
    }
    
    return state;
}
