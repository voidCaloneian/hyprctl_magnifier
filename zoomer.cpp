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

// Путь к файлу конфигурации
const std::string CONFIG_FILE = "devices.cfg";

// Глобальные переменные для выбранных устройств
// Сохраняем имена устройств, чтобы потом найти их по имени
std::string keyboard_name;
std::string mouse_name;

// Сюда сохраняется найденный путь устройства после сканирования
std::string keyboard_path;
std::string mouse_path;

// Настройка зума
std::atomic<bool> win_pressed(false);
std::mutex zoom_mutex;
double zoom_target  = 1.0;       // Целевое значение зума
double zoom_current = 1.0;       // Текущее (плавное) значение зума
const double zoom_step      = 0.3;  // Шаг изменения целевого зума
const double zoom_increment = 0.03;  // Инкремент изменения текущего зума за такт
const double zoom_min       = 1.0;   // Минимальное значение зума
const double zoom_max       = 10.0;  // Максимальное значение зума

// Функция для вызова hyprctl с текущим зумом
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
        std::cout << "Применён зум: " << current_zoom << std::endl;
    } else {
        std::cerr << "Ошибка вызова команды hyprctl" << std::endl;
    }
}

// Поток для плавного обновления зума
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

// Функция создания виртуального устройства для мыши через uinput
int create_uinput_device() {
    int uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) {
        perror("Ошибка открытия /dev/uinput");
        return -1;
    }

    // Включаем необходимые типы событий 
    if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0) {
        perror("ioctl UI_SET_EVBIT EV_KEY");
        return -1;
    }
    if (ioctl(uinput_fd, UI_SET_EVBIT, EV_REL) < 0) {
        perror("ioctl UI_SET_EVBIT EV_REL");
        return -1;
    }

    // Разрешаем кнопки мыши (левая, правая и средняя)
    if (ioctl(uinput_fd, UI_SET_KEYBIT, BTN_LEFT) < 0 ||
        ioctl(uinput_fd, UI_SET_KEYBIT, BTN_RIGHT) < 0 ||
        ioctl(uinput_fd, UI_SET_KEYBIT, BTN_MIDDLE) < 0) {
        perror("ioctl UI_SET_KEYBIT BTN_*");
        return -1;
    }

    // Разрешаем относительные оси: перемещение и колесико
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
        perror("Ошибка записи uinput_user_dev");
        return -1;
    }

    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        perror("ioctl UI_DEV_CREATE");
        return -1;
    }

    return uinput_fd;
}

// Функция для отправки события через uinput
void send_uinput_event(int uinput_fd, struct input_event &ev) {
    if (write(uinput_fd, &ev, sizeof(ev)) < 0) {
        perror("Ошибка записи события uinput");
    }
}

// Функция отправки SYN-события через uinput
void sync_uinput(int uinput_fd) {
    struct input_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    if (write(uinput_fd, &ev, sizeof(ev)) < 0) {
        perror("Ошибка синхронизации uinput");
    }
}

// ---------------- Функции работы с конфигурацией ----------------

// Сохраняет имена выбранных устройств в файл CONFIG_FILE
void save_config(const std::string &kbd_name, const std::string &ms_name) {
    std::ofstream ofs(CONFIG_FILE);
    if (ofs) {
        ofs << "keyboard=" << kbd_name << "\n";
        ofs << "mouse=" << ms_name << "\n";
        std::cout << "Конфигурация сохранена в " << CONFIG_FILE << std::endl;
    } else {
        std::cerr << "Не удалось сохранить конфигурацию в " << CONFIG_FILE << std::endl;
    }
}

// Чтение конфигурации. Возвращает true, если удалось считать оба параметра.
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

// Структура для хранения информации об устройстве
struct DeviceInfo {
    std::string path;
    std::string name;
};

// Функция для сканирования устройств в /dev/input, которые начинаются с "event"
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
        di.name = dev_name ? dev_name : "Неизвестно";
        libevdev_free(dev);
        close(fd);
        devices.push_back(di);
    }
    closedir(dir);
    return devices;
}

// Функция интерактивного выбора устройства из списка
std::string choose_device(const std::string &type, std::string &chosen_name) {
    auto devices = scan_input_devices();
    if (devices.empty()) {
        std::cerr << "Устройства не найдены!" << std::endl;
        exit(1);
    }

    std::cout << "\nСписок устройств для выбора " << type << ":" << std::endl;
    for (size_t i = 0; i < devices.size(); i++) {
        std::cout << "  [" << i << "] " << devices[i].path << " - " << devices[i].name << std::endl;
    }
    std::cout << "Введите номер устройства для " << type << ": ";
    size_t selection = 0;
    std::cin >> selection;
    if (selection >= devices.size()) {
        std::cerr << "Неверный выбор!" << std::endl;
        exit(1);
    }
    chosen_name = devices[selection].name;
    std::cout << "Выбрано: " << devices[selection].path << " (" << devices[selection].name << ")" << std::endl;
    return devices[selection].path;
}

