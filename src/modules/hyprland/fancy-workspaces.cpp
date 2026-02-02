#include "modules/hyprland/fancy-workspaces.hpp"

#include <gdkmm/pixbuf.h>
#include <glibmm/fileutils.h>
#include <glibmm/keyfile.h>
#include <glibmm/miscutils.h>
#include <json/value.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "util/command.hpp"
#include "util/gtk_icon.hpp"
#include "util/regex_collection.hpp"
#include "util/string.hpp"

namespace waybar::modules::hyprland {

FancyWorkspaces::FancyWorkspaces(const std::string& id, const Bar& bar, const Json::Value& config)
    : AModule(config, "workspaces", id, false, false),
      m_bar(bar),
      m_box(bar.orientation, 0),
      m_ipc(IPC::inst()) {
  parseConfig(config);

  m_box.set_name("workspaces");
  if (!id.empty()) {
    m_box.get_style_context()->add_class(id);
  }
  m_box.get_style_context()->add_class(MODULE_CLASS);
  event_box_.add(m_box);

  // Clean up old thumbnail cache on startup
  spdlog::info("Cleaning up thumbnail cache on startup");
  m_thumbnailCache.cleanup(0, 100);  // Remove all thumbnails

  setCurrentMonitorId();
  init();
  registerIpc();
}

FancyWorkspaces::~FancyWorkspaces() {
  m_ipc.unregisterForIPC(this);
  // wait for possible event handler to finish
  std::lock_guard<std::mutex> lg(m_mutex);
}

void FancyWorkspaces::init() {
  m_activeWorkspaceId = m_ipc.getSocket1JsonReply("activeworkspace")["id"].asInt();

  initializeWorkspaces();
  dp.emit();
}

Json::Value FancyWorkspaces::createMonitorWorkspaceData(std::string const& name,
                                                   std::string const& monitor) {
  spdlog::trace("Creating persistent workspace: {} on monitor {}", name, monitor);
  Json::Value workspaceData;

  auto workspaceId = parseWorkspaceId(name);
  if (!workspaceId.has_value()) {
    workspaceId = 0;
  }
  workspaceData["id"] = *workspaceId;
  workspaceData["name"] = name;
  workspaceData["monitor"] = monitor;
  workspaceData["windows"] = 0;
  return workspaceData;
}

void FancyWorkspaces::createWorkspace(Json::Value const& workspace_data,
                                 Json::Value const& clients_data) {
  auto workspaceName = workspace_data["name"].asString();
  auto workspaceId = workspace_data["id"].asInt();

  // Skip workspaces with ID 0 (these are workspace rules like "n[s:.]", not real workspaces)
  if (workspaceId == 0) {
    spdlog::debug("Workspace '{}' skipped: invalid id {}", workspaceName, workspaceId);
    return;
  }

  spdlog::debug("Creating workspace {}", workspaceName);

  // avoid recreating existing workspaces
  auto workspace =
      std::ranges::find_if(m_workspaces, [&](std::unique_ptr<FancyWorkspace> const& w) {
        if (workspaceId > 0) {
          return w->id() == workspaceId;
        }
        return (workspaceName.starts_with("special:") && workspaceName.substr(8) == w->name()) ||
               workspaceName == w->name();
      });

  if (workspace != m_workspaces.end()) {
    // don't recreate workspace, but update persistency if necessary
    const auto keys = workspace_data.getMemberNames();

    const auto* k = "persistent-rule";
    if (std::ranges::find(keys, k) != keys.end()) {
      spdlog::debug("Set dynamic persistency of workspace {} to: {}", workspaceName,
                    workspace_data[k].asBool() ? "true" : "false");
      (*workspace)->setPersistentRule(workspace_data[k].asBool());
    }

    k = "persistent-config";
    if (std::ranges::find(keys, k) != keys.end()) {
      spdlog::debug("Set config persistency of workspace {} to: {}", workspaceName,
                    workspace_data[k].asBool() ? "true" : "false");
      (*workspace)->setPersistentConfig(workspace_data[k].asBool());
    }

    return;
  }

  // create new workspace
  m_workspaces.emplace_back(std::make_unique<FancyWorkspace>(workspace_data, *this, clients_data));
  Gtk::Button& newWorkspaceButton = m_workspaces.back()->button();
  m_box.pack_start(newWorkspaceButton, false, false);
  sortWorkspaces();
  newWorkspaceButton.show_all();
}

void FancyWorkspaces::createWorkspacesToCreate() {
  for (const auto& [workspaceData, clientsData] : m_workspacesToCreate) {
    createWorkspace(workspaceData, clientsData);
  }
  if (!m_workspacesToCreate.empty()) {
    updateWindowCount();
    sortWorkspaces();
  }
  m_workspacesToCreate.clear();
}

/**
 *  FancyWorkspaces::doUpdate - update workspaces in UI thread.
 *
 * Note: some memberfields are modified by both UI thread and event listener thread, use m_mutex to
 *       protect these member fields, and lock should released before calling AModule::update().
 */
void FancyWorkspaces::doUpdate() {
  std::unique_lock lock(m_mutex);

  removeWorkspacesToRemove();
  createWorkspacesToCreate();
  updateWorkspaceStates();
  updateWindowCount();
  sortWorkspaces();
  applyProjectCollapsing();

  bool anyWindowCreated = updateWindowsToCreate();

  if (anyWindowCreated) {
    dp.emit();
  }
}

void FancyWorkspaces::extendOrphans(int workspaceId, Json::Value const& clientsJson) {
  spdlog::trace("Extending orphans with workspace {}", workspaceId);
  for (const auto& client : clientsJson) {
    if (client["workspace"]["id"].asInt() == workspaceId) {
      registerOrphanWindow({client});
    }
  }
}

std::string FancyWorkspaces::getRewrite(std::string window_class, std::string window_title) {
  std::string windowReprKey;
  if (windowRewriteConfigUsesTitle()) {
    windowReprKey = fmt::format("class<{}> title<{}>", window_class, window_title);
  } else {
    windowReprKey = fmt::format("class<{}>", window_class);
  }
  auto const rewriteRule = m_windowRewriteRules.get(windowReprKey);
  return fmt::format(fmt::runtime(rewriteRule), fmt::arg("class", window_class),
                     fmt::arg("title", window_title));
}

std::vector<int> FancyWorkspaces::getVisibleWorkspaces() {
  std::vector<int> visibleWorkspaces;
  auto monitors = IPC::inst().getSocket1JsonReply("monitors");
  for (const auto& monitor : monitors) {
    auto ws = monitor["activeWorkspace"];
    if (ws.isObject() && ws["id"].isInt()) {
      visibleWorkspaces.push_back(ws["id"].asInt());
    }
    auto sws = monitor["specialWorkspace"];
    auto name = sws["name"].asString();
    if (sws.isObject() && sws["id"].isInt() && !name.empty()) {
      visibleWorkspaces.push_back(sws["id"].asInt());
    }
  }
  return visibleWorkspaces;
}

void FancyWorkspaces::initializeWorkspaces() {
  spdlog::debug("Initializing workspaces");

  // if the workspace rules changed since last initialization, make sure we reset everything:
  for (auto& workspace : m_workspaces) {
    m_workspacesToRemove.push_back(std::to_string(workspace->id()));
  }

  // get all current workspaces
  auto const workspacesJson = m_ipc.getSocket1JsonReply("workspaces");
  auto const clientsJson = m_ipc.getSocket1JsonReply("clients");

  for (Json::Value workspaceJson : workspacesJson) {
    std::string workspaceName = workspaceJson["name"].asString();
    if ((allOutputs() || m_bar.output->name == workspaceJson["monitor"].asString()) &&
        (!workspaceName.starts_with("special") || showSpecial()) &&
        !isWorkspaceIgnored(workspaceName)) {
      m_workspacesToCreate.emplace_back(workspaceJson, clientsJson);
    } else {
      extendOrphans(workspaceJson["id"].asInt(), clientsJson);
    }
  }

  spdlog::debug("Initializing persistent workspaces");
  if (m_persistentWorkspaceConfig.isObject()) {
    // a persistent workspace config is defined, so use that instead of workspace rules
    loadPersistentWorkspacesFromConfig(clientsJson);
  }
  // load Hyprland's workspace rules
  loadPersistentWorkspacesFromWorkspaceRules(clientsJson);
}

namespace {
bool isDoubleSpecial(std::string const& workspace_name) {
  // Hyprland's IPC sometimes reports the creation of workspaces strangely named
  // `special:special:<some_name>`. This function checks for that and is used
  // to avoid creating (and then removing) such workspaces.
  // See hyprwm/Hyprland#3424 for more info.
  return workspace_name.find("special:special:") != std::string::npos;
}
}  // namespace

bool FancyWorkspaces::isWorkspaceIgnored(std::string const& name) {
  for (auto& rule : m_ignoreWorkspaces) {
    if (std::regex_match(name, rule)) {
      return true;
      break;
    }
  }

  return false;
}

void FancyWorkspaces::loadPersistentWorkspacesFromConfig(Json::Value const& clientsJson) {
  spdlog::info("Loading persistent workspaces from Waybar config");
  const std::vector<std::string> keys = m_persistentWorkspaceConfig.getMemberNames();
  std::vector<std::string> persistentWorkspacesToCreate;

  const std::string currentMonitor = m_bar.output->name;
  const bool monitorInConfig = std::ranges::find(keys, currentMonitor) != keys.end();
  for (const std::string& key : keys) {
    // only add if either:
    // 1. key is the current monitor name
    // 2. key is "*" and this monitor is not already defined in the config
    bool canCreate = key == currentMonitor || (key == "*" && !monitorInConfig);
    const Json::Value& value = m_persistentWorkspaceConfig[key];
    spdlog::trace("Parsing persistent workspace config: {} => {}", key, value.toStyledString());

    if (value.isInt()) {
      // value is a number => create that many workspaces for this monitor
      if (canCreate) {
        int amount = value.asInt();
        spdlog::debug("Creating {} persistent workspaces for monitor {}", amount, currentMonitor);
        for (int i = 0; i < amount; i++) {
          persistentWorkspacesToCreate.emplace_back(std::to_string((m_monitorId * amount) + i + 1));
        }
      }
    } else if (value.isArray() && !value.empty()) {
      // value is an array => create defined workspaces for this monitor
      if (canCreate) {
        for (const Json::Value& workspace : value) {
          spdlog::debug("Creating workspace {} on monitor {}", workspace, currentMonitor);
          persistentWorkspacesToCreate.emplace_back(workspace.asString());
        }
      } else {
        // key is the workspace and value is array of monitors to create on
        for (const Json::Value& monitor : value) {
          if (monitor.isString() && monitor.asString() == currentMonitor) {
            persistentWorkspacesToCreate.emplace_back(currentMonitor);
            break;
          }
        }
      }
    } else {
      // this workspace should be displayed on all monitors
      persistentWorkspacesToCreate.emplace_back(key);
    }
  }

  for (auto const& workspace : persistentWorkspacesToCreate) {
    auto workspaceData = createMonitorWorkspaceData(workspace, m_bar.output->name);
    workspaceData["persistent-config"] = true;
    m_workspacesToCreate.emplace_back(workspaceData, clientsJson);
  }
}

void FancyWorkspaces::loadPersistentWorkspacesFromWorkspaceRules(const Json::Value& clientsJson) {
  spdlog::info("Loading persistent workspaces from Hyprland workspace rules");

  auto const workspaceRules = m_ipc.getSocket1JsonReply("workspacerules");
  for (Json::Value const& rule : workspaceRules) {
    if (!rule["workspaceString"].isString()) {
      spdlog::warn("Workspace rules: invalid workspaceString, skipping: {}", rule);
      continue;
    }
    if (!rule["persistent"].asBool()) {
      continue;
    }
    auto workspace = rule.isMember("defaultName") ? rule["defaultName"].asString()
                                                  : rule["workspaceString"].asString();

    // There could be persistent special workspaces, only show those when show-special is enabled.
    if (workspace.starts_with("special:") && !showSpecial()) {
      continue;
    }

    // The prefix "name:" cause mismatches with workspace names taken anywhere else.
    if (workspace.starts_with("name:")) {
      workspace = workspace.substr(5);
    }
    auto const& monitor = rule["monitor"].asString();
    // create this workspace persistently if:
    // 1. the allOutputs config option is enabled
    // 2. the rule's monitor is the current monitor
    // 3. no monitor is specified in the rule => assume it needs to be persistent on every monitor
    if (allOutputs() || m_bar.output->name == monitor || monitor.empty()) {
      // => persistent workspace should be shown on this monitor
      auto workspaceData = createMonitorWorkspaceData(workspace, m_bar.output->name);
      workspaceData["persistent-rule"] = true;
      m_workspacesToCreate.emplace_back(workspaceData, clientsJson);
    } else {
      // This can be any workspace selector.
      m_workspacesToRemove.emplace_back(workspace);
    }
  }
}

void FancyWorkspaces::onEvent(const std::string& ev) {
  std::lock_guard<std::mutex> lock(m_mutex);
  std::string eventName(begin(ev), begin(ev) + ev.find_first_of('>'));
  std::string payload = ev.substr(eventName.size() + 2);

  if (eventName == "workspacev2") {
    onWorkspaceActivated(payload);
  } else if (eventName == "activespecial") {
    onSpecialWorkspaceActivated(payload);
  } else if (eventName == "destroyworkspacev2") {
    onWorkspaceDestroyed(payload);
  } else if (eventName == "createworkspacev2") {
    onWorkspaceCreated(payload);
  } else if (eventName == "focusedmonv2") {
    onMonitorFocused(payload);
  } else if (eventName == "moveworkspacev2") {
    onWorkspaceMoved(payload);
  } else if (eventName == "openwindow") {
    onWindowOpened(payload);
  } else if (eventName == "closewindow") {
    onWindowClosed(payload);
  } else if (eventName == "movewindowv2") {
    onWindowMoved(payload);
  } else if (eventName == "urgent") {
    setUrgentWorkspace(payload);
  } else if (eventName == "renameworkspace") {
    onWorkspaceRenamed(payload);
  } else if (eventName == "windowtitlev2") {
    onWindowTitleEvent(payload);
  } else if (eventName == "activewindowv2") {
    onActiveWindowChanged(payload);
  } else if (eventName == "configreloaded") {
    onConfigReloaded();
  }

  dp.emit();
}

void FancyWorkspaces::onWorkspaceActivated(std::string const& payload) {
  const auto [workspaceIdStr, workspaceName] = splitDoublePayload(payload);
  const auto workspaceId = parseWorkspaceId(workspaceIdStr);
  if (workspaceId.has_value()) {
    m_activeWorkspaceId = *workspaceId;
    
    // No need to kill old capture process - it validates workspace before committing
    m_captureProcessPid = 0;
    
    // Start background capture for all windows in this workspace
    auto workspace = std::find_if(m_workspaces.begin(), m_workspaces.end(),
                                   [&workspaceName](const auto& ws) { return ws->name() == workspaceName; });
    if (workspace != m_workspaces.end()) {
      captureThumbnailsForWorkspace(workspaceName);
    }

    // Track last active workspace per group for collapsed button behavior
    auto prefix = extractProjectPrefix(workspaceName);
    if (prefix) {
      // Find the workspace object to get its actual monitor
      auto workspaceIt =
          std::find_if(m_workspaces.begin(), m_workspaces.end(),
                       [&workspaceName](const auto& ws) { return ws->name() == workspaceName; });

      if (workspaceIt != m_workspaces.end()) {
        // Only track history if workspace is on this bar's monitor
        std::string workspaceMonitor = (*workspaceIt)->output();
        std::string barMonitor = getBarOutput();

        if (workspaceMonitor == barMonitor) {
          std::string key = *prefix + "@" + barMonitor;
          m_lastActivePerGroup[key] = workspaceName;
          spdlog::trace("Tracked last active workspace: {} for key {}", workspaceName, key);
        }
      }
    }
  }
}

void FancyWorkspaces::onSpecialWorkspaceActivated(std::string const& payload) {
  std::string name(begin(payload), begin(payload) + payload.find_first_of(','));
  m_activeSpecialWorkspaceName = (!name.starts_with("special:") ? name : name.substr(8));
}

void FancyWorkspaces::onWorkspaceDestroyed(std::string const& payload) {
  const auto [workspaceId, workspaceName] = splitDoublePayload(payload);
  if (!isDoubleSpecial(workspaceName)) {
    m_workspacesToRemove.push_back(workspaceId);
    
    // Execute on-workspace-destroyed hook
    if (!m_onWorkspaceDestroyed.empty()) {
      executeHook(m_onWorkspaceDestroyed, workspaceName, "", 0);
    }
  }
}

void FancyWorkspaces::onWorkspaceCreated(std::string const& payload, Json::Value const& clientsData) {
  spdlog::debug("Workspace created: {}", payload);

  const auto [workspaceIdStr, _] = splitDoublePayload(payload);

  const auto workspaceId = parseWorkspaceId(workspaceIdStr);
  if (!workspaceId.has_value()) {
    return;
  }

  auto const workspaceRules = m_ipc.getSocket1JsonReply("workspacerules");
  auto const workspacesJson = m_ipc.getSocket1JsonReply("workspaces");

  for (Json::Value workspaceJson : workspacesJson) {
    const auto currentId = workspaceJson["id"].asInt();
    if (currentId == *workspaceId) {
      std::string workspaceName = workspaceJson["name"].asString();
      // This workspace name is more up-to-date than the one in the event payload.
      if (isWorkspaceIgnored(workspaceName)) {
        spdlog::trace("Not creating workspace because it is ignored: id={} name={}", *workspaceId,
                      workspaceName);
        break;
      }

      if ((allOutputs() || m_bar.output->name == workspaceJson["monitor"].asString()) &&
          (showSpecial() || !workspaceName.starts_with("special")) &&
          !isDoubleSpecial(workspaceName)) {
        for (Json::Value const& rule : workspaceRules) {
          auto ruleWorkspaceName = rule.isMember("defaultName")
                                       ? rule["defaultName"].asString()
                                       : rule["workspaceString"].asString();
          if (ruleWorkspaceName == workspaceName) {
            workspaceJson["persistent-rule"] = rule["persistent"].asBool();
            break;
          }
        }

        m_workspacesToCreate.emplace_back(workspaceJson, clientsData);
        
        // Execute on-workspace-created hook
        if (!m_onWorkspaceCreated.empty()) {
          std::string monitor = workspaceJson["monitor"].asString();
          executeHook(m_onWorkspaceCreated, workspaceName, monitor, currentId);
        }
        
        break;
      }
    } else {
      extendOrphans(*workspaceId, clientsData);
    }
  }
}

void FancyWorkspaces::onWorkspaceMoved(std::string const& payload) {
  spdlog::debug("Workspace moved: {}", payload);

  // Update active workspace
  m_activeWorkspaceId = (m_ipc.getSocket1JsonReply("activeworkspace"))["id"].asInt();

  if (allOutputs()) return;

  const auto [workspaceIdStr, workspaceName, monitorName] = splitTriplePayload(payload);

  const auto subPayload = makePayload(workspaceIdStr, workspaceName);

  if (m_bar.output->name == monitorName) {
    Json::Value clientsData = m_ipc.getSocket1JsonReply("clients");
    onWorkspaceCreated(subPayload, clientsData);
  } else {
    spdlog::debug("Removing workspace because it was moved to another monitor: {}", subPayload);
    onWorkspaceDestroyed(subPayload);
  }
}

void FancyWorkspaces::onWorkspaceRenamed(std::string const& payload) {
  spdlog::debug("Workspace renamed: {}", payload);
  const auto [workspaceIdStr, newName] = splitDoublePayload(payload);

  const auto workspaceId = parseWorkspaceId(workspaceIdStr);
  if (!workspaceId.has_value()) {
    return;
  }

  for (auto& workspace : m_workspaces) {
    if (workspace->id() == *workspaceId) {
      workspace->setName(newName);
      break;
    }
  }
  sortWorkspaces();
}

void FancyWorkspaces::onMonitorFocused(std::string const& payload) {
  spdlog::trace("Monitor focused: {}", payload);

  const auto [monitorName, workspaceIdStr] = splitDoublePayload(payload);

  const auto workspaceId = parseWorkspaceId(workspaceIdStr);
  if (!workspaceId.has_value()) {
    return;
  }

  m_activeWorkspaceId = *workspaceId;

  for (Json::Value& monitor : m_ipc.getSocket1JsonReply("monitors")) {
    if (monitor["name"].asString() == monitorName) {
      const auto name = monitor["specialWorkspace"]["name"].asString();
      m_activeSpecialWorkspaceName = !name.starts_with("special:") ? name : name.substr(8);
    }
  }
}

void FancyWorkspaces::onWindowOpened(std::string const& payload) {
  spdlog::trace("Window opened: {}", payload);
  updateWindowCount();
  size_t lastCommaIdx = 0;
  size_t nextCommaIdx = payload.find(',');
  std::string windowAddress = payload.substr(lastCommaIdx, nextCommaIdx - lastCommaIdx);

  lastCommaIdx = nextCommaIdx;
  nextCommaIdx = payload.find(',', nextCommaIdx + 1);
  std::string workspaceName = payload.substr(lastCommaIdx + 1, nextCommaIdx - lastCommaIdx - 1);

  lastCommaIdx = nextCommaIdx;
  nextCommaIdx = payload.find(',', nextCommaIdx + 1);
  std::string windowClass = payload.substr(lastCommaIdx + 1, nextCommaIdx - lastCommaIdx - 1);

  std::string windowTitle = payload.substr(nextCommaIdx + 1, payload.length() - nextCommaIdx);

  bool isActive = m_currentActiveWindowAddress == windowAddress;
  m_windowsToCreate.emplace_back(workspaceName, windowAddress, windowClass, windowTitle, isActive);
}

void FancyWorkspaces::onWindowClosed(std::string const& addr) {
  spdlog::trace("Window closed: {}", addr);
  updateWindowCount();
  m_orphanWindowMap.erase(addr);
  for (auto& workspace : m_workspaces) {
    if (workspace->closeWindow(addr)) {
      break;
    }
  }
}

void FancyWorkspaces::onWindowMoved(std::string const& payload) {
  spdlog::trace("Window moved: {}", payload);
  updateWindowCount();
  auto [windowAddress, _, workspaceName] = splitTriplePayload(payload);

  FancyWindowRepr windowRepr;

  // If the window was still queued to be created, just change its destination
  // and exit
  for (auto& window : m_windowsToCreate) {
    if (window.getAddress() == windowAddress) {
      window.moveToWorkspace(workspaceName);
      return;
    }
  }

  // Take the window's representation from the old workspace...
  for (auto& workspace : m_workspaces) {
    if (auto windowAddr = workspace->closeWindow(windowAddress); windowAddr != std::nullopt) {
      windowRepr = windowAddr.value();
      break;
    }
  }

  // ...if it was empty, check if the window is an orphan...
  if (windowRepr.empty() && m_orphanWindowMap.contains(windowAddress)) {
    windowRepr = m_orphanWindowMap[windowAddress];
  }

  // ...and then add it to the new workspace
  if (!windowRepr.empty()) {
    m_orphanWindowMap.erase(windowAddress);
    m_windowsToCreate.emplace_back(workspaceName, windowAddress, windowRepr);
  }
}

void FancyWorkspaces::onWindowTitleEvent(std::string const& payload) {
  spdlog::trace("Window title changed: {}", payload);
  std::optional<std::function<void(FancyWindowCreationPayload)>> inserter;

  const auto [windowAddress, _] = splitDoublePayload(payload);

  // If the window was an orphan, rename it at the orphan's vector
  if (m_orphanWindowMap.contains(windowAddress)) {
    inserter = [this](FancyWindowCreationPayload wcp) { this->registerOrphanWindow(std::move(wcp)); };
  } else {
    auto windowWorkspace = std::ranges::find_if(m_workspaces, [windowAddress](auto& workspace) {
      return workspace->containsWindow(windowAddress);
    });

    // If the window exists on a workspace, rename it at the workspace's window
    // map
    if (windowWorkspace != m_workspaces.end()) {
      inserter = [windowWorkspace](FancyWindowCreationPayload wcp) {
        (*windowWorkspace)->insertWindow(std::move(wcp));
      };
    } else {
      auto queuedWindow =
          std::ranges::find_if(m_windowsToCreate, [&windowAddress](auto& windowPayload) {
            return windowPayload.getAddress() == windowAddress;
          });

      // If the window was queued, rename it in the queue
      if (queuedWindow != m_windowsToCreate.end()) {
        inserter = [queuedWindow](FancyWindowCreationPayload wcp) { *queuedWindow = std::move(wcp); };
      }
    }
  }

  if (inserter.has_value()) {
    Json::Value clientsData = m_ipc.getSocket1JsonReply("clients");
    std::string jsonWindowAddress = fmt::format("0x{}", windowAddress);

    auto client = std::ranges::find_if(clientsData, [jsonWindowAddress](auto& client) {
      return client["address"].asString() == jsonWindowAddress;
    });

    if (client != clientsData.end() && !client->empty()) {
      (*inserter)({*client});
    }
  }
}

void FancyWorkspaces::onActiveWindowChanged(WindowAddress const& activeWindowAddress) {
  spdlog::debug("[THUMBNAIL] Active window changed: {}", activeWindowAddress);
  m_currentActiveWindowAddress = activeWindowAddress;

  // Capture thumbnail of the newly active window (async)
  if (!activeWindowAddress.empty() && m_thumbnailCache.isAvailable()) {
    spdlog::debug("[THUMBNAIL] Starting capture process for {}", activeWindowAddress);
    Json::Value clientsData = m_ipc.getSocket1JsonReply("clients");
    std::string jsonWindowAddress = "0x" + activeWindowAddress;
    
    auto client = std::ranges::find_if(clientsData, [&jsonWindowAddress](auto& client) {
      return client["address"].asString() == jsonWindowAddress;
    });
    
    if (client != clientsData.end() && !client->empty()) {
      int workspaceId = (*client)["workspace"]["id"].asInt();
      
      spdlog::debug("[THUMBNAIL] Window workspace ID: {}, active workspace ID: {}", 
                    workspaceId, m_activeWorkspaceId);
      
      // Capture the window (the 300ms delay in captureWindow will let animation finish)
      int x = (*client)["at"][0].asInt();
      int y = (*client)["at"][1].asInt();
      int w = (*client)["size"][0].asInt();
      int h = (*client)["size"][1].asInt();
      std::string windowClass = (*client)["class"].asString();
      std::string windowTitle = (*client)["title"].asString();
      std::string workspaceName = (*client)["workspace"]["name"].asString();
      
      spdlog::debug("[THUMBNAIL] Capturing active window {} ({}x{} at {},{})", 
                    activeWindowAddress, w, h, x, y);
      
      // Capture async (with 300ms delay for animation)
      m_thumbnailCache.captureWindow(activeWindowAddress, x, y, w, h,
                                     windowClass, windowTitle, workspaceName);
    }
  }

  for (auto& [address, window] : m_orphanWindowMap) {
    window.setActive(address == activeWindowAddress);
  }
  for (auto const& workspace : m_workspaces) {
    workspace->setActiveWindow(activeWindowAddress);
  }
  for (auto& window : m_windowsToCreate) {
    window.setActive(window.getAddress() == activeWindowAddress);
  }
}

void FancyWorkspaces::onConfigReloaded() {
  spdlog::info("Hyprland config reloaded, reinitializing hyprland/workspaces module...");
  init();
}

auto FancyWorkspaces::parseConfig(const Json::Value& config) -> void {
  const auto& configFormat = config["format"];
  m_formatBefore = configFormat.isString() ? configFormat.asString() : "{name}";
  m_withIcon = m_formatBefore.find("{icon}") != std::string::npos;
  auto withWindows = m_formatBefore.find("{windows}") != std::string::npos;

  if (m_withIcon && m_iconsMap.empty()) {
    populateIconsMap(config["format-icons"]);
  }

  populateBoolConfig(config, "all-outputs", m_allOutputs);
  populateBoolConfig(config, "show-special", m_showSpecial);
  populateBoolConfig(config, "special-visible-only", m_specialVisibleOnly);
  populateBoolConfig(config, "persistent-only", m_persistentOnly);
  populateBoolConfig(config, "active-only", m_activeOnly);
  populateBoolConfig(config, "move-to-monitor", m_moveToMonitor);
  populateBoolConfig(config, "collapse-inactive-projects", m_collapseInactiveProjects);
  populateBoolConfig(config, "transform-workspace-names", m_transformWorkspaceNames);

  // Parse show-window-icons config
  const auto& showWindowIconsConfig = config["show-window-icons"];
  if (showWindowIconsConfig.isString()) {
    std::string value = showWindowIconsConfig.asString();
    if (value == "none") {
      m_showWindowIcons = ShowWindowIcons::NONE;
    } else if (value == "current-group") {
      m_showWindowIcons = ShowWindowIcons::CURRENT_GROUP;
    } else if (value == "all") {
      m_showWindowIcons = ShowWindowIcons::ALL;
    } else {
      spdlog::warn("[WICONS] Invalid show-window-icons value '{}', using default 'all'", value);
      m_showWindowIcons = ShowWindowIcons::CURRENT_GROUP;
    }
    spdlog::info("[WICONS] Window icons config: show-window-icons='{}' (mode={})", value,
                 static_cast<int>(m_showWindowIcons));
  } else {
    spdlog::info(
        "[WICONS] Window icons config: show-window-icons not set, using default 'all' (mode={})",
        static_cast<int>(m_showWindowIcons));
  }

  // Parse icon-size config
  if (config["icon-size"].isInt()) {
    m_windowIconSize = config["icon-size"].asInt();
    spdlog::info("[WICONS] Window icons config: icon-size={}", m_windowIconSize);
  } else {
    spdlog::info("[WICONS] Window icons config: icon-size not set, using default {}",
                 m_windowIconSize);
  }

  m_persistentWorkspaceConfig = config.get("persistent-workspaces", Json::Value());
  
  m_onWorkspaceCreated = config.get("on-workspace-created", "").asString();
  m_onWorkspaceDestroyed = config.get("on-workspace-destroyed", "").asString();
  
  if (!m_onWorkspaceCreated.empty()) {
    spdlog::info("Workspace hook: on-workspace-created = {}", m_onWorkspaceCreated);
  }
  if (!m_onWorkspaceDestroyed.empty()) {
    spdlog::info("Workspace hook: on-workspace-destroyed = {}", m_onWorkspaceDestroyed);
  }
  
  populateSortByConfig(config);
  populateIgnoreWorkspacesConfig(config);
  populateFormatWindowSeparatorConfig(config);
  populateWindowRewriteConfig(config);

  if (withWindows) {
    populateWorkspaceTaskbarConfig(config);
  }
  if (m_enableTaskbar) {
    auto parts = split(m_formatBefore, "{windows}", 1);
    m_formatBefore = parts[0];
    m_formatAfter = parts.size() > 1 ? parts[1] : "";
  }
}

auto FancyWorkspaces::populateIconsMap(const Json::Value& formatIcons) -> void {
  for (const auto& name : formatIcons.getMemberNames()) {
    m_iconsMap.emplace(name, formatIcons[name].asString());
  }
  m_iconsMap.emplace("", "");
}

auto FancyWorkspaces::populateBoolConfig(const Json::Value& config, const std::string& key, bool& member)
    -> void {
  const auto& configValue = config[key];
  if (configValue.isBool()) {
    member = configValue.asBool();
  }
}

auto FancyWorkspaces::populateSortByConfig(const Json::Value& config) -> void {
  const auto& configSortBy = config["sort-by"];
  if (configSortBy.isString()) {
    auto sortByStr = configSortBy.asString();
    try {
      m_sortBy = m_enumParser.parseStringToEnum(sortByStr, m_sortMap);
    } catch (const std::invalid_argument& e) {
      m_sortBy = SortMethod::DEFAULT;
      spdlog::warn(
          "Invalid string representation for sort-by. Falling back to default sort method.");
    }
  }
}

auto FancyWorkspaces::populateIgnoreWorkspacesConfig(const Json::Value& config) -> void {
  auto ignoreWorkspaces = config["ignore-workspaces"];
  if (ignoreWorkspaces.isArray()) {
    for (const auto& workspaceRegex : ignoreWorkspaces) {
      if (workspaceRegex.isString()) {
        std::string ruleString = workspaceRegex.asString();
        try {
          const std::regex rule{ruleString, std::regex_constants::icase};
          m_ignoreWorkspaces.emplace_back(rule);
        } catch (const std::regex_error& e) {
          spdlog::error("Invalid rule {}: {}", ruleString, e.what());
        }
      } else {
        spdlog::error("Not a string: '{}'", workspaceRegex);
      }
    }
  }
}

auto FancyWorkspaces::populateFormatWindowSeparatorConfig(const Json::Value& config) -> void {
  const auto& formatWindowSeparator = config["format-window-separator"];
  m_formatWindowSeparator =
      formatWindowSeparator.isString() ? formatWindowSeparator.asString() : " ";
}

auto FancyWorkspaces::populateWindowRewriteConfig(const Json::Value& config) -> void {
  const auto& windowRewrite = config["window-rewrite"];
  if (!windowRewrite.isObject()) {
    spdlog::debug("window-rewrite is not defined or is not an object, using default rules.");
    return;
  }

  const auto& windowRewriteDefaultConfig = config["window-rewrite-default"];
  std::string windowRewriteDefault =
      windowRewriteDefaultConfig.isString() ? windowRewriteDefaultConfig.asString() : "?";

  m_windowRewriteRules = util::RegexCollection(
      windowRewrite, windowRewriteDefault,
      [this](std::string& window_rule) { return windowRewritePriorityFunction(window_rule); });
}

auto FancyWorkspaces::populateWorkspaceTaskbarConfig(const Json::Value& config) -> void {
  const auto& workspaceTaskbar = config["workspace-taskbar"];
  if (!workspaceTaskbar.isObject()) {
    spdlog::debug("workspace-taskbar is not defined or is not an object, using default rules.");
    return;
  }

  populateBoolConfig(workspaceTaskbar, "enable", m_enableTaskbar);
  populateBoolConfig(workspaceTaskbar, "update-active-window", m_updateActiveWindow);
  populateBoolConfig(workspaceTaskbar, "reverse-direction", m_taskbarReverseDirection);

  if (workspaceTaskbar["format"].isString()) {
    /* The user defined a format string, use it */
    std::string format = workspaceTaskbar["format"].asString();
    m_taskbarWithTitle = format.find("{title") != std::string::npos; /* {title} or {title.length} */
    auto parts = split(format, "{icon}", 1);
    m_taskbarFormatBefore = parts[0];
    if (parts.size() > 1) {
      m_taskbarWithIcon = true;
      m_taskbarFormatAfter = parts[1];
    }
  } else {
    /* The default is to only show the icon */
    m_taskbarWithIcon = true;
  }

  auto iconTheme = workspaceTaskbar["icon-theme"];
  if (iconTheme.isArray()) {
    for (auto& c : iconTheme) {
      m_iconLoader.add_custom_icon_theme(c.asString());
    }
  } else if (iconTheme.isString()) {
    m_iconLoader.add_custom_icon_theme(iconTheme.asString());
  }

  if (workspaceTaskbar["icon-size"].isInt()) {
    m_taskbarIconSize = workspaceTaskbar["icon-size"].asInt();
  }
  if (workspaceTaskbar["orientation"].isString() &&
      toLower(workspaceTaskbar["orientation"].asString()) == "vertical") {
    m_taskbarOrientation = Gtk::ORIENTATION_VERTICAL;
  }

  if (workspaceTaskbar["on-click-window"].isString()) {
    m_onClickWindow = workspaceTaskbar["on-click-window"].asString();
  }

  if (workspaceTaskbar["ignore-list"].isArray()) {
    for (auto& windowRegex : workspaceTaskbar["ignore-list"]) {
      std::string ruleString = windowRegex.asString();
      try {
        m_ignoreWindows.emplace_back(ruleString, std::regex_constants::icase);
      } catch (const std::regex_error& e) {
        spdlog::error("Invalid rule {}: {}", ruleString, e.what());
      }
    }
  }

  if (workspaceTaskbar["active-window-position"].isString()) {
    auto posStr = workspaceTaskbar["active-window-position"].asString();
    try {
      m_activeWindowPosition =
          m_activeWindowEnumParser.parseStringToEnum(posStr, m_activeWindowPositionMap);
    } catch (const std::invalid_argument& e) {
      spdlog::warn(
          "Invalid string representation for active-window-position. Falling back to 'none'.");
      m_activeWindowPosition = ActiveWindowPosition::NONE;
    }
  }
}

void FancyWorkspaces::registerOrphanWindow(FancyWindowCreationPayload create_window_payload) {
  if (!create_window_payload.isEmpty(*this)) {
    m_orphanWindowMap[create_window_payload.getAddress()] = create_window_payload.repr(*this);
  }
}

auto FancyWorkspaces::registerIpc() -> void {
  m_ipc.registerForIPC("workspacev2", this);
  m_ipc.registerForIPC("activespecial", this);
  m_ipc.registerForIPC("createworkspacev2", this);
  m_ipc.registerForIPC("destroyworkspacev2", this);
  m_ipc.registerForIPC("focusedmonv2", this);
  m_ipc.registerForIPC("moveworkspacev2", this);
  m_ipc.registerForIPC("renameworkspace", this);
  m_ipc.registerForIPC("openwindow", this);
  m_ipc.registerForIPC("closewindow", this);
  m_ipc.registerForIPC("movewindowv2", this);
  m_ipc.registerForIPC("urgent", this);
  m_ipc.registerForIPC("configreloaded", this);

  if (windowRewriteConfigUsesTitle() || m_taskbarWithTitle || m_showWindowIcons != ShowWindowIcons::NONE) {
    spdlog::info(
        "Registering for Hyprland's 'windowtitlev2' events because window titles are displayed "
        "(in window rewrite rules, taskbar, or icon tooltips).");
    m_ipc.registerForIPC("windowtitlev2", this);
  }
  // Always register for activewindowv2 for thumbnail capture
  spdlog::info("Registering for Hyprland's 'activewindowv2' events for thumbnail capture");
  m_ipc.registerForIPC("activewindowv2", this);
}

void FancyWorkspaces::removeWorkspacesToRemove() {
  for (const auto& workspaceString : m_workspacesToRemove) {
    removeWorkspace(workspaceString);
  }
  m_workspacesToRemove.clear();
}

void FancyWorkspaces::removeWorkspace(std::string const& workspaceString) {
  spdlog::debug("Removing workspace {}", workspaceString);

  // If this succeeds, we have a workspace ID.
  const auto workspaceId = parseWorkspaceId(workspaceString);

  std::string name;
  // TODO: At some point we want to support all workspace selectors
  // This is just a subset.
  // https://wiki.hyprland.org/Configuring/Workspace-Rules/#workspace-selectors
  if (workspaceString.starts_with("special:")) {
    name = workspaceString.substr(8);
  } else if (workspaceString.starts_with("name:")) {
    name = workspaceString.substr(5);
  } else {
    name = workspaceString;
  }

  const auto workspace =
      std::ranges::find_if(m_workspaces, [&](std::unique_ptr<FancyWorkspace>& x) {
        if (workspaceId.has_value()) {
          return *workspaceId == x->id();
        }
        return name == x->name();
      });

  if (workspace == m_workspaces.end()) {
    // happens when a workspace on another monitor is destroyed
    return;
  }

  if ((*workspace)->isPersistentConfig()) {
    spdlog::trace("Not removing config persistent workspace id={} name={}", (*workspace)->id(),
                  (*workspace)->name());
    return;
  }

  m_box.remove(workspace->get()->button());
  m_workspaces.erase(workspace);
}

void FancyWorkspaces::setCurrentMonitorId() {
  // get monitor ID from name (used by persistent workspaces)
  m_monitorId = 0;
  auto monitors = m_ipc.getSocket1JsonReply("monitors");
  auto currentMonitor = std::ranges::find_if(monitors, [this](const Json::Value& m) {
    return m["name"].asString() == m_bar.output->name;
  });
  if (currentMonitor == monitors.end()) {
    spdlog::error("Monitor '{}' does not have an ID? Using 0", m_bar.output->name);
  } else {
    m_monitorId = (*currentMonitor)["id"].asInt();
    spdlog::trace("Current monitor ID: {}", m_monitorId);
  }
}

void FancyWorkspaces::sortSpecialCentered() {
  std::vector<std::unique_ptr<FancyWorkspace>> specialWorkspaces;
  std::vector<std::unique_ptr<FancyWorkspace>> hiddenWorkspaces;
  std::vector<std::unique_ptr<FancyWorkspace>> normalWorkspaces;

  for (auto& workspace : m_workspaces) {
    if (workspace->isSpecial()) {
      specialWorkspaces.push_back(std::move(workspace));
    } else {
      if (workspace->button().is_visible()) {
        normalWorkspaces.push_back(std::move(workspace));
      } else {
        hiddenWorkspaces.push_back(std::move(workspace));
      }
    }
  }
  m_workspaces.clear();

  size_t center = normalWorkspaces.size() / 2;

  m_workspaces.insert(m_workspaces.end(), std::make_move_iterator(normalWorkspaces.begin()),
                      std::make_move_iterator(normalWorkspaces.begin() + center));

  m_workspaces.insert(m_workspaces.end(), std::make_move_iterator(specialWorkspaces.begin()),
                      std::make_move_iterator(specialWorkspaces.end()));

  m_workspaces.insert(m_workspaces.end(),
                      std::make_move_iterator(normalWorkspaces.begin() + center),
                      std::make_move_iterator(normalWorkspaces.end()));

  m_workspaces.insert(m_workspaces.end(), std::make_move_iterator(hiddenWorkspaces.begin()),
                      std::make_move_iterator(hiddenWorkspaces.end()));
}

void FancyWorkspaces::sortWorkspaces() {
  std::ranges::sort(  //
      m_workspaces, [&](std::unique_ptr<FancyWorkspace>& a, std::unique_ptr<FancyWorkspace>& b) {
        // Helper comparisons
        auto isIdLess = a->id() < b->id();
        auto isNameLess = a->name() < b->name();

        switch (m_sortBy) {
          case SortMethod::ID:
            return isIdLess;
          case SortMethod::NAME:
            return isNameLess;
          case SortMethod::NUMBER:
            try {
              return std::stoi(a->name()) < std::stoi(b->name());
            } catch (const std::invalid_argument&) {
              // Handle the exception if necessary.
              break;
            }
          case SortMethod::DEFAULT:
          default:
            // Handle the default case here.
            // normal -> named persistent -> named -> special -> named special

            // both normal (includes numbered persistent) => sort by ID
            if (a->id() > 0 && b->id() > 0) {
              return isIdLess;
            }

            // one normal, one special => normal first
            if ((a->isSpecial()) ^ (b->isSpecial())) {
              return b->isSpecial();
            }

            // only one normal, one named
            if ((a->id() > 0) ^ (b->id() > 0)) {
              return a->id() > 0;
            }

            // both special
            if (a->isSpecial() && b->isSpecial()) {
              // if one is -99 => put it last
              if (a->id() == -99 || b->id() == -99) {
                return b->id() == -99;
              }
              // both are 0 (not yet named persistents) / named specials
              // (-98 <= ID <= -1)
              return isNameLess;
            }

            // sort non-special named workspaces by name (ID <= -1377)
            return isNameLess;
            break;
        }

        // Return a default value if none of the cases match.
        return isNameLess;  // You can adjust this to your specific needs.
      });
  if (m_sortBy == SortMethod::SPECIAL_CENTERED) {
    this->sortSpecialCentered();
  }

  for (size_t i = 0; i < m_workspaces.size(); ++i) {
    m_box.reorder_child(m_workspaces[i]->button(), i);
  }
}

void FancyWorkspaces::setUrgentWorkspace(std::string const& windowaddress) {
  const Json::Value clientsJson = m_ipc.getSocket1JsonReply("clients");
  int workspaceId = -1;
  std::string fullAddress;

  for (Json::Value clientJson : clientsJson) {
    if (clientJson["address"].asString().ends_with(windowaddress)) {
      workspaceId = clientJson["workspace"]["id"].asInt();
      fullAddress = clientJson["address"].asString();
      break;
    }
  }

  // Track the specific urgent window address
  if (!fullAddress.empty()) {
    m_urgentWindows.insert(fullAddress);
    spdlog::debug("Added urgent window: {}", fullAddress);
  }

  auto workspace = std::ranges::find_if(
      m_workspaces,
      [workspaceId](std::unique_ptr<FancyWorkspace>& x) { return x->id() == workspaceId; });
  if (workspace != m_workspaces.end()) {
    workspace->get()->setUrgent();
  }
}

auto FancyWorkspaces::update() -> void {
  doUpdate();
  AModule::update();
}

void FancyWorkspaces::updateWindowCount() {
  const Json::Value workspacesJson = m_ipc.getSocket1JsonReply("workspaces");
  for (auto const& workspace : m_workspaces) {
    auto workspaceJson = std::ranges::find_if(workspacesJson, [&](Json::Value const& x) {
      return x["name"].asString() == workspace->name() ||
             (workspace->isSpecial() && x["name"].asString() == "special:" + workspace->name());
    });
    uint32_t count = 0;
    if (workspaceJson != workspacesJson.end()) {
      try {
        count = (*workspaceJson)["windows"].asUInt();
      } catch (const std::exception& e) {
        spdlog::error("Failed to update window count: {}", e.what());
      }
    }
    workspace->setWindows(count);
  }
}

bool FancyWorkspaces::updateWindowsToCreate() {
  bool anyWindowCreated = false;
  std::vector<FancyWindowCreationPayload> notCreated;
  for (auto& windowPayload : m_windowsToCreate) {
    bool created = false;
    for (auto& workspace : m_workspaces) {
      if (workspace->onWindowOpened(windowPayload)) {
        created = true;
        anyWindowCreated = true;
        break;
      }
    }
    if (!created) {
      static auto const WINDOW_CREATION_TIMEOUT = 2;
      if (windowPayload.incrementTimeSpentUncreated() < WINDOW_CREATION_TIMEOUT) {
        notCreated.push_back(windowPayload);
      } else {
        registerOrphanWindow(windowPayload);
      }
    }
  }
  m_windowsToCreate.clear();
  m_windowsToCreate = notCreated;
  return anyWindowCreated;
}

void FancyWorkspaces::updateWorkspaceStates() {
  const std::vector<int> visibleWorkspaces = getVisibleWorkspaces();
  auto updatedWorkspaces = m_ipc.getSocket1JsonReply("workspaces");

  auto currentWorkspace = m_ipc.getSocket1JsonReply("activeworkspace");
  std::string currentWorkspaceName =
      currentWorkspace.isMember("name") ? currentWorkspace["name"].asString() : "";

  for (auto& workspace : m_workspaces) {
    bool isActiveByName =
        !currentWorkspaceName.empty() && workspace->name() == currentWorkspaceName;

    workspace->setActive(
        workspace->id() == m_activeWorkspaceId || isActiveByName ||
        (workspace->isSpecial() && workspace->name() == m_activeSpecialWorkspaceName));
    
    if (workspace->isActive()) {
      spdlog::debug("Workspace {} is now active, urgent={}", workspace->name(), workspace->isUrgent());
    }
    
    if (workspace->isActive() && workspace->isUrgent()) {
      spdlog::debug("Clearing urgent for workspace {}", workspace->name());
      workspace->setUrgent(false);
      // Clear urgent windows for this workspace
      auto wsWindows = getWorkspaceWindows(workspace.get());
      for (const auto& window : wsWindows) {
        // Ensure address has 0x prefix to match what was stored
        std::string addr = window.windowAddress;
        if (!addr.starts_with("0x")) {
          addr = "0x" + addr;
        }
        spdlog::debug("Clearing urgent window: {}", addr);
        auto erased = m_urgentWindows.erase(addr);
        spdlog::debug("Erased {} (was {}present)", addr, erased ? "" : "NOT ");
      }
      spdlog::debug("Urgent windows remaining: {}", m_urgentWindows.size());
    }
    workspace->setVisible(std::ranges::find(visibleWorkspaces, workspace->id()) !=
                          visibleWorkspaces.end());
    std::string& workspaceIcon = m_iconsMap[""];
    if (m_withIcon) {
      workspaceIcon = workspace->selectIcon(m_iconsMap);
    }
    auto updatedWorkspace = std::ranges::find_if(updatedWorkspaces, [&workspace](const auto& w) {
      auto wNameRaw = w["name"].asString();
      auto wName = wNameRaw.starts_with("special:") ? wNameRaw.substr(8) : wNameRaw;
      return wName == workspace->name();
    });
    if (updatedWorkspace != updatedWorkspaces.end()) {
      workspace->setOutput((*updatedWorkspace)["monitor"].asString());
    }
    workspace->update(workspaceIcon);
  }
}

int FancyWorkspaces::windowRewritePriorityFunction(std::string const& window_rule) {
  // Rules that match against title are prioritized
  // Rules that don't specify if they're matching against either title or class are deprioritized
  bool const hasTitle = window_rule.find("title") != std::string::npos;
  bool const hasClass = window_rule.find("class") != std::string::npos;

  if (hasTitle && hasClass) {
    m_anyWindowRewriteRuleUsesTitle = true;
    return 3;
  }
  if (hasTitle) {
    m_anyWindowRewriteRuleUsesTitle = true;
    return 2;
  }
  if (hasClass) {
    return 1;
  }
  return 0;
}

template <typename... Args>
std::string FancyWorkspaces::makePayload(Args const&... args) {
  std::ostringstream result;
  bool first = true;
  ((result << (first ? "" : ",") << args, first = false), ...);
  return result.str();
}

std::pair<std::string, std::string> FancyWorkspaces::splitDoublePayload(std::string const& payload) {
  const std::string part1 = payload.substr(0, payload.find(','));
  const std::string part2 = payload.substr(part1.size() + 1);
  return {part1, part2};
}

std::tuple<std::string, std::string, std::string> FancyWorkspaces::splitTriplePayload(
    std::string const& payload) {
  const size_t firstComma = payload.find(',');
  const size_t secondComma = payload.find(',', firstComma + 1);

  const std::string part1 = payload.substr(0, firstComma);
  const std::string part2 = payload.substr(firstComma + 1, secondComma - (firstComma + 1));
  const std::string part3 = payload.substr(secondComma + 1);

  return {part1, part2, part3};
}

std::optional<int> FancyWorkspaces::parseWorkspaceId(std::string const& workspaceIdStr) {
  try {
    return workspaceIdStr == "special" ? -99 : std::stoi(workspaceIdStr);
  } catch (std::exception const& e) {
    spdlog::debug("Workspace \"{}\" is not bound to an id: {}", workspaceIdStr, e.what());
    return std::nullopt;
  }
}

std::optional<std::string> FancyWorkspaces::extractProjectPrefix(const std::string& workspaceName) {
  static std::regex pattern(R"(^\.(\d*[a-zA-Z]+)\d+)");
  std::smatch match;
  if (std::regex_search(workspaceName, match, pattern)) {
    return "." + match[1].str();
  }
  return std::nullopt;
}

std::string FancyWorkspaces::extractNumber(const std::string& workspaceName) {
  static std::regex pattern(R"((\d*[a-zA-Z]+)(\d+))");
  std::smatch match;
  if (std::regex_search(workspaceName, match, pattern)) {
    return match[2].str();
  }
  return "";
}

std::vector<std::string> FancyWorkspaces::getWorkspaceWindowClasses(FancyWorkspace* ws) {
  return ws->getWindowClasses();
}

std::vector<FancyWorkspaces::WindowInfo> FancyWorkspaces::getWorkspaceWindows(FancyWorkspace* ws) {
  auto wsWindows = ws->getWindows();
  std::vector<WindowInfo> result;
  for (const auto& w : wsWindows) {
    result.push_back({w.windowClass, w.windowTitle, w.windowAddress});
  }
  return result;
}

std::optional<std::string> FancyWorkspaces::getIconNameForClass(const std::string& windowClass) {
  // Reuse the icon lookup logic from workspace.cpp
  // For now, we'll include the helper functions directly

  auto data_dirs = Glib::get_system_data_dirs();
  data_dirs.insert(data_dirs.begin(), Glib::get_user_data_dir());

  // Helper to find desktop file
  auto getDesktopFile = [&](const std::string& appId) -> std::optional<std::string> {
    for (const auto& data_dir : data_dirs) {
      const auto data_app_dir = data_dir + "/applications/";
      if (!std::filesystem::exists(data_app_dir)) continue;

      for (const auto& entry : std::filesystem::recursive_directory_iterator(data_app_dir)) {
        if (entry.is_regular_file()) {
          std::string filename = entry.path().filename().string();
          std::string suffix = appId + ".desktop";

          if (filename.size() >= suffix.size()) {
            std::string lowerFilename = filename;
            std::string lowerSuffix = suffix;
            std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(),
                           ::tolower);
            std::transform(lowerSuffix.begin(), lowerSuffix.end(), lowerSuffix.begin(), ::tolower);

            if (lowerFilename.compare(lowerFilename.size() - lowerSuffix.size(), lowerSuffix.size(),
                                      lowerSuffix) == 0) {
              return entry.path().string();
            }
          }
        }
      }
    }
    return std::nullopt;
  };

