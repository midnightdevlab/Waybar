#include "util/thumbnail_cache.hpp"

#include <spdlog/spdlog.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace waybar::util {

namespace fs = std::filesystem;

ThumbnailCache::ThumbnailCache() {
  m_cacheDir = getCachePath();
  
  // Create cache directory if it doesn't exist
  try {
    fs::create_directories(m_cacheDir);
  } catch (const fs::filesystem_error& e) {
    spdlog::warn("Failed to create thumbnail cache directory: {}", e.what());
  }
  
  m_captureAvailable = checkCaptureTools();
  if (!m_captureAvailable) {
    spdlog::warn("Thumbnail capture tools not available (need grim and magick/convert)");
  }
}

std::string ThumbnailCache::getCachePath() const {
  const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
  std::string base_cache;
  
  if (xdg_cache && xdg_cache[0] != '\0') {
    base_cache = xdg_cache;
  } else {
    const char* home = std::getenv("HOME");
    if (home) {
      base_cache = std::string(home) + "/.cache";
    } else {
      base_cache = "/tmp";
    }
  }
  
  return base_cache + "/waybar/thumbnails";
}

std::string ThumbnailCache::getThumbnailFilePath(const std::string& windowAddress) const {
  return m_cacheDir + "/" + windowAddress + ".png";
}

std::string ThumbnailCache::getMetadataFilePath(const std::string& windowAddress) const {
  return m_cacheDir + "/" + windowAddress + ".meta";
}

bool ThumbnailCache::checkCaptureTools() {
  // Check for grim
  int grim_result = system("command -v grim >/dev/null 2>&1");
  if (grim_result != 0) {
    return false;
  }
  
  // Check for magick or convert
  int magick_result = system("command -v magick >/dev/null 2>&1");
  int convert_result = system("command -v convert >/dev/null 2>&1");
  
  return (magick_result == 0 || convert_result == 0);
}

std::string ThumbnailCache::getResizeCommand() const {
  // Prefer magick (ImageMagick v7), fallback to convert (v6)
  int magick_result = system("command -v magick >/dev/null 2>&1");
  return (magick_result == 0) ? "magick" : "convert";
}

