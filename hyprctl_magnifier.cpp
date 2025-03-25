#include <libevdev/libevdev.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>
#include <dirent.h>

// Path to configuration file
const std::string CONFIG_FILE = "devices.cfg";

// Global variables for selected devices
// Save device names to find them later by name
std::string keyboard_name;
std::string mouse_name;

// After scanning, the found device path will be stored here
std::string keyboard_path;
std::string mouse_path;

// Zoom settings
std::atomic<bool> win_pressed(false);
std::mutex zoom_mutex;
double zoom_target  = 1.0;       // Target zoom value
double zoom_current = 1.0;       // Current (smooth) zoom value
const double zoom_step      = 0.3;  // Step for changing target zoom
const double zoom_increment = 0.03;  // Increment to change current zoom per tick
const double zoom_min       = 1.0;   // Minimum zoom value
const double zoom_max       = 10.0;  // Maximum zoom value

// Function to call hyprctl with the current zoom
void update_zoom() {
    double current_zoom;
    {
        std::lock_guard<std::mutex> lock(zoom_mutex);
        current_zoom = zoom_current;
    }
    char command[256];
    std::snprintf(command, sizeof(command), "hyprctl keyword misc:cursor_zoom_factor %.2f", current_zoom);
    int ret = std::system(command);
    if (ret == 0) {
        std::cout << "Applied zoom: " << current_zoom << std::endl;
    } else {
        std::cerr << "Error calling hyprctl command" << std::endl;
    }
}

// Thread for smooth zoom updating
void smooth_zoom_updater() {
    while (true) {
        bool updated = false;
        {
            std::lock_guard<std::mutex> lock(zoom_mutex);
            double diff = zoom_target - zoom_current;
            if (std::fabs(diff) > 0.001) {
                double delta = (std::fabs(diff) < zoom_increment) ? diff : (diff > 0 ? zoom_increment : -zoom_increment);
                zoom_current += delta;
                updated = true;
            }
        }
        if (updated) {
            update_zoom();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Function to create a virtual mouse device via uinput
int create_uinput_device() {
    int uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) {
        perror("Error opening /dev/uinput");
        return -1;
    }

    // Enable necessary event types
    if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0) {
        perror("ioctl UI_SET_EVBIT EV_KEY");
        return -1;
    }
    if (ioctl(uinput_fd, UI_SET_EVBIT, EV_REL) < 0) {
        perror("ioctl UI_SET_EVBIT EV_REL");
        return -1;
    }

    // Enable mouse buttons (left, right, and middle, mouse4, mouse5)
    if (ioctl(uinput_fd, UI_SET_KEYBIT, BTN_LEFT) < 0 ||
        ioctl(uinput_fd, UI_SET_KEYBIT, BTN_RIGHT) < 0 ||
        ioctl(uinput_fd, UI_SET_KEYBIT, BTN_SIDE) < 0 ||
        ioctl(uinput_fd, UI_SET_KEYBIT, BTN_EXTRA) < 0 || 
        ioctl(uinput_fd, UI_SET_KEYBIT, BTN_MIDDLE) < 0) {
        perror("ioctl UI_SET_KEYBIT BTN_*");
        return -1;
    }

    // Enable relative axes: movement and wheel
    if (ioctl(uinput_fd, UI_SET_RELBIT, REL_X) < 0 ||
        ioctl(uinput_fd, UI_SET_RELBIT, REL_Y) < 0 ||
        ioctl(uinput_fd, UI_SET_RELBIT, REL_WHEEL) < 0) {
        perror("ioctl UI_SET_RELBIT");
        return -1;
    }

    struct uinput_user_dev uidev;
    std::memset(&uidev, 0, sizeof(uidev));
    std::snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "virtual-mouse");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1;
    uidev.id.product = 0x1;
    uidev.id.version = 1;

    if (write(uinput_fd, &uidev, sizeof(uidev)) < 0) {
        perror("Error writing uinput_user_dev");
        return -1;
    }

    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        perror("ioctl UI_DEV_CREATE");
        return -1;
    }

    return uinput_fd;
}