  auto desktopFile = getDesktopFile(windowClass);
  if (desktopFile.has_value()) {
    try {
      Glib::KeyFile keyfile;
      keyfile.load_from_file(desktopFile.value());
      return keyfile.get_string("Desktop Entry", "Icon");
    } catch (...) {
      // Fall through to heuristics
    }
  }

  // Try heuristics
  if (DefaultGtkIconThemeWrapper::has_icon(windowClass)) {
    return windowClass;
  }

  auto desktopSuffix = windowClass + "-desktop";
  if (DefaultGtkIconThemeWrapper::has_icon(desktopSuffix)) {
    return desktopSuffix;
  }

  return std::nullopt;
}

bool FancyWorkspaces::isWorkspaceInActiveGroup(const std::string& workspaceName) {
  // Find the active workspace
  auto activeWorkspace = std::find_if(m_workspaces.begin(), m_workspaces.end(),
                                      [](const auto& ws) { return ws->isActive(); });

  if (activeWorkspace == m_workspaces.end()) {
    // No active workspace, show no icons
    return false;
  }

  // Extract project prefix from both workspaces
  auto activePrefix = extractProjectPrefix((*activeWorkspace)->name());
  auto thisPrefix = extractProjectPrefix(workspaceName);

  // Both must have a prefix and they must match
  if (!activePrefix.has_value() || !thisPrefix.has_value()) {
    // One or both don't have a project prefix
    // Check if they're the same workspace (for single workspaces without groups)
    return workspaceName == (*activeWorkspace)->name();
  }

  return activePrefix.value() == thisPrefix.value();
}

