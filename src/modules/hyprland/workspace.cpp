#include <json/value.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <utility>
#include <set>
#include <filesystem>
#include <optional>

#include <gdkmm/pixbuf.h>
#include <glibmm/fileutils.h>
#include <glibmm/keyfile.h>
#include <glibmm/miscutils.h>

#include "modules/hyprland/workspaces.hpp"
#include "util/command.hpp"
#include "util/icon_loader.hpp"
#include "util/gtk_icon.hpp"

namespace waybar::modules::hyprland {

// Helper functions for icon loading (from AAppIconLabel.cpp)
namespace {

std::string toLowerCase(const std::string& input) {
  std::string result = input;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

std::optional<std::string> getFileBySuffix(const std::string& dir, const std::string& suffix,
                                           bool check_lower_case) {
  if (!std::filesystem::exists(dir)) {
    return {};
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      std::string filename = entry.path().filename().string();
      if (filename.size() < suffix.size()) {
        continue;
      }
      if ((filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0) ||
          (check_lower_case && filename.compare(filename.size() - suffix.size(), suffix.size(),
                                                toLowerCase(suffix)) == 0)) {
        return entry.path().string();
      }
    }
  }
  return {};
}

std::optional<std::string> getDesktopFilePath(const std::string& app_identifier,
                                              const std::string& alternative_app_identifier) {
  if (app_identifier.empty()) {
    return {};
  }

  auto data_dirs = Glib::get_system_data_dirs();
  data_dirs.insert(data_dirs.begin(), Glib::get_user_data_dir());
  for (const auto& data_dir : data_dirs) {
    const auto data_app_dir = data_dir + "/applications/";
    auto desktop_file_suffix = app_identifier + ".desktop";
    auto desktop_file_path = getFileBySuffix(data_app_dir, desktop_file_suffix, true);
    if (desktop_file_path.has_value()) {
      return desktop_file_path;
    }
    if (!alternative_app_identifier.empty()) {
      desktop_file_suffix = alternative_app_identifier + ".desktop";
      desktop_file_path = getFileBySuffix(data_app_dir, desktop_file_suffix, true);
      if (desktop_file_path.has_value()) {
        return desktop_file_path;
      }
    }
  }
  return {};
}

std::optional<Glib::ustring> getIconName(const std::string& app_identifier,
                                         const std::string& alternative_app_identifier) {
  const auto desktop_file_path = getDesktopFilePath(app_identifier, alternative_app_identifier);
  if (!desktop_file_path.has_value()) {
    // Try some heuristics to find a matching icon
    if (DefaultGtkIconThemeWrapper::has_icon(app_identifier)) {
      return app_identifier;
    }

    auto app_identifier_desktop = app_identifier + "-desktop";
    if (DefaultGtkIconThemeWrapper::has_icon(app_identifier_desktop)) {
      return app_identifier_desktop;
    }

    auto first_space = app_identifier.find_first_of(' ');
    if (first_space != std::string::npos) {
      auto first_word = toLowerCase(app_identifier.substr(0, first_space));
      if (DefaultGtkIconThemeWrapper::has_icon(first_word)) {
        return first_word;
      }
    }

    const auto first_dash = app_identifier.find_first_of('-');
    if (first_dash != std::string::npos) {
      auto first_word = toLowerCase(app_identifier.substr(0, first_dash));
      if (DefaultGtkIconThemeWrapper::has_icon(first_word)) {
        return first_word;
      }
    }

    return {};
  }

  try {
    Glib::KeyFile desktop_file;
    desktop_file.load_from_file(desktop_file_path.value());
    return desktop_file.get_string("Desktop Entry", "Icon");
  } catch (Glib::FileError& error) {
    spdlog::warn("Error while loading desktop file {}: {}", desktop_file_path.value(),
                 std::string(error.what()));
  } catch (Glib::KeyFileError& error) {
    spdlog::warn("Error while loading desktop file {}: {}", desktop_file_path.value(),
                 std::string(error.what()));
  }
  return {};
}

} // namespace

Workspace::Workspace(const Json::Value &workspace_data, Workspaces &workspace_manager,
                     const Json::Value &clients_data)
    : m_workspaceManager(workspace_manager),
      m_id(workspace_data["id"].asInt()),
      m_name(workspace_data["name"].asString()),
      m_output(workspace_data["monitor"].asString()),  // TODO:allow using monitor desc
      m_windows(workspace_data["windows"].asInt()),
      m_isActive(true),
      m_isPersistentRule(workspace_data["persistent-rule"].asBool()),
      m_isPersistentConfig(workspace_data["persistent-config"].asBool()),
      m_ipc(IPC::inst()) {
  if (m_name.starts_with("name:")) {
    m_name = m_name.substr(5);
  } else if (m_name.starts_with("special")) {
    m_name = m_id == -99 ? m_name : m_name.substr(8);
    m_isSpecial = true;
  }

  m_button.add_events(Gdk::BUTTON_PRESS_MASK);
  m_button.signal_button_press_event().connect(sigc::mem_fun(*this, &Workspace::handleClicked),
                                               false);

  m_button.set_relief(Gtk::RELIEF_NONE);
  m_iconBox.set_spacing(2);
  if (m_workspaceManager.enableTaskbar()) {
    m_content.set_orientation(m_workspaceManager.taskbarOrientation());
    m_content.pack_start(m_labelBefore, false, false);
  } else {
    m_content.set_center_widget(m_labelBefore);
  }
  m_content.pack_end(m_iconBox, false, false);
  m_button.add(m_content);

  initializeWindowMap(clients_data);
}

void addOrRemoveClass(const Glib::RefPtr<Gtk::StyleContext> &context, bool condition,
                      const std::string &class_name) {
  if (condition) {
    context->add_class(class_name);
  } else {
    context->remove_class(class_name);
  }
}

std::optional<WindowRepr> Workspace::closeWindow(WindowAddress const &addr) {
  auto it = std::ranges::find_if(m_windowMap,
                                 [&addr](const auto &window) { return window.address == addr; });
  // If the vector contains the address, remove it and return the window representation
  if (it != m_windowMap.end()) {
    WindowRepr windowRepr = *it;
    m_windowMap.erase(it);
    return windowRepr;
  }
  return std::nullopt;
}

bool Workspace::handleClicked(GdkEventButton *bt) const {
  if (bt->type == GDK_BUTTON_PRESS) {
    try {
      // Right-click on empty workspace: remove it (even if active)
      if (bt->button == 3 && isEmpty()) {
        spdlog::debug("Right-click on empty workspace '{}', attempting to remove", name());
        std::string cmd = "waybar-workspace-remove.sh " + name();
        util::command::res result = util::command::exec(cmd, "workspace-remove");
        if (result.exit_code == 0) {
          spdlog::info("Removed workspace '{}'", name());
          return true;
        } else {
          spdlog::warn("Workspace removal failed: {}", result.out);
          return false;
        }
      }
      
      // Left-click on active workspace: create a new workspace for the same project
      if (bt->button == 1 && isActive()) {
        spdlog::debug("Active workspace clicked, attempting to create new workspace for project");
        util::command::res result = util::command::exec("waybar-workspace-create-current.sh", "workspace-create");
        if (result.exit_code == 0) {
          spdlog::info("Created new workspace via script");
          return true;
        } else {
          spdlog::warn("Workspace creation script failed: {}", result.out);
          // Fall through to normal behavior
        }
      }
      
      // Normal workspace switching behavior (left-click on inactive workspace)
      if (bt->button == 1) {
        if (id() > 0) {  // normal
          if (m_workspaceManager.moveToMonitor()) {
            m_ipc.getSocket1Reply("dispatch focusworkspaceoncurrentmonitor " + std::to_string(id()));
          } else {
            m_ipc.getSocket1Reply("dispatch workspace " + std::to_string(id()));
          }
        } else if (!isSpecial()) {  // named (this includes persistent)
          if (m_workspaceManager.moveToMonitor()) {
            m_ipc.getSocket1Reply("dispatch focusworkspaceoncurrentmonitor name:" + name());
          } else {
            m_ipc.getSocket1Reply("dispatch workspace name:" + name());
          }
        } else if (id() != -99) {  // named special
          m_ipc.getSocket1Reply("dispatch togglespecialworkspace " + name());
        } else {  // special
          m_ipc.getSocket1Reply("dispatch togglespecialworkspace");
        }
        return true;
      }
    } catch (const std::exception &e) {
      spdlog::error("Failed to dispatch workspace: {}", e.what());
    }
  }
  return false;
}

void Workspace::initializeWindowMap(const Json::Value &clients_data) {
  m_windowMap.clear();
  for (auto client : clients_data) {
    if (client["workspace"]["id"].asInt() == id()) {
      insertWindow({client});
    }
  }
}

void Workspace::setActiveWindow(WindowAddress const &addr) {
  std::optional<long> activeIdx;
  for (size_t i = 0; i < m_windowMap.size(); ++i) {
    auto &window = m_windowMap[i];
    bool isActive = (window.address == addr);
    window.setActive(isActive);
    if (isActive) {
      activeIdx = i;
    }
  }

  auto activeWindowPos = m_workspaceManager.activeWindowPosition();
  if (activeIdx.has_value() && activeWindowPos != Workspaces::ActiveWindowPosition::NONE) {
    auto window = std::move(m_windowMap[*activeIdx]);
    m_windowMap.erase(m_windowMap.begin() + *activeIdx);
    if (activeWindowPos == Workspaces::ActiveWindowPosition::FIRST) {
      m_windowMap.insert(m_windowMap.begin(), std::move(window));
    } else if (activeWindowPos == Workspaces::ActiveWindowPosition::LAST) {
      m_windowMap.emplace_back(std::move(window));
    }
  }
}

void Workspace::insertWindow(WindowCreationPayload create_window_payload) {
  if (!create_window_payload.isEmpty(m_workspaceManager)) {
    auto repr = create_window_payload.repr(m_workspaceManager);

    if (!repr.empty() || m_workspaceManager.enableTaskbar()) {
      auto addr = create_window_payload.getAddress();
      auto it = std::ranges::find_if(
          m_windowMap, [&addr](const auto &window) { return window.address == addr; });
      // If the vector contains the address, update the window representation, otherwise insert it
      if (it != m_windowMap.end()) {
        *it = repr;
      } else {
        m_windowMap.emplace_back(repr);
      }
    }
  }
};

bool Workspace::onWindowOpened(WindowCreationPayload const &create_window_payload) {
  if (create_window_payload.getWorkspaceName() == name()) {
    insertWindow(create_window_payload);
    return true;
  }
  return false;
}

std::string &Workspace::selectIcon(std::map<std::string, std::string> &icons_map) {
  spdlog::trace("Selecting icon for workspace {}", name());
  if (isUrgent()) {
    auto urgentIconIt = icons_map.find("urgent");
    if (urgentIconIt != icons_map.end()) {
      return urgentIconIt->second;
    }
  }

  if (isActive()) {
    auto activeIconIt = icons_map.find("active");
    if (activeIconIt != icons_map.end()) {
      return activeIconIt->second;
    }
  }

  if (isSpecial()) {
    auto specialIconIt = icons_map.find("special");
    if (specialIconIt != icons_map.end()) {
      return specialIconIt->second;
    }
  }

  auto namedIconIt = icons_map.find(name());
  if (namedIconIt != icons_map.end()) {
    return namedIconIt->second;
  }

  if (isVisible()) {
    auto visibleIconIt = icons_map.find("visible");
    if (visibleIconIt != icons_map.end()) {
      return visibleIconIt->second;
    }
  }

  if (isEmpty()) {
    auto emptyIconIt = icons_map.find("empty");
    if (emptyIconIt != icons_map.end()) {
      return emptyIconIt->second;
    }
  }

  if (isPersistent()) {
    auto persistentIconIt = icons_map.find("persistent");
    if (persistentIconIt != icons_map.end()) {
      return persistentIconIt->second;
    }
  }

  auto defaultIconIt = icons_map.find("default");
  if (defaultIconIt != icons_map.end()) {
    return defaultIconIt->second;
  }

  return m_name;
}

void Workspace::update(const std::string &workspace_icon) {
  static bool first_call = true;
  if (first_call) {
    spdlog::info("[WICONS] First workspace update call - workspace icons feature active");
    first_call = false;
  }
  
  if (this->m_workspaceManager.persistentOnly() && !this->isPersistent()) {
    m_button.hide();
    return;
  }
  // clang-format off
  if (this->m_workspaceManager.activeOnly() && \
     !this->isActive() && \
     !this->isPersistent() && \
     !this->isVisible() && \
     !this->isSpecial()) {
    // clang-format on
    // if activeOnly is true, hide if not active, persistent, visible or special
    m_button.hide();
    return;
  }
  if (this->m_workspaceManager.specialVisibleOnly() && this->isSpecial() && !this->isVisible()) {
    m_button.hide();
    return;
  }
  m_button.show();

  auto styleContext = m_button.get_style_context();
  addOrRemoveClass(styleContext, isActive(), "active");
  addOrRemoveClass(styleContext, isSpecial(), "special");
  addOrRemoveClass(styleContext, isEmpty(), "empty");
  addOrRemoveClass(styleContext, isPersistent(), "persistent");
  addOrRemoveClass(styleContext, isUrgent(), "urgent");
  addOrRemoveClass(styleContext, isVisible(), "visible");
  addOrRemoveClass(styleContext, m_workspaceManager.getBarOutput() == output(), "hosting-monitor");

  std::string windows;
  // Optimization: The {windows} substitution string is only possible if the taskbar is disabled, no
  // need to compute this if enableTaskbar() is true
  if (!m_workspaceManager.enableTaskbar()) {
    auto windowSeparator = m_workspaceManager.getWindowSeparator();

    bool isNotFirst = false;

    for (const auto &window_repr : m_windowMap) {
      if (isNotFirst) {
        windows.append(windowSeparator);
      }
      isNotFirst = true;
      windows.append(window_repr.repr_rewrite);
    }
  }

  auto formatBefore = m_workspaceManager.formatBefore();
  m_labelBefore.set_markup(fmt::format(fmt::runtime(formatBefore), fmt::arg("id", id()),
                                       fmt::arg("name", name()), fmt::arg("icon", workspace_icon),
                                       fmt::arg("windows", windows)));
  m_labelBefore.get_style_context()->add_class("workspace-label");

  if (m_workspaceManager.enableTaskbar()) {
    updateTaskbar(workspace_icon);
  }
  
  // Render window icons based on configuration
  updateWindowIcons();
}

void Workspace::updateWindowIcons() {
  // Clear ALL children from IconBox (fixes EventBox accumulation bug)
  // Previous code only removed images from m_iconImages vector but left EventBox containers
  for (auto* child : m_iconBox.get_children()) {
    m_iconBox.remove(*child);
    delete child;
  }
  m_iconImages.clear();
  m_iconBox.hide();

  auto showMode = m_workspaceManager.showWindowIcons();
  
  if (showMode == Workspaces::ShowWindowIcons::NONE) {
    return;
  }

  // Check if we should show icons based on current-group logic
  if (showMode == Workspaces::ShowWindowIcons::CURRENT_GROUP) {
    // Show icons only if this workspace is in the active group
    if (!m_workspaceManager.isWorkspaceInActiveGroup(m_name)) {
      return;
    }
  }

  int icon_size = m_workspaceManager.windowIconSize();
  
  // Collect window icons, titles, and addresses (deduplicate icons, collect all data)
  std::set<std::string> unique_icons;
  std::vector<std::string> icon_names_ordered;
  std::map<std::string, std::vector<std::string>> icon_to_titles;
  std::map<std::string, std::vector<std::string>> icon_to_addresses;
  
  for (const auto& window : m_windowMap) {
    if (shouldSkipWindow(window)) {
      continue;
    }
    
    auto icon_name_opt = getIconName(window.window_class, "");
    if (icon_name_opt.has_value()) {
      std::string icon_name = icon_name_opt.value();
      
      // Add to ordered list if first occurrence
      if (unique_icons.find(icon_name) == unique_icons.end()) {
        unique_icons.insert(icon_name);
        icon_names_ordered.push_back(icon_name);
      }
      
      // Collect window title and address for tooltip and click handler
      icon_to_titles[icon_name].push_back(window.window_title);
      icon_to_addresses[icon_name].push_back(window.address);
    }
  }

  // Create and add icon images
  for (const auto& icon_name : icon_names_ordered) {
    auto* img = new Gtk::Image();
    img->set_pixel_size(icon_size);
    
    if (icon_name.front() == '/') {
      // File path - load from file
      try {
        auto pixbuf = Gdk::Pixbuf::create_from_file(icon_name, icon_size, icon_size);
        img->set(pixbuf);
      } catch (const Glib::Error& e) {
        spdlog::warn("[WICONS] Failed to load icon from file {}: {}", icon_name, e.what().c_str());
        delete img;
        continue;
      }
    } else {
      // Icon name - load from theme
      img->set_from_icon_name(icon_name, Gtk::ICON_SIZE_INVALID);
    }
    
    // Build tooltip from window titles
    const auto& titles = icon_to_titles[icon_name];
    std::string tooltip;
    if (titles.size() == 1) {
      // Single window: just show the title
      tooltip = titles[0];
    } else {
      // Multiple windows: show as list
      tooltip = icon_name + ":\n";
      for (const auto& title : titles) {
        tooltip += "• " + title + "\n";
      }
      // Remove trailing newline
      if (!tooltip.empty() && tooltip.back() == '\n') {
        tooltip.pop_back();
      }
    }
    
    // Wrap icon in EventBox to capture clicks
    auto* eventBox = new Gtk::EventBox();
    eventBox->add(*img);
    eventBox->set_tooltip_text(tooltip);
    
    // Add click handler to focus the first window
    const auto& addresses = icon_to_addresses[icon_name];
    if (!addresses.empty()) {
      std::string firstWindowAddress = addresses[0];
      eventBox->signal_button_press_event().connect([this, firstWindowAddress, icon_name](GdkEventButton* event) -> bool {
        if (event->button == 1) {  // Left click
          spdlog::debug("[WICONS] Icon clicked, focusing window: {}", firstWindowAddress);
          std::string response = m_workspaceManager.getIpc().getSocket1Reply("dispatch focuswindow address:0x" + firstWindowAddress);
          if (response.find("ok") == std::string::npos && !response.empty()) {
            spdlog::debug("[WICONS] Hyprland response: '{}'", response);
          }
          return true;
        }
        return false;
      });
    }
    
    eventBox->show();
    img->show();
    m_iconBox.pack_start(*eventBox, false, false);
    m_iconImages.push_back(img);
  }

  if (!m_iconImages.empty()) {
    m_iconBox.show();
  }
}

bool Workspace::isEmpty() const {
  auto ignore_list = m_workspaceManager.getIgnoredWindows();
  if (ignore_list.empty()) {
    return m_windows == 0;
  }
  // If there are windows but they are all ignored, consider the workspace empty
  return std::all_of(
      m_windowMap.begin(), m_windowMap.end(),
      [this, &ignore_list](const auto &window_repr) { return shouldSkipWindow(window_repr); });
}

void Workspace::setLabelText(const std::string& text) {
  m_labelBefore.set_markup(text);
}

std::vector<std::string> Workspace::getWindowClasses() const {
  std::vector<std::string> classes;
  for (const auto& window : m_windowMap) {
    if (!shouldSkipWindow(window) && !window.window_class.empty()) {
      classes.push_back(window.window_class);
    }
  }
  return classes;
}

std::vector<Workspace::WindowInfo> Workspace::getWindows() const {
  std::vector<WindowInfo> windows;
  for (const auto& window : m_windowMap) {
    if (!shouldSkipWindow(window)) {
      windows.push_back({window.window_class, window.window_title, window.address});
    }
  }
  return windows;
}

void Workspace::updateTaskbar(const std::string &workspace_icon) {
  for (auto child : m_content.get_children()) {
    if (child != &m_labelBefore) {
      m_content.remove(*child);
    }
  }

  bool isFirst = true;
  auto processWindow = [&](const WindowRepr &window_repr) {
    if (shouldSkipWindow(window_repr)) {
      return;  // skip
    }
    if (isFirst) {
      isFirst = false;
    } else if (m_workspaceManager.getWindowSeparator() != "") {
      auto windowSeparator = Gtk::make_managed<Gtk::Label>(m_workspaceManager.getWindowSeparator());
      m_content.pack_start(*windowSeparator, false, false);
      windowSeparator->show();
    }

    auto window_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL);
    window_box->set_tooltip_text(window_repr.window_title);
    window_box->get_style_context()->add_class("taskbar-window");
    if (window_repr.isActive) {
      window_box->get_style_context()->add_class("active");
    }
    auto event_box = Gtk::manage(new Gtk::EventBox());
    event_box->add(*window_box);
    if (m_workspaceManager.onClickWindow() != "") {
      event_box->signal_button_press_event().connect(
          sigc::bind(sigc::mem_fun(*this, &Workspace::handleClick), window_repr.address));
    }

    auto text_before = fmt::format(fmt::runtime(m_workspaceManager.taskbarFormatBefore()),
                                   fmt::arg("title", window_repr.window_title));
    if (!text_before.empty()) {
      auto window_label_before = Gtk::make_managed<Gtk::Label>(text_before);
      window_box->pack_start(*window_label_before, true, true);
    }

    if (m_workspaceManager.taskbarWithIcon()) {
      auto app_info_ = IconLoader::get_app_info_from_app_id_list(window_repr.window_class);
      int icon_size = m_workspaceManager.taskbarIconSize();
      auto window_icon = Gtk::make_managed<Gtk::Image>();
      m_workspaceManager.iconLoader().image_load_icon(*window_icon, app_info_, icon_size);
      window_box->pack_start(*window_icon, false, false);
    }

    auto text_after = fmt::format(fmt::runtime(m_workspaceManager.taskbarFormatAfter()),
                                  fmt::arg("title", window_repr.window_title));
    if (!text_after.empty()) {
      auto window_label_after = Gtk::make_managed<Gtk::Label>(text_after);
      window_box->pack_start(*window_label_after, true, true);
    }

    m_content.pack_start(*event_box, true, false);
    event_box->show_all();
  };