// Поиск устройства по имени, если оно указано в конфигурации
// Возвращает путь к устройству, если найдено, иначе пустую строку
std::string find_device_by_name(const std::string &target_name) {
    auto devices = scan_input_devices();
    for (const auto &dev : devices) {
        if (dev.name == target_name) {
            return dev.path;
        }
    }
    return "";
}

// ---------------- Конец функций конфигурации ----------------

// Поток мониторинга событий клавиатуры (без виртуального устройства)
void keyboard_monitor() {
    int fd = open(keyboard_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Не удалось открыть устройство клавиатуры: " << keyboard_path << std::endl;
        return;
    }
    struct libevdev* dev = nullptr;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        std::cerr << "Ошибка инициализации libevdev для клавиатуры: " << strerror(-rc) << std::endl;
        close(fd);
        return;
    }
    std::cout << "Клавиатура: " << libevdev_get_name(dev) << std::endl;

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    while (true) {
        int poll_status = poll(&pfd, 1, 50);
        if (poll_status > 0) {
            struct input_event ev;
            while ((rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) == 0) {
                if (ev.type == EV_KEY && (ev.code == KEY_LEFTMETA || ev.code == KEY_RIGHTMETA)) {
                    if (ev.value == 1) { // Нажатие клавиши Windows
                        win_pressed.store(true);
                        std::cout << "Windows нажата" << std::endl;
                    } else if (ev.value == 0) { // Отпускание клавиши Windows
                        win_pressed.store(false);
                        std::cout << "Windows отпущена" << std::endl;
                    }
                }
            }
        }
    }
    libevdev_free(dev);
    close(fd);
}

// Поток мониторинга событий мыши с использованием uinput для переотправки событий
void mouse_monitor() {
    int fd = open(mouse_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Не удалось открыть устройство мыши: " << mouse_path << std::endl;
        return;
    }
    struct libevdev* dev = nullptr;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        std::cerr << "Ошибка инициализации libevdev для мыши: " << strerror(-rc) << std::endl;
        close(fd);
        return;
    }
    std::cout << "Мышь: " << libevdev_get_name(dev) << std::endl;

    int uinput_fd = create_uinput_device();
    if (uinput_fd < 0) {
        std::cerr << "Не удалось создать виртуальное устройство для мыши" << std::endl;
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
                            std::cout << "Увеличение целевого зума до " << zoom_target << std::endl;
                        } else if (ev.value < 0) {
                            zoom_target = std::max(zoom_target - zoom_step, zoom_min);
                            std::cout << "Уменьшение целевого зума до " << zoom_target << std::endl;
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
    std::cout << "Запуск глобального перехвата событий. Возможно, потребуется запуск от sudo.\n" << std::endl;

    bool config_loaded = read_config(keyboard_name, mouse_name);

    // Если конфигурация загружена, пытаемся найти устройства по имени
    if (config_loaded) {
        std::cout << "Загружена конфигурация:\n"
                  << "  Клавиатура: " << keyboard_name << "\n"
                  << "  Мышь: " << mouse_name << "\n" << std::endl;
        keyboard_path = find_device_by_name(keyboard_name);
        mouse_path = find_device_by_name(mouse_name);
        if (keyboard_path.empty()) {
            std::cout << "Устройство клавиатуры с именем \"" << keyboard_name << "\" не найдено." << std::endl;
        }
        if (mouse_path.empty()) {
            std::cout << "Устройство мыши с именем \"" << mouse_name << "\" не найдено." << std::endl;
        }
    }

    // Если конфигурация не загружена или устройства по имени не найдены – интерактивный выбор
    if (keyboard_path.empty()) {
        keyboard_path = choose_device("клавиатуры", keyboard_name);
    }
    if (mouse_path.empty()) {
        mouse_path = choose_device("мыши", mouse_name);
    }
    // Сохраняем актуальные имена устройств
    save_config(keyboard_name, mouse_name);

    std::thread zoom_thread(smooth_zoom_updater);
    std::thread keyboard_thread(keyboard_monitor);
    std::thread mouse_thread(mouse_monitor);
    
    keyboard_thread.join();
    mouse_thread.join();
    zoom_thread.join();
    
    return 0;
}