int FancyWorkspaces::countWorkspacesInProject(const std::string& prefix) {
  int count = 0;
  for (const auto& ws : m_workspaces) {
    auto wsPrefix = extractProjectPrefix(ws->name());
    if (wsPrefix && *wsPrefix == prefix) {
      count++;
    }
  }
  return count;
}

std::unique_ptr<Gtk::Button> FancyWorkspaces::createLabelButton(const std::string& text) {
  auto btn = std::make_unique<Gtk::Button>();
  btn->set_label(text);
  btn->set_relief(Gtk::RELIEF_NONE);
  btn->set_sensitive(false);  // Non-clickable
  btn->get_style_context()->add_class("workspace-label");
  btn->get_style_context()->add_class("grouped");  // For CSS spacing
  btn->get_style_context()->add_class(MODULE_CLASS);
  return btn;
}

std::string FancyWorkspaces::selectBestWindowForIcon(
    const std::vector<std::string>& addresses,
    const std::map<std::string, std::string>& addressToWorkspace, const std::string& groupPrefix,
    const std::string& monitor) {
  if (addresses.empty()) {
    spdlog::error("[ICON_CLICK] No addresses provided");
    return "";
  }

  // Priority 1: Check for urgent workspace
  for (const auto& addr : addresses) {
    auto wsIt = addressToWorkspace.find(addr);
    if (wsIt != addressToWorkspace.end()) {
      // Find workspace by name and check if urgent
      auto workspace = std::ranges::find_if(
          m_workspaces, [&wsIt](const std::unique_ptr<FancyWorkspace>& ws) {
            return ws->name() == wsIt->second;
          });
      if (workspace != m_workspaces.end() && workspace->get()->isUrgent()) {
        spdlog::info("[ICON_CLICK] Found window in urgent workspace '{}': {}", wsIt->second, addr);
        return addr;
      }
    }
  }

  // Build key for last active lookup
  std::string key = groupPrefix + "@" + monitor;

  // Priority 2: Try to find last active workspace
  auto it = m_lastActivePerGroup.find(key);
  if (it != m_lastActivePerGroup.end()) {
    std::string lastActiveWs = it->second;

    // Look for a window in that workspace
    for (const auto& addr : addresses) {
      auto wsIt = addressToWorkspace.find(addr);
      if (wsIt != addressToWorkspace.end() && wsIt->second == lastActiveWs) {
        spdlog::info("[ICON_CLICK] Found window in last active workspace '{}': {}", lastActiveWs,
                     addr);
        return addr;
      }
    }

    spdlog::debug("[ICON_CLICK] No window in last active workspace '{}', using first",
                  lastActiveWs);
  } else {
    spdlog::debug("[ICON_CLICK] No last active workspace for group '{}', using first", groupPrefix);
  }

  // Fallback: return first window
  return addresses[0];
}