// Function to send an event through uinput
void send_uinput_event(int uinput_fd, struct input_event &ev) {
    if (write(uinput_fd, &ev, sizeof(ev)) < 0) {
        perror("Error writing uinput event");
    }
}

// Function to send a SYN event via uinput
void sync_uinput(int uinput_fd) {
    struct input_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    if (write(uinput_fd, &ev, sizeof(ev)) < 0) {
        perror("Error synchronizing uinput");
    }
}

// ---------------- Configuration functions ----------------

// Saves the selected devices' names to the CONFIG_FILE
void save_config(const std::string &kbd_name, const std::string &ms_name) {
    std::ofstream ofs(CONFIG_FILE);
    if (ofs) {
        ofs << "keyboard=" << kbd_name << "\n";
        ofs << "mouse=" << ms_name << "\n";
        std::cout << "Configuration saved to " << CONFIG_FILE << std::endl;
    } else {
        std::cerr << "Failed to save configuration to " << CONFIG_FILE << std::endl;
    }
}

// Reads the configuration. Returns true if both parameters were read.
bool read_config(std::string &kbd_name, std::string &ms_name) {
    std::ifstream ifs(CONFIG_FILE);
    if (!ifs)
        return false;
    std::string line;
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string key, value;
        if (std::getline(iss, key, '=') && std::getline(iss, value)) {
            if (key == "keyboard")
                kbd_name = value;
            else if (key == "mouse")
                ms_name = value;
        }
    }
    return !kbd_name.empty() && !ms_name.empty();
}

// Structure to store device information
struct DeviceInfo {
    std::string path;
    std::string name;
};

// Function to scan for devices in /dev/input that start with "event"
std::vector<DeviceInfo> scan_input_devices() {
    std::vector<DeviceInfo> devices;
    const std::string input_dir = "/dev/input";
    DIR *dir = opendir(input_dir.c_str());
    if (!dir) {
        perror("opendir");
        return devices;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (std::strncmp(entry->d_name, "event", 5) != 0)
            continue;
        DeviceInfo di;
        di.path = input_dir + "/" + entry->d_name;

        int fd = open(di.path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        struct libevdev *dev = nullptr;
        if (libevdev_new_from_fd(fd, &dev) < 0) {
            close(fd);
            continue;
        }
        const char* dev_name = libevdev_get_name(dev);
        di.name = dev_name ? dev_name : "Unknown";
        libevdev_free(dev);
        close(fd);
        devices.push_back(di);
    }
    closedir(dir);
    return devices;
}

// Function for interactive device selection from the list
std::string choose_device(const std::string &type, std::string &chosen_name) {
    auto devices = scan_input_devices();
    if (devices.empty()) {
        std::cerr << "No devices found!" << std::endl;
        exit(1);
    }

    std::cout << "\nList of devices to choose for " << type << ":" << std::endl;
    for (size_t i = 0; i < devices.size(); i++) {
        std::cout << "  [" << i << "] " << devices[i].path << " - " << devices[i].name << std::endl;
    }
    std::cout << "Enter device number for " << type << ": ";
    size_t selection = 0;
    std::cin >> selection;
    if (selection >= devices.size()) {
        std::cerr << "Invalid selection!" << std::endl;
        exit(1);
    }
    chosen_name = devices[selection].name;
    std::cout << "Selected: " << devices[selection].path << " (" << devices[selection].name << ")" << std::endl;
    return devices[selection].path;
}

// Searches for a device by name (if specified in configuration)
// Returns the device path if found, otherwise an empty string
std::string find_device_by_name(const std::string &target_name) {
    auto devices = scan_input_devices();
    for (const auto &dev : devices) {
        if (dev.name == target_name) {
            return dev.path;
        }
    }
    return "";
}

// ---------------- End of configuration functions ----------------

// Keyboard events monitoring thread (without virtual device)
void keyboard_monitor() {
    int fd = open(keyboard_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Failed to open keyboard device: " << keyboard_path << std::endl;
        return;
    }
    struct libevdev* dev = nullptr;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        std::cerr << "Error initializing libevdev for keyboard: " << strerror(-rc) << std::endl;
        close(fd);
        return;
    }
    std::cout << "Keyboard: " << libevdev_get_name(dev) << std::endl;

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    while (true) {
        int poll_status = poll(&pfd, 1, 50);
        if (poll_status > 0) {
            struct input_event ev;
            while ((rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) == 0) {
                if (ev.type == EV_KEY && (ev.code == KEY_LEFTMETA || ev.code == KEY_RIGHTMETA)) {
                    if (ev.value == 1) { // Windows key press
                        win_pressed.store(true);
                        std::cout << "Windows pressed" << std::endl;
                    } else if (ev.value == 0) { // Windows key release
                        win_pressed.store(false);
                        std::cout << "Windows released" << std::endl;
                    }
                }
            }
        }
    }
    libevdev_free(dev);
    close(fd);
}

