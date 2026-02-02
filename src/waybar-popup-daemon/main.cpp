// Waybar Popup Daemon - POC
// Separate GTK application for showing interactive popups

#include <gtkmm.h>
#include <spdlog/spdlog.h>
#include <json/json.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <thread>
#include <atomic>
#include <sstream>
#include <chrono>
#include "hyprland_ipc.hpp"

constexpr const char* SOCKET_PATH = "/tmp/waybar-popup.sock";

class PopupWindow : public Gtk::Window {
public:
  PopupWindow(HyprlandIPC& hypr_ipc) : m_hypr_ipc(hypr_ipc) {
    set_title("waybar-thumbnail-popup");
    set_decorated(false);
    set_type_hint(Gdk::WINDOW_TYPE_HINT_TOOLTIP);
    set_skip_taskbar_hint(true);
    set_skip_pager_hint(true);
    set_keep_above(true);
    
    // Set window gravity to NORTH_WEST so resize anchors to top-left
    // This prevents the window from being re-centered when it grows
    set_gravity(Gdk::GRAVITY_NORTH_WEST);
    
    // Create content box
    m_box.set_orientation(Gtk::ORIENTATION_VERTICAL);
    m_box.set_margin_top(8);
    m_box.set_margin_bottom(8);
    m_box.set_margin_start(8);
    m_box.set_margin_end(8);
    m_box.set_spacing(4);
    
    add(m_box);
    
    // Setup hover events to keep window visible
    signal_enter_notify_event().connect([this](GdkEventCrossing*) {
      spdlog::debug("[DAEMON] Mouse entered popup");
      // Cancel any pending hide
      if (m_hide_timeout) {
        m_hide_timeout.disconnect();
      }
      return false;
    });
    
    signal_leave_notify_event().connect([this](GdkEventCrossing*) {
      spdlog::debug("[DAEMON] Mouse left popup");
      // Hide after 500ms delay (allows moving back to popup)
      m_hide_timeout = Glib::signal_timeout().connect([this]() {
        spdlog::debug("[DAEMON] Auto-hiding after mouse leave");
        hide();
        return false;  // Don't repeat
      }, 500);
      return false;
    });
    
    // Don't show/hide yet - window address will be retrieved on first show
    // This avoids the window being hidden before compositor creates it
  }
  
  void init() {
    // Show window briefly to register with compositor and get address
    // We'll do this after GTK main loop starts
    Glib::signal_idle().connect_once([this]() {
      resize(20, 20);  // Tiny size
      
      // Position offscreen before showing
      move(-10000, -10000);
      
      show_all();
      
      // Give compositor time to register
      Glib::signal_timeout().connect_once([this]() {
        m_window_address = getHyprlandAddress();
        spdlog::info("[DAEMON] Window address detected: {}", m_window_address);
        // Move offscreen using Hyprland IPC (this definitely works)
        if (!m_window_address.empty()) {
          hide();
        }
      }, 200);  // 200ms delay
    });
  }
  
  std::string getWindowAddress() const { return m_window_address; }
  
  // Override hide to move offscreen instead (Hyprland can't find hidden windows)
  void hide() {
    m_should_show = false;  // Cancel any pending show
    spdlog::debug("[DAEMON] hide() called, m_should_show set to false");
    if (!m_window_address.empty()) {
      spdlog::debug("[DAEMON] Hiding window (moving to -10000,-10000)");
      m_hypr_ipc.moveWindow(m_window_address, -10000, -10000);
    }
    // Also resize to tiny
    resize(20, 20);
  }
  
  void updateContent(const std::vector<std::string>& titles, 
                     const std::vector<std::string>& image_paths = {}) {
    // Clear old content
    for (auto* child : m_box.get_children()) {
      m_box.remove(*child);
    }
    
    // Add content with images if provided
    size_t num_items = titles.size();
    for (size_t i = 0; i < num_items; i++) {
      auto* vbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));
      
