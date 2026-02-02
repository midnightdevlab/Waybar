#include <gtkmm.h>
#include <iostream>
#include <thread>
#include <chrono>

class TestWindow : public Gtk::Window {
public:
  TestWindow() {
    set_title("Gravity Test");
    set_decorated(false);
    // Remove type hint to avoid needing parent
    
    // Initial position and size
    move(500, 500);
    resize(100, 100);
    
    m_label.set_text("Initial: 100x100 at 500,500");
    add(m_label);
    
    show_all();
    
    // Get window address after it's shown
    Glib::signal_idle().connect_once([this]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      
      // Get our window address
      std::string cmd = "hyprctl clients -j | jq -r '.[] | select(.title==\"Gravity Test\") | .address'";
      FILE* pipe = popen(cmd.c_str(), "r");
      if (pipe) {
        char buffer[128];
        if (fgets(buffer, sizeof(buffer), pipe)) {
          m_address = std::string(buffer);
          m_address.erase(m_address.find_last_not_of(" \n\r\t") + 1);
          std::cout << "Window address: " << m_address << std::endl;
        }
        pclose(pipe);
      }
      
      // Step 1: Read initial window info
      std::cout << "\n=== STEP 1: Initial state (100x100 at 500,500) ===" << std::endl;
      readWindowInfo();
      
      // Wait a bit
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      
      // Step 2: Resize to 500x500 using Hyprland
      std::cout << "\n=== STEP 2: Resizing to 500x500 via hyprctl ===" << std::endl;
      std::string resize_cmd = "hyprctl dispatch resizewindowpixel exact 500 500,address:" + m_address;
      system(resize_cmd.c_str());
      
      // Wait for resize to complete
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      
      // Step 3: Read window info after resize
      std::cout << "\n=== STEP 3: After resize to 500x500 ===" << std::endl;
      readWindowInfo();
      
      std::cout << "\n=== Analysis ===" << std::endl;
      std::cout << "If position changed, window is anchored at CENTER during resize" << std::endl;
      std::cout << "If position stayed 500,500, window is anchored at TOP-LEFT" << std::endl;
      
      // Exit after showing results
      std::this_thread::sleep_for(std::chrono::milliseconds(2000));
      hide();
    });
  }

private:
  void readWindowInfo() {
    if (m_address.empty()) return;
    
    std::string cmd = "hyprctl clients | grep -A 30 '" + m_address + "'";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
      char buffer[256];
      while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        if (line.find("at:") != std::string::npos ||
            line.find("size:") != std::string::npos) {
          std::cout << line;
        }
      }
      pclose(pipe);
    }
  }
  
  Gtk::Label m_label;
  std::string m_address;
};

int main(int argc, char* argv[]) {
  auto app = Gtk::Application::create(argc, argv, "org.test.gravity");
  TestWindow window;
  return app->run(window);
}