// Mouse events monitoring thread with uinput to re-send events
void mouse_monitor() {
    int fd = open(mouse_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Failed to open mouse device: " << mouse_path << std::endl;
        return;
    }
    struct libevdev* dev = nullptr;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        std::cerr << "Error initializing libevdev for mouse: " << strerror(-rc) << std::endl;
        close(fd);
        return;
    }
    std::cout << "Mouse: " << libevdev_get_name(dev) << std::endl;

    int uinput_fd = create_uinput_device();
    if (uinput_fd < 0) {
        std::cerr << "Failed to create virtual mouse device" << std::endl;
        libevdev_free(dev);
        close(fd);
        return;
    }

    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        perror("ioctl EVIOCGRAB");
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    while (true) {
        int poll_status = poll(&pfd, 1, 50);
        if (poll_status > 0) {
            struct input_event ev;
            while ((rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) == 0) {
                if (ev.type == EV_REL && ev.code == REL_WHEEL && win_pressed.load()) {
                    {
                        std::lock_guard<std::mutex> lock(zoom_mutex);
                        if (ev.value > 0) {
                            zoom_target = std::min(zoom_target + zoom_step, zoom_max);
                            std::cout << "Target zoom increased to " << zoom_target << std::endl;
                        } else if (ev.value < 0) {
                            zoom_target = std::max(zoom_target - zoom_step, zoom_min);
                            std::cout << "Target zoom decreased to " << zoom_target << std::endl;
                        }
                    }
                    continue;
                }
                send_uinput_event(uinput_fd, ev);
                if (ev.type != EV_SYN) {
                    struct input_event syn_ev;
                    std::memset(&syn_ev, 0, sizeof(syn_ev));
                    syn_ev.type = EV_SYN;
                    syn_ev.code = SYN_REPORT;
                    syn_ev.value = 0;
                    send_uinput_event(uinput_fd, syn_ev);
                }
            }
        }
    }
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    libevdev_free(dev);
    close(fd);
}

int main() {
    std::cout << "Starting global event interceptor. You may need to run with sudo.\n" << std::endl;

    bool config_loaded = read_config(keyboard_name, mouse_name);

    // If configuration is loaded, try to find devices by name
    if (config_loaded) {
        std::cout << "Configuration loaded:\n"
                  << "  Keyboard: " << keyboard_name << "\n"
                  << "  Mouse: " << mouse_name << "\n" << std::endl;
        keyboard_path = find_device_by_name(keyboard_name);
        mouse_path = find_device_by_name(mouse_name);
        if (keyboard_path.empty()) {
            std::cout << "Keyboard device with name \"" << keyboard_name << "\" not found." << std::endl;
        }
        if (mouse_path.empty()) {
            std::cout << "Mouse device with name \"" << mouse_name << "\" not found." << std::endl;
        }
    }

    // If configuration is not loaded or devices by name are not found – interactive selection
    if (keyboard_path.empty()) {
        keyboard_path = choose_device("keyboard", keyboard_name);
    }
    if (mouse_path.empty()) {
        mouse_path = choose_device("mouse", mouse_name);
    }
    // Save the current device names
    save_config(keyboard_name, mouse_name);

    std::thread zoom_thread(smooth_zoom_updater);
    std::thread keyboard_thread(keyboard_monitor);
    std::thread mouse_thread(mouse_monitor);
    
    keyboard_thread.join();
    mouse_thread.join();
    zoom_thread.join();
    
    return 0;
}