      // Add image if path provided
      if (i < image_paths.size() && !image_paths[i].empty()) {
        spdlog::debug("[DAEMON] Loading image: {}", image_paths[i]);
        try {
          auto pixbuf = Gdk::Pixbuf::create_from_file(image_paths[i]);
          spdlog::debug("[DAEMON] Image loaded: {}x{}", pixbuf->get_width(), pixbuf->get_height());
          
          // Scale to reasonable thumbnail size (e.g., 200px wide, preserve aspect ratio)
          int width = pixbuf->get_width();
          int height = pixbuf->get_height();
          int target_width = 200;
          int target_height = (height * target_width) / width;
          
          if (width > target_width) {
            pixbuf = pixbuf->scale_simple(target_width, target_height, Gdk::INTERP_BILINEAR);
            spdlog::debug("[DAEMON] Image scaled to: {}x{}", target_width, target_height);
          }
          
          auto* image = Gtk::manage(new Gtk::Image(pixbuf));
          image->set_size_request(target_width, target_height);  // Force exact size
          vbox->pack_start(*image, false, false);
          spdlog::debug("[DAEMON] Image widget added to layout");
        } catch (const Glib::Error& e) {
          spdlog::warn("[DAEMON] Failed to load image {}: {}", image_paths[i], e.what().c_str());
          // Fall through to show title without image
        }
      }
      
      // Add title label below image
      auto* label = Gtk::manage(new Gtk::Label("• " + titles[i]));
      label->set_xalign(0.0);
      vbox->pack_start(*label, false, false);
      
      m_box.pack_start(*vbox, false, false);
    }
    
    m_box.show_all();
    
    // Force window to recalculate size - reset to minimum
    resize(20, 20);
  }
  
  void showAt(int x, int y, const std::string& monitor) {
    spdlog::info("[DAEMON] Show at ({},{}) on monitor {}", x, y, monitor);
    
    if (m_window_address.empty()) {
      spdlog::error("[DAEMON] Cannot position - no window address (init failed?)");
      return;
    }
    
    // Get monitor offset to convert relative to absolute coordinates
    auto [mon_x, mon_y] = getMonitorOffset(monitor);
    int abs_x = mon_x + x;
    int abs_y = mon_y + y;
    
    spdlog::debug("[DAEMON] Monitor {} offset: ({},{}), absolute position ({},{})", 
                  monitor, mon_x, mon_y, abs_x, abs_y);
    
    // 1. Move offscreen first (synchronous)
    hide();
    
    m_should_show = true;  // Re-enable showing after hide (hide sets it to false)
    spdlog::debug("[DAEMON] m_should_show set to true after hide()");
    
    // 2. Calculate required size from GTK
    // First show content (but still hidden offscreen)
    show_all();
    
    // Process GTK events to let it calculate sizes
    while (Gtk::Main::events_pending()) {
      Gtk::Main::iteration();
    }
    
    // Get the preferred size from GTK
    Gtk::Requisition min_size, natural_size;
    get_preferred_size(min_size, natural_size);
    int target_w = natural_size.width;
    int target_h = natural_size.height;
    
    spdlog::debug("[DAEMON] GTK preferred size: {}x{}", target_w, target_h);
    
    // Resize window to exact size needed (both GTK and Hyprland)
    resize(target_w, target_h);
    m_hypr_ipc.resizeWindow(m_window_address, target_w, target_h);
    spdlog::debug("[DAEMON] Resized to {}x{}", target_w, target_h);
    
    // Check if we should still show (might have been cancelled)
    if (!m_should_show) {
      spdlog::debug("[DAEMON] Show cancelled (m_should_show=false)");
      return;
    }
    
    spdlog::debug("[DAEMON] Proceeding with show (m_should_show=true)");
    
    // 3. Move to workspace and position (synchronous operations)
    // Check if already on correct workspace to avoid unnecessary move
    auto current_state = m_hypr_ipc.getWindowState(m_window_address);
    if (!current_state || current_state->workspace != ".waybar0") {
      spdlog::debug("[DAEMON] Moving to workspace .waybar0 (currently on: {})", 
                    current_state ? current_state->workspace : "unknown");
      m_hypr_ipc.moveToWorkspace(m_window_address, "name:.waybar0");
    } else {
      spdlog::debug("[DAEMON] Already on workspace .waybar0, skipping workspace move");
    }
    
    spdlog::debug("[DAEMON] Positioning at ({},{})", abs_x, abs_y);
    m_hypr_ipc.moveWindow(m_window_address, abs_x, abs_y);
  }