void FancyWorkspaces::applyProjectCollapsing() {
  // Check if any feature is enabled
  if (!m_collapseInactiveProjects && !m_transformWorkspaceNames) {
    spdlog::debug("Workspace project features disabled");
    return;
  }

  // spdlog::debug("Workspace project collapsing/transform: processing {} workspaces",
  //               m_workspaces.size());

  // Group workspaces by project prefix
  struct ProjectGroup {
    std::string prefix;
    std::vector<FancyWorkspace*> workspaces;
    bool hasActive = false;
    bool hasWindows = false;  // Track if any workspace in group has windows
    bool hasUrgent = false;   // Track if any workspace in group is urgent
    int firstPosition = -1;
  };

  std::map<std::string, ProjectGroup> groups;

  // Get current bar's monitor for filtering
  std::string currentMonitor = getBarOutput();

  for (size_t i = 0; i < m_workspaces.size(); ++i) {
    auto& workspace = m_workspaces[i];
    auto prefix = extractProjectPrefix(workspace->name());

    spdlog::trace("Workspace '{}' -> prefix: {}", workspace->name(), prefix ? *prefix : "none");

    // Only include workspaces from current monitor in groups
    if (prefix && workspace->output() == currentMonitor) {
      auto& group = groups[*prefix];
      group.prefix = *prefix;
      group.workspaces.push_back(workspace.get());

      if (workspace->isActive()) {
        group.hasActive = true;
      }

      if (workspace->isUrgent()) {
        group.hasUrgent = true;
      }

      // Check if this workspace has windows by checking if it has "empty" CSS class
      auto styleContext = workspace->button().get_style_context();
      if (!styleContext->has_class("empty")) {
        group.hasWindows = true;
      }

      if (group.firstPosition == -1) {
        group.firstPosition = i;
      }
    }
  }

  // spdlog::debug("Workspace project features: found {} project groups", groups.size());

  // Sort workspaces within each group numerically by their number
  for (auto& [prefix, group] : groups) {
    std::sort(group.workspaces.begin(), group.workspaces.end(),
              [this](FancyWorkspace* a, FancyWorkspace* b) {
                std::string numA = extractNumber(a->name());
                std::string numB = extractNumber(b->name());
                try {
                  return std::stoi(numA) < std::stoi(numB);
                } catch (...) {
                  return a->name() < b->name();  // Fallback to name comparison
                }
              });
  }

  // Clear old collapsed groups
  for (auto* groupBox : m_collapsedGroups) {
    m_box.remove(*groupBox);
  }
  m_collapsedGroups.clear();

  // Clear old expanded group boxes
  for (auto* groupBox : m_expandedGroupBoxes) {
    m_box.remove(*groupBox);
  }
  m_expandedGroupBoxes.clear();

  for (auto& btn : m_labelButtons) {
    m_box.remove(*btn);
  }
  m_labelButtons.clear();

  // Apply collapsing/transform logic
  // Track position offset as groups add elements
  int positionOffset = 0;

  for (auto& [prefix, group] : groups) {
    std::string cleanPrefix = prefix.substr(1);  // Remove leading dot

    // Decide what to do based on enabled features
    bool shouldCollapse =
        m_collapseInactiveProjects && !group.hasActive && group.workspaces.size() > 1;
    bool shouldTransform = m_transformWorkspaceNames;

    // Choose display name based on transform flag
    std::string displayPrefix = shouldTransform ? cleanPrefix : prefix;

    // Track elements added by this group
    int elementsAdded = 0;

    if (shouldCollapse) {
      // Collapse: hide individual workspaces, show [prefix] with icons as sibling buttons
      // spdlog::debug("Workspace group '{}' -> collapsing to [{}]", prefix, displayPrefix);
      for (auto* ws : group.workspaces) {
        ws->button().hide();
      }

      // Create container box for the group (not a button!)
      auto* groupBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
      groupBox->get_style_context()->add_class("collapsed-project");  // Backward compat
      groupBox->get_style_context()->add_class("collapsed-project-group");

      // Add opening bracket label
      auto* openBracket = Gtk::manage(new Gtk::Label("["));
      groupBox->pack_start(*openBracket, false, false);

      // Create label button: prefix (without brackets)
      auto* labelBtn = Gtk::manage(new Gtk::Button());
      labelBtn->set_relief(Gtk::RELIEF_NONE);
      labelBtn->get_style_context()->add_class("collapsed-project-label");
      labelBtn->get_style_context()->add_class(MODULE_CLASS);
      labelBtn->set_label(displayPrefix);  // Just the prefix, no brackets

      // Add click handler for label - switches to workspace
      FancyWorkspace* firstWorkspace = group.workspaces[0];
      std::string groupPrefix = prefix;  // Capture by value
      labelBtn->signal_clicked().connect([this, firstWorkspace, groupPrefix]() {
        try {
          // Build compound key for this group+monitor
          std::string monitor = getBarOutput();
          std::string key = groupPrefix + "@" + monitor;

          // Look up last active workspace for this group
          std::string workspaceName;
          auto it = m_lastActivePerGroup.find(key);
          if (it != m_lastActivePerGroup.end()) {
            workspaceName = it->second;
            spdlog::debug("Workspace collapsed label '{}' clicked: switching to last active {}",
                          groupPrefix, workspaceName);
          } else {
            // No history, fall back to first workspace
            workspaceName = firstWorkspace->name();
            spdlog::debug(
                "Workspace collapsed label '{}' clicked: no history, switching to first {}",
                groupPrefix, workspaceName);
          }

          m_ipc.getSocket1Reply("dispatch workspace name:" + workspaceName);
        } catch (const std::exception& e) {
          spdlog::error("Workspace group label click failed: {}", e.what());
        }
      });

      // Add right-click handler for label - cleanup empty workspaces
      std::vector<FancyWorkspace*> groupWorkspaces = group.workspaces;  // Capture by value
      labelBtn->signal_button_press_event().connect([this, groupWorkspaces, groupPrefix](GdkEventButton* bt) {
        if (bt->type == GDK_BUTTON_PRESS && bt->button == 3) {
          spdlog::debug("Right-click on collapsed group '{}', removing empty workspaces", groupPrefix);
          for (auto* ws : groupWorkspaces) {
            if (ws->isEmpty()) {
              std::string cmd = "waybar-workspace-remove.sh " + ws->name();
              util::command::res result = util::command::exec(cmd, "workspace-remove");
              if (result.exit_code == 0) {
                spdlog::info("Removed workspace '{}'", ws->name());
              } else {
                spdlog::warn("Workspace removal failed: {}", result.out);
              }
            }
          }
          return true;
        }
        return false;
      });

      // Apply empty class if group has no windows
      if (!group.hasWindows) {
        labelBtn->get_style_context()->add_class("empty");
      }

      // Apply urgent class if any workspace in group is urgent
      if (group.hasUrgent) {
        labelBtn->get_style_context()->add_class("urgent");
      }

      groupBox->pack_start(*labelBtn, false, false);

      // Collect and deduplicate icons from all workspaces in this group
      if (m_showWindowIcons == ShowWindowIcons::ALL) {
        std::set<std::string> uniqueIconNames;
        std::vector<std::string> iconNamesOrdered;
        std::map<std::string, std::vector<std::pair<std::string, std::string>>>
            iconToWorkspaceAndTitles;
        std::map<std::string, std::vector<std::string>> iconToAddresses;
        std::map<std::string, std::string> addressToWorkspace;  // For smart selection

        for (auto* ws : group.workspaces) {
          auto windows = getWorkspaceWindows(ws);
          for (const auto& window : windows) {
            auto iconNameOpt = getIconNameForClass(window.windowClass);
            if (iconNameOpt.has_value()) {
              std::string iconName = iconNameOpt.value();
              if (uniqueIconNames.find(iconName) == uniqueIconNames.end()) {
                uniqueIconNames.insert(iconName);
                iconNamesOrdered.push_back(iconName);
              }
              // Collect workspace name and window title for tooltip
              iconToWorkspaceAndTitles[iconName].push_back({ws->name(), window.windowTitle});
              // Collect window address for click handler
              iconToAddresses[iconName].push_back(window.windowAddress);
              // Map address to workspace for smart selection
              addressToWorkspace[window.windowAddress] = ws->name();
            }
          }
        }

        // Create icon buttons
        for (const auto& iconName : iconNamesOrdered) {
          auto* iconBtn = Gtk::manage(new Gtk::Button());
          iconBtn->set_relief(Gtk::RELIEF_NONE);
          iconBtn->get_style_context()->add_class("collapsed-project-icon");
          iconBtn->get_style_context()->add_class(MODULE_CLASS);

          auto* icon = Gtk::manage(new Gtk::Image());
          icon->set_pixel_size(m_windowIconSize);

          if (iconName.front() == '/') {
            // File path
            try {
              auto pixbuf =
                  Gdk::Pixbuf::create_from_file(iconName, m_windowIconSize, m_windowIconSize);
              icon->set(pixbuf);
            } catch (const Glib::Error& e) {
              spdlog::warn("[ICON_CLICK] Failed to load icon from file {}: {}", iconName,
                           e.what().c_str());
              continue;
            }
          } else {
            // Icon name from theme
            icon->set_from_icon_name(iconName, Gtk::ICON_SIZE_INVALID);
          }

          iconBtn->add(*icon);

          // Build tooltip data - keep structured for interleaving
          const auto& workspaceAndTitles = iconToWorkspaceAndTitles[iconName];
          
          // Set up custom tooltip with thumbnails
          const auto& iconAddresses = iconToAddresses[iconName];
          iconBtn->set_has_tooltip(true);
          iconBtn->signal_query_tooltip().connect(
              [iconName, workspaceAndTitles, iconAddresses](int x, int y, bool keyboard_tooltip,
                                       const Glib::RefPtr<Gtk::Tooltip>& tooltip_widget) -> bool {
                // Create tooltip content box
                auto* vbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));
                
                // Add header
                auto* header = Gtk::manage(new Gtk::Label(iconName + ":"));
                header->set_xalign(0.0);
                vbox->pack_start(*header, false, false);
                
                // Interleave thumbnails and titles
                waybar::util::ThumbnailCache cache;
                size_t count = std::min(iconAddresses.size(), workspaceAndTitles.size());
                
                for (size_t i = 0; i < count; i++) {
                  const auto& addr = iconAddresses[i];
                  const auto& [wsName, title] = workspaceAndTitles[i];
                  
                  // Try to load thumbnail
                  auto thumbnail_path = cache.getThumbnailPath(addr);
                  if (thumbnail_path.has_value()) {
                    try {
                      auto pixbuf = Gdk::Pixbuf::create_from_file(thumbnail_path.value());
                      // Scale thumbnail if needed (max 256x256)
                      int width = pixbuf->get_width();
                      int height = pixbuf->get_height();
                      if (width > 256 || height > 256) {
                        double scale = std::min(256.0 / width, 256.0 / height);
                        width = static_cast<int>(width * scale);
                        height = static_cast<int>(height * scale);
                        pixbuf = pixbuf->scale_simple(width, height, Gdk::INTERP_BILINEAR);
                      }
                      
                      auto* thumb_img = Gtk::manage(new Gtk::Image(pixbuf));
                      vbox->pack_start(*thumb_img, false, false);
                    } catch (const Glib::Error& e) {
                      spdlog::debug("[ICON_TOOLTIP] Failed to load thumbnail for {}: {}", addr,
                                    e.what().c_str());
                    }
                  }
                  
                  // Add title with workspace
                  std::string titleText = "  " + wsName + ": " + title;
                  auto* titleLabel = Gtk::manage(new Gtk::Label(titleText));
                  titleLabel->set_xalign(0.0);
                  titleLabel->set_line_wrap(true);
                  titleLabel->set_max_width_chars(50);
                  vbox->pack_start(*titleLabel, false, false);
                }
                
                vbox->show_all();
                tooltip_widget->set_custom(*vbox);
                return true;
              });

          // Check if any of this icon's windows are urgent (by address)
          // (iconAddresses already captured in lambda above)
          bool hasUrgentWindow = std::ranges::any_of(iconAddresses, [this](const std::string& addr) {
            bool isUrgent = m_urgentWindows.contains("0x" + addr);
            if (isUrgent) {
              spdlog::debug("[ICON_URGENT] Icon address 0x{} is urgent", addr);
            }
            return isUrgent;
          });
          if (hasUrgentWindow) {
            spdlog::debug("[ICON_URGENT] Icon '{}' has urgent window, applying class", iconName);
            iconBtn->get_style_context()->add_class("urgent");
          }

          // Add click handler for icon - smart window focus
          std::vector<std::string> allAddresses = iconToAddresses[iconName];
          std::map<std::string, std::string> addrToWs = addressToWorkspace;
          iconBtn->signal_clicked().connect(
              [this, allAddresses, addrToWs, groupPrefix, iconName]() {
                std::string targetAddress =
                    selectBestWindowForIcon(allAddresses, addrToWs, groupPrefix, getBarOutput());
                if (!targetAddress.empty()) {
                  spdlog::info("[ICON_CLICK] Icon '{}' clicked, focusing window: {}", iconName,
                               targetAddress);
                  m_ipc.getSocket1Reply("dispatch focuswindow address:0x" + targetAddress);
                }
              });

          groupBox->pack_start(*iconBtn, false, false);
        }
      }

      // Add closing bracket label
      auto* closeBracket = Gtk::manage(new Gtk::Label("]"));
      groupBox->pack_start(*closeBracket, false, false);

      // Calculate adjusted position accounting for elements added by earlier groups
      int targetPosition = group.firstPosition + positionOffset;

      m_box.add(*groupBox);
      m_box.reorder_child(*groupBox, targetPosition);
      groupBox->show_all();

      m_collapsedGroups.push_back(groupBox);

      // Collapsed group adds 1 element (the groupBox)
      elementsAdded = 1;

    } else if (shouldTransform) {
      // Transform names without collapsing
      if (group.workspaces.size() == 1) {
        // Single workspace: show as just prefix name (no number, no brackets)
        // spdlog::debug("Workspace group '{}' -> single workspace, display as '{}'", prefix,
        //               cleanPrefix);
        auto* ws = group.workspaces[0];

        // Set display name to just the prefix - use setLabelText to preserve icons
        ws->setLabelText(cleanPrefix);
        ws->button().show();

        // Single workspace adds 0 extra elements (workspace already exists)
        elementsAdded = 0;

      } else {
        // Multiple workspaces: show as [prefix num num num]
        // spdlog::debug("Workspace group '{}' -> transformed as [{}...]", prefix, cleanPrefix);

        // Calculate adjusted position
        int pos = group.firstPosition + positionOffset;

        // Create start container: [bracket + label] matching collapsed group structure
        auto* startBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
        startBox->get_style_context()->add_class("expanded-group-start");
        if (group.hasActive) {
          startBox->get_style_context()->add_class("active-group");
        }
        
        // Use Label for bracket (like collapsed groups do)
        auto* openBracket = Gtk::manage(new Gtk::Label("["));
        openBracket->get_style_context()->add_class("group-bracket");
        if (group.hasActive) {
          openBracket->get_style_context()->add_class("active-group");
        }
        startBox->pack_start(*openBracket, false, false);

        // Add project name (make it clickable to create new workspace)
        auto* projectLabel = Gtk::manage(new Gtk::Button());
        projectLabel->set_label(cleanPrefix);
        projectLabel->set_relief(Gtk::RELIEF_NONE);
        projectLabel->get_style_context()->add_class("workspace-label");
        projectLabel->get_style_context()->add_class("grouped");
        projectLabel->get_style_context()->add_class(
            "empty");  // Use empty style for project labels
        projectLabel->get_style_context()->add_class(MODULE_CLASS);
        if (group.hasActive) {
          projectLabel->get_style_context()->add_class("active-group");
        }

        // Add click handler to create new workspace in this project
        std::string projectName = cleanPrefix;
        projectLabel->signal_clicked().connect([this, projectName]() {
          try {
            spdlog::debug("Workspace project label '{}' clicked: creating new workspace",
                          projectName);

            std::string cmd = "waybar-workspace-create.sh " + projectName;
            util::command::res result = util::command::exec(cmd, "workspace-create");

            if (result.exit_code == 0) {
              spdlog::info("Created new workspace for project '{}'", projectName);
            } else {
              spdlog::warn("Workspace creation failed: {}", result.out);
            }
          } catch (const std::exception& e) {
            spdlog::error("Workspace project label click failed: {}", e.what());
          }
        });

        // Add right-click handler to cleanup empty workspaces
        std::vector<FancyWorkspace*> groupWorkspaces = group.workspaces;
        projectLabel->signal_button_press_event().connect([this, groupWorkspaces, projectName](GdkEventButton* bt) {
          if (bt->type == GDK_BUTTON_PRESS && bt->button == 3) {
            spdlog::debug("Right-click on expanded group '{}', removing empty workspaces", projectName);
            for (auto* ws : groupWorkspaces) {
              if (ws->isEmpty()) {
                std::string cmd = "waybar-workspace-remove.sh " + ws->name();
                util::command::res result = util::command::exec(cmd, "workspace-remove");
                if (result.exit_code == 0) {
                  spdlog::info("Removed workspace '{}'", ws->name());
                } else {
                  spdlog::warn("Workspace removal failed: {}", result.out);
                }
              }
            }
            return true;
          }
          return false;
        });

        startBox->pack_start(*projectLabel, false, false);
        
        m_box.add(*startBox);
        m_box.reorder_child(*startBox, pos++);
        startBox->show_all();
        m_expandedGroupBoxes.push_back(startBox);

        // Add workspaces (just numbers)
        for (auto* ws : group.workspaces) {
          std::string number = extractNumber(ws->name());
          if (number.empty()) {
            number = "?";
          }

          // Use setLabelText to update label without destroying icon boxes
          ws->setLabelText(number);
          ws->button().get_style_context()->add_class("grouped");  // For CSS spacing
          if (group.hasActive) {
            ws->button().get_style_context()->add_class("active-group");
          }
          ws->button().show();
          m_box.reorder_child(ws->button(), pos++);
        }

        // Create end container: [closing bracket] matching collapsed group structure
        auto* endBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
        endBox->get_style_context()->add_class("expanded-group-end");
        if (group.hasActive) {
          endBox->get_style_context()->add_class("active-group");
        }
        
        // Use Label for bracket (like collapsed groups do)
        auto* closeBracket = Gtk::manage(new Gtk::Label("]"));
        closeBracket->get_style_context()->add_class("group-bracket");
        if (group.hasActive) {
          closeBracket->get_style_context()->add_class("active-group");
        }
        endBox->pack_start(*closeBracket, false, false);
        
        m_box.add(*endBox);
        m_box.reorder_child(*endBox, pos);
        endBox->show_all();
        m_expandedGroupBoxes.push_back(endBox);
        endBox->show_all();

        // Transformed group adds: startBox + endBox = 2 elements
        // (workspaces already exist, just reordered)
        elementsAdded = 2;
      }
    } else {
      // No transform, no collapse - just show normally
      for (auto* ws : group.workspaces) {
        ws->button().show();
      }
      elementsAdded = 0;
    }

    // Update position offset for next group
    positionOffset += elementsAdded;
  }
}