void ThumbnailCache::captureWindow(const std::string& windowAddress, int x, int y, int width,
                                   int height, const std::string& windowClass,
                                   const std::string& windowTitle,
                                   const std::string& workspaceName) {
  if (!m_captureAvailable) {
    return;
  }
  
  spdlog::debug("[THUMBNAIL] Capturing window {}: {}x{} at {},{}", windowAddress, width, height, x, y);
  
  std::string full_path = m_cacheDir + "/full_" + windowAddress + ".png";
  std::string temp_thumb = m_cacheDir + "/temp_" + windowAddress + "_" + std::to_string(getpid()) + ".png";
  std::string thumb_path = getThumbnailFilePath(windowAddress);
  std::string meta_path = getMetadataFilePath(windowAddress);
  
  // Build capture command with -s 1 to use logical pixels (not scaled)
  std::ostringstream capture_cmd;
  capture_cmd << "grim -s 1 -g \"" << x << "," << y << " " << width << "x" << height << "\" "
              << full_path << " 2>/dev/null";
  
  // Execute capture in background (fork to avoid blocking)
  pid_t pid = fork();
  if (pid == 0) {
    // Child process - wait for animation to complete before capturing
    usleep(300000); // 300ms delay for workspace switch animation
    
    int result = system(capture_cmd.str().c_str());
    if (result != 0) {
      spdlog::debug("[THUMBNAIL] Capture failed for window {}", windowAddress);
      _exit(1);
    }
    
    // Resize to thumbnail (using temp file)
    std::string resize_cmd_name = getResizeCommand();
    std::ostringstream resize_cmd;
    resize_cmd << resize_cmd_name << " " << full_path << " -resize 256x256 " << temp_thumb
               << " 2>/dev/null";
    result = system(resize_cmd.str().c_str());
    if (result != 0) {
      spdlog::debug("[THUMBNAIL] Resize failed for window {}", windowAddress);
      unlink(full_path.c_str());
      _exit(1);
    }
    
    // Clean up full size image
    unlink(full_path.c_str());
    
    // Verify workspace is still the same before committing
    // Query current workspace of the window
    std::string cmd = "hyprctl clients -j | jq -r '.[] | select(.address==\"0x" + windowAddress + "\") | .workspace.name'";
    FILE* pipe = popen(cmd.c_str(), "r");
    std::string current_workspace;
    if (pipe) {
      char buffer[256];
      if (fgets(buffer, sizeof(buffer), pipe)) {
        current_workspace = buffer;
        // Remove trailing newline
        if (!current_workspace.empty() && current_workspace.back() == '\n') {
          current_workspace.pop_back();
        }
      }
      pclose(pipe);
    }
    
    // Only commit if workspace hasn't changed
    if (current_workspace == workspaceName) {
      spdlog::debug("[THUMBNAIL] Workspace still {}, committing thumbnail", workspaceName);
      // Atomic move from temp to final location
      rename(temp_thumb.c_str(), thumb_path.c_str());
      
      // Write metadata
      std::ofstream meta_file(meta_path);
      if (meta_file.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::system_clock::to_time_t(now);
        
        meta_file << "address=" << windowAddress << "\n";
        meta_file << "class=" << windowClass << "\n";
        meta_file << "title=" << windowTitle << "\n";
        meta_file << "workspace=" << workspaceName << "\n";
        meta_file << "timestamp=" << timestamp << "\n";
        meta_file << "width=" << width << "\n";
        meta_file << "height=" << height << "\n";
        meta_file.close();
      }
    } else {
      spdlog::debug("[THUMBNAIL] Workspace changed from {} to {}, discarding thumbnail", 
                    workspaceName, current_workspace);
      unlink(temp_thumb.c_str());
    }
    
    _exit(0);
  } else if (pid < 0) {
    spdlog::error("[THUMBNAIL] Failed to fork for thumbnail capture");
  }
  // Parent continues without waiting
}

void ThumbnailCache::captureWindowSync(const std::string& windowAddress, int x, int y, int width,
                                       int height, const std::string& windowClass,
                                       const std::string& windowTitle,
                                       const std::string& workspaceName) {
  if (!m_captureAvailable) {
    return;
  }
  
  spdlog::debug("[THUMBNAIL] Capturing window synchronously {}: {}x{} at {},{}", 
                windowAddress, width, height, x, y);
  
  std::string full_path = m_cacheDir + "/full_" + windowAddress + ".png";
  std::string thumb_path = getThumbnailFilePath(windowAddress);
  std::string meta_path = getMetadataFilePath(windowAddress);
  
  // Build and execute capture command synchronously with -s 1 for logical pixels
  std::ostringstream capture_cmd;
  capture_cmd << "grim -s 1 -g \"" << x << "," << y << " " << width << "x" << height << "\" "
              << full_path << " 2>/dev/null";
  
  int result = system(capture_cmd.str().c_str());
  if (result != 0) {
    spdlog::debug("[THUMBNAIL] Sync capture failed for window {}", windowAddress);
    return;
  }
  
  // Resize to thumbnail
  std::string resize_cmd_name = getResizeCommand();
  std::ostringstream resize_cmd;
  resize_cmd << resize_cmd_name << " " << full_path << " -resize 256x256 " << thumb_path
             << " 2>/dev/null";
  result = system(resize_cmd.str().c_str());
  if (result != 0) {
    spdlog::debug("[THUMBNAIL] Sync resize failed for window {}", windowAddress);
    unlink(full_path.c_str());
    return;
  }
  
  // Clean up full size image
  unlink(full_path.c_str());
  
  // Write metadata
  std::ofstream meta_file(meta_path);
  if (meta_file.is_open()) {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::system_clock::to_time_t(now);
    
    meta_file << "address=" << windowAddress << "\n";
    meta_file << "class=" << windowClass << "\n";
    meta_file << "title=" << windowTitle << "\n";
    meta_file << "workspace=" << workspaceName << "\n";
    meta_file << "timestamp=" << timestamp << "\n";
    meta_file << "width=" << width << "\n";
    meta_file << "height=" << height << "\n";
    meta_file.close();
  }
}