private:
  HyprlandIPC& m_hypr_ipc;
  Gtk::Box m_box;
  std::string m_window_address;
  sigc::connection m_hide_timeout;
  std::atomic<bool> m_should_show{false};
  
  std::pair<int,int> getMonitorOffset(const std::string& monitor_name) {
    // Query monitor information from Hyprland
    FILE* pipe = popen("hyprctl -j monitors", "r");
    if (!pipe) {
      spdlog::error("[DAEMON] Failed to query monitors");
      return {0, 0};
    }
    
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
      result += buffer;
    }
    pclose(pipe);
    
    // Parse JSON to find monitor offset
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream stream(result);
    std::string errors;
    
    if (Json::parseFromStream(builder, stream, &root, &errors)) {
      for (const auto& monitor : root) {
        if (monitor["name"].asString() == monitor_name) {
          int x = monitor["x"].asInt();
          int y = monitor["y"].asInt();
          spdlog::debug("[DAEMON] Monitor {} offset: ({},{})", monitor_name, x, y);
          return {x, y};
        }
      }
    }
    
    spdlog::warn("[DAEMON] Monitor {} not found, using (0,0)", monitor_name);
    return {0, 0};
  }
  
  std::string getHyprlandAddress() {
    // Retry more times with longer delays - window needs time to be registered by compositor
    for (int attempt = 0; attempt < 10; attempt++) {
      if (attempt > 0) {
        spdlog::debug("[DAEMON] Retry {} to get window address", attempt);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      
      // Get window list and find our window by title
      FILE* pipe = popen("hyprctl clients -j", "r");
      if (!pipe) continue;
      
      std::string result;
      char buffer[128];
      while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
      }
      pclose(pipe);
      
      // Debug: log the raw JSON on first attempt
      if (attempt == 0) {
        spdlog::debug("[DAEMON] hyprctl clients output (first 500 chars): {}", result.substr(0, 500));
      }
      
      // Parse JSON to find our window
      Json::Value root;
      Json::CharReaderBuilder builder;
      std::istringstream stream(result);
      std::string errors;
      
      if (Json::parseFromStream(builder, stream, &root, &errors)) {
        spdlog::debug("[DAEMON] Found {} clients in hyprctl", root.size());
        for (const auto& client : root) {
          std::string title = client["title"].asString();
          if (attempt == 0 && !title.empty()) {
            spdlog::debug("[DAEMON]   Client: '{}'", title);
          }
          if (title == "waybar-thumbnail-popup") {
            std::string addr = client["address"].asString();
            // Remove "0x" prefix if present
            if (addr.substr(0, 2) == "0x") {
              addr = addr.substr(2);
            }
            return addr;
          }
        }
      } else {
        spdlog::error("[DAEMON] JSON parse error: {}", errors);
      }
    }
    
    return "";
  }
};

class IPCServer {
public:
  IPCServer(PopupWindow& window) : m_window(window), m_running(false) {}
  
  bool start() {
    // Remove old socket if exists
    unlink(SOCKET_PATH);
    
    // Create Unix socket
    m_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_socket < 0) {
      spdlog::error("[DAEMON] Failed to create socket");
      return false;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(m_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      spdlog::error("[DAEMON] Failed to bind socket");
      return false;
    }
    
    if (listen(m_socket, 5) < 0) {
      spdlog::error("[DAEMON] Failed to listen on socket");
      return false;
    }
    
    spdlog::info("[DAEMON] IPC server listening on {}", SOCKET_PATH);
    
    // Start accept thread
    m_running = true;
    m_thread = std::thread(&IPCServer::acceptLoop, this);
    
    return true;
  }
  