void FancyWorkspaces::executeHook(const std::string& command, const std::string& workspaceName,
                                  const std::string& workspaceMonitor, int workspaceId) {
  if (command.empty()) {
    return;
  }

  // Replace variables in command
  std::string cmd = command;
  size_t pos = 0;
  while ((pos = cmd.find("{name}", pos)) != std::string::npos) {
    cmd.replace(pos, 6, workspaceName);
    pos += workspaceName.length();
  }
  
  pos = 0;
  while ((pos = cmd.find("{monitor}", pos)) != std::string::npos) {
    cmd.replace(pos, 9, workspaceMonitor);
    pos += workspaceMonitor.length();
  }
  
  pos = 0;
  std::string idStr = std::to_string(workspaceId);
  while ((pos = cmd.find("{id}", pos)) != std::string::npos) {
    cmd.replace(pos, 4, idStr);
    pos += idStr.length();
  }

  spdlog::debug("Executing hook: {}", cmd);

  // Fork and execute command asynchronously
  pid_t pid = fork();
  if (pid == 0) {
    // Child process
    execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
    _exit(1);
  } else if (pid < 0) {
    spdlog::error("Failed to fork process for hook execution");
  }
  // Parent continues without waiting
}

void FancyWorkspaces::captureThumbnailsForWorkspace(const std::string& workspaceName) {
  if (!m_thumbnailCache.isAvailable()) {
    return;
  }
  
  spdlog::debug("[THUMBNAIL] Starting batch capture for workspace '{}'", workspaceName);
  
  // Find the workspace
  auto workspace = std::find_if(m_workspaces.begin(), m_workspaces.end(),
                                [&workspaceName](const auto& ws) { return ws->name() == workspaceName; });
  
  if (workspace == m_workspaces.end()) {
    return;
  }
  
  // Get all windows in this workspace
  auto windows = getWorkspaceWindows(workspace->get());
  
  if (windows.empty()) {
    spdlog::debug("[THUMBNAIL] No windows in workspace '{}'", workspaceName);
    return;
  }
  
  // Get current clients data for geometry
  Json::Value clientsData = m_ipc.getSocket1JsonReply("clients");
  
  // Fork a process to handle batch capture with delay
  pid_t pid = fork();
  if (pid == 0) {
    // Child process: wait for animation, then capture all windows one by one
    usleep(300000); // 300ms for animation
    
    spdlog::debug("[THUMBNAIL] Capturing {} windows in workspace '{}'", windows.size(), workspaceName);
    
    // Create a new cache instance in child process
    waybar::util::ThumbnailCache cache;
    
    for (const auto& window : windows) {
      // Find this window's geometry
      std::string jsonWindowAddress = "0x" + window.windowAddress;
      
      auto client = std::ranges::find_if(clientsData, [&jsonWindowAddress](auto& client) {
        return client["address"].asString() == jsonWindowAddress;
      });
      
      if (client != clientsData.end() && !client->empty()) {
        int x = (*client)["at"][0].asInt();
        int y = (*client)["at"][1].asInt();
        int w = (*client)["size"][0].asInt();
        int h = (*client)["size"][1].asInt();
        
        // Capture synchronously (no extra delay needed, we already waited)
        cache.captureWindowSync(window.windowAddress, x, y, w, h,
                               window.windowClass, window.windowTitle, workspaceName);
      }
    }
    
    _exit(0);
  } else if (pid > 0) {
    // Parent: track the PID so we can kill it if needed
    m_captureProcessPid = pid;
  }
}

}  // namespace waybar::modules::hyprland