  if (m_workspaceManager.taskbarReverseDirection()) {
    for (auto it = m_windowMap.rbegin(); it != m_windowMap.rend(); ++it) {
      processWindow(*it);
    }
  } else {
    for (const auto &window_repr : m_windowMap) {
      processWindow(window_repr);
    }
  }

  auto formatAfter = m_workspaceManager.formatAfter();
  if (!formatAfter.empty()) {
    m_labelAfter.set_markup(fmt::format(fmt::runtime(formatAfter), fmt::arg("id", id()),
                                        fmt::arg("name", name()),
                                        fmt::arg("icon", workspace_icon)));
    m_content.pack_end(m_labelAfter, false, false);
    m_labelAfter.show();
  }
}

bool Workspace::handleClick(const GdkEventButton *event_button, WindowAddress const &addr) const {
  if (event_button->type == GDK_BUTTON_PRESS) {
    std::string command = std::regex_replace(m_workspaceManager.onClickWindow(),
                                             std::regex("\\{address\\}"), "0x" + addr);
    command = std::regex_replace(command, std::regex("\\{button\\}"),
                                 std::to_string(event_button->button));
    auto res = util::command::execNoRead(command);
    if (res.exit_code != 0) {
      spdlog::error("Failed to execute {}: {}", command, res.out);
    }
  }
  return true;
}

bool Workspace::shouldSkipWindow(const WindowRepr &window_repr) const {
  auto ignore_list = m_workspaceManager.getIgnoredWindows();
  auto it = std::ranges::find_if(ignore_list, [&window_repr](const auto &ignoreItem) {
    return std::regex_match(window_repr.window_class, ignoreItem) ||
           std::regex_match(window_repr.window_title, ignoreItem);
  });
  return it != ignore_list.end();
}

}  // namespace waybar::modules::hyprland