  void stop() {
    m_running = false;
    if (m_thread.joinable()) {
      m_thread.join();
    }
    close(m_socket);
    unlink(SOCKET_PATH);
  }

private:
  PopupWindow& m_window;
  int m_socket;
  std::atomic<bool> m_running;
  std::thread m_thread;
  
  void acceptLoop() {
    while (m_running) {
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(m_socket, &readfds);
      
      struct timeval tv;
      tv.tv_sec = 1;
      tv.tv_usec = 0;
      
      int ret = select(m_socket + 1, &readfds, nullptr, nullptr, &tv);
      if (ret <= 0) continue;
      
      int client = accept(m_socket, nullptr, nullptr);
      if (client < 0) continue;
      
      handleClient(client);
      close(client);
    }
  }
  
  void handleClient(int client) {
    char buffer[4096];
    ssize_t n = read(client, buffer, sizeof(buffer) - 1);
    if (n <= 0) return;
    
    buffer[n] = '\0';
    std::string message(buffer);
    
    spdlog::debug("[DAEMON] Received: {}", message);
    
    // Parse JSON command
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream stream(message);
    std::string errors;
    
    if (!Json::parseFromStream(builder, stream, &root, &errors)) {
      spdlog::error("[DAEMON] Invalid JSON: {}", errors);
      return;
    }
    
    std::string type = root["type"].asString();
    
    if (type == "show") {
      int x = root["x"].asInt();
      int y = root["y"].asInt();
      std::string monitor = root["monitor"].asString();
      
      std::vector<std::string> titles;
      for (const auto& title : root["titles"]) {
        titles.push_back(title.asString());
      }
      
      std::vector<std::string> image_paths;
      if (root.isMember("images")) {
        for (const auto& path : root["images"]) {
          image_paths.push_back(path.asString());
        }
      }
      
      // Update on main thread
      Glib::signal_idle().connect_once([this, x, y, monitor, titles, image_paths]() {
        m_window.updateContent(titles, image_paths);
        m_window.showAt(x, y, monitor);
      });
      
    } else if (type == "hide") {
      Glib::signal_idle().connect_once([this]() {
        m_window.hide();
      });
    }
  }
};

int main(int argc, char* argv[]) {
  // Setup logging
  spdlog::set_level(spdlog::level::debug);
  spdlog::info("[DAEMON] Starting waybar-popup-daemon");
  
  // Check if parent PID was passed
  pid_t parent_pid = 0;
  if (argc > 1) {
    parent_pid = std::atoi(argv[1]);
    spdlog::info("[DAEMON] Parent PID: {}", parent_pid);
  }
  
  auto app = Gtk::Application::create("org.waybar.popup");
  
  // Create Hyprland IPC handler (starts event listener)
  HyprlandIPC hypr_ipc;
  
  PopupWindow window(hypr_ipc);
  window.init();
  
  IPCServer server(window);
  if (!server.start()) {
    spdlog::error("[DAEMON] Failed to start IPC server");
    return 1;
  }
  
  // Monitor parent if PID was provided
  std::atomic<bool> should_run{true};
  std::thread parent_monitor;
  if (parent_pid > 0) {
    parent_monitor = std::thread([parent_pid, app, &should_run]() {
      while (should_run) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (kill(parent_pid, 0) != 0) {
          spdlog::info("[DAEMON] Parent died, exiting");
          Glib::signal_idle().connect_once([app]() { app->quit(); });
          break;
        }
      }
    });
  }
  
  spdlog::info("[DAEMON] Ready");
  app->hold();
  int result = app->run();
  
  should_run = false;
  if (parent_monitor.joinable()) parent_monitor.join();
  
  server.stop();
  return result;
}
