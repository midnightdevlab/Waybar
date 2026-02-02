// Test program for PopupIPCClient
#include "util/popup_ipc_client.hpp"
#include <spdlog/spdlog.h>
#include <iostream>
#include <thread>
#include <chrono>

using namespace waybar::util;

int main() {
  spdlog::set_level(spdlog::level::debug);
  
  std::cout << "Testing PopupIPCClient with images..." << std::endl;
  
  // Some test images from system
  std::vector<std::string> test_images = {
    "/usr/share/pixmaps/radeon-profile.png",
    "/usr/share/pixmaps/xarchiver/xarchiver-green.png",
    "/usr/share/pixmaps/xarchiver/xarchiver-red.png",
    "/usr/share/pixmaps/xfdesktop/xfdesktop-fallback-icon.png"
  };
  
  PopupIPCClient client;
  
  // Test 1: Connect
  std::cout << "\n1. Testing connection..." << std::endl;
  if (!client.connect()) {
    std::cerr << "Failed to connect to daemon!" << std::endl;
    return 1;
  }
  std::cout << "✓ Connected successfully" << std::endl;
  
  // Test 2: Show popup with images on DP-3
  std::cout << "\n2. Testing show popup with images (DP-3)..." << std::endl;
  std::vector<std::string> titles = {"Window 1", "Window 2", "Window 3"};
  std::vector<std::string> images = {test_images[0], test_images[1], test_images[2]};
  
  if (!client.showPopup(960, 100, "DP-3", titles, images)) {
    std::cerr << "Failed to show popup!" << std::endl;
    return 1;
  }
  std::cout << "✓ Popup shown with images" << std::endl;
  std::cout << "  (Check DP-3 at position 960,100 - should show 3 thumbnails)" << std::endl;
  
  // Wait a bit
  std::this_thread::sleep_for(std::chrono::seconds(4));
  
  // Test 3: Show popup with different images on DP-2
  std::cout << "\n3. Testing show popup with tall content (DP-2)..." << std::endl;
  titles = {"App 1", "App 2", "App 3", "App 4"};
  images = {test_images[0], test_images[1], test_images[2], test_images[3]};
  
  if (!client.showPopup(2880, 100, "DP-2", titles, images)) {
    std::cerr << "Failed to show popup!" << std::endl;
    return 1;
  }
  std::cout << "✓ Popup shown with 4 images" << std::endl;
  std::cout << "  (Check DP-2 at position 2880,100)" << std::endl;
  
  // Wait a bit
  std::this_thread::sleep_for(std::chrono::seconds(4));
  
  // Test 4: Show without images (backwards compatibility)
  std::cout << "\n4. Testing show without images..." << std::endl;
  titles = {"Text only 1", "Text only 2"};
  
  if (!client.showPopup(960, 100, "DP-3", titles)) {
    std::cerr << "Failed to show popup!" << std::endl;
    return 1;
  }
  std::cout << "✓ Popup shown without images (text only)" << std::endl;
  
  // Wait a bit
  std::this_thread::sleep_for(std::chrono::seconds(3));
  
  // Test 5: Hide popup
  std::cout << "\n5. Testing hide popup..." << std::endl;
  if (!client.hidePopup()) {
    std::cerr << "Failed to hide popup!" << std::endl;
    return 1;
  }
  std::cout << "✓ Popup hidden" << std::endl;
  
  // Wait a bit
  std::this_thread::sleep_for(std::chrono::seconds(1));
  
  // Test 6: Show single image
  std::cout << "\n6. Testing single large image..." << std::endl;
  titles = {"Single window"};
  images = {test_images[0]};
  
  if (!client.showPopup(960, 100, "DP-3", titles, images)) {
    std::cerr << "Failed to show popup!" << std::endl;
    return 1;
  }
  std::cout << "✓ Popup shown with single image" << std::endl;
  
  // Wait a bit
  std::this_thread::sleep_for(std::chrono::seconds(3));
  
  // Test 7: Clean up
  std::cout << "\n7. Cleaning up..." << std::endl;
  client.hidePopup();
  
  std::cout << "\n✓✓✓ All tests passed! ✓✓✓" << std::endl;
  return 0;
}