std::optional<std::string> ThumbnailCache::getThumbnailPath(const std::string& windowAddress,
                                                            int maxAgeSeconds) {
  std::string thumb_path = getThumbnailFilePath(windowAddress);
  
  // Check if file exists
  if (!fs::exists(thumb_path)) {
    return std::nullopt;
  }
  
  // Check age
  try {
    auto ftime = fs::last_write_time(thumb_path);
    auto now = fs::file_time_type::clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - ftime).count();
    
    if (age > maxAgeSeconds) {
      spdlog::debug("[THUMBNAIL] Thumbnail too old for {}: {}s", windowAddress, age);
      return std::nullopt;
    }
    
    return thumb_path;
  } catch (const fs::filesystem_error& e) {
    spdlog::debug("[THUMBNAIL] Error checking thumbnail age: {}", e.what());
    return std::nullopt;
  }
}

std::optional<ThumbnailMetadata> ThumbnailCache::getMetadata(const std::string& windowAddress) {
  std::string meta_path = getMetadataFilePath(windowAddress);
  
  if (!fs::exists(meta_path)) {
    return std::nullopt;
  }
  
  std::ifstream meta_file(meta_path);
  if (!meta_file.is_open()) {
    return std::nullopt;
  }
  
  ThumbnailMetadata meta;
  meta.windowAddress = windowAddress;
  
  std::string line;
  while (std::getline(meta_file, line)) {
    size_t eq_pos = line.find('=');
    if (eq_pos == std::string::npos) continue;
    
    std::string key = line.substr(0, eq_pos);
    std::string value = line.substr(eq_pos + 1);
    
    if (key == "class") {
      meta.windowClass = value;
    } else if (key == "title") {
      meta.windowTitle = value;
    } else if (key == "workspace") {
      meta.workspaceName = value;
    } else if (key == "timestamp") {
      time_t timestamp = std::stol(value);
      meta.timestamp = std::chrono::system_clock::from_time_t(timestamp);
    } else if (key == "width") {
      meta.width = std::stoi(value);
    } else if (key == "height") {
      meta.height = std::stoi(value);
    }
  }
  
  return meta;
}

void ThumbnailCache::cleanup(int maxAgeSeconds, size_t maxSizeMB) {
  try {
    size_t total_size = 0;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    
    // Collect all thumbnail files with their timestamps
    for (const auto& entry : fs::directory_iterator(m_cacheDir)) {
      if (entry.path().extension() == ".png") {
        files.emplace_back(fs::last_write_time(entry), entry.path());
        total_size += fs::file_size(entry);
      }
    }
    
    // Sort by age (oldest first)
    std::sort(files.begin(), files.end());
    
    auto now = fs::file_time_type::clock::now();
    size_t max_bytes = maxSizeMB * 1024 * 1024;
    
    // Remove old files or if over size limit
    for (const auto& [ftime, path] : files) {
      auto age = std::chrono::duration_cast<std::chrono::seconds>(now - ftime).count();
      
      bool should_remove = (age > maxAgeSeconds) || (total_size > max_bytes);
      
      if (should_remove) {
        size_t file_size = fs::file_size(path);
        fs::remove(path);
        
        // Remove metadata file too
        std::string meta_path = path.string();
        meta_path.replace(meta_path.find(".png"), 4, ".meta");
        fs::remove(meta_path);
        
        total_size -= file_size;
        spdlog::debug("[THUMBNAIL] Cleaned up old thumbnail: {}", path.filename().string());
      }
    }
  } catch (const fs::filesystem_error& e) {
    spdlog::warn("[THUMBNAIL] Cleanup error: {}", e.what());
  }
}

}  // namespace waybar::util
