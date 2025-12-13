// запуск программы FileFinder.exe <путь_к_каталогу> <шаблон_поиска> [количество_потоков]

#include <iostream>
#include <filesystem>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <regex>
#include <string>
#include <locale.h> 

namespace fs = std::filesystem;
std::regex wildcardToRegex(const std::string& pattern) { // преобразование шаблона в регулярное выражение
    std::string regex_s;
    regex_s.reserve(pattern.size() * 2); // резервация памяти заранее
    regex_s.push_back('^'); // ^ - начало строки
    for (char c : pattern) {
        switch (c) {
        case '*': regex_s += ".*"; break;
        case '?': regex_s += "."; break;
        case '.': regex_s += "\\."; break;
        case '\\': regex_s += "\\\\"; break;
        default:
            if (std::string(".^$+()[]{}|").find(c) != std::string::npos) {
                regex_s.push_back('\\');
            }
            regex_s.push_back(c);
        }
    }
    regex_s.push_back('$'); // $ - конец строки
    return std::regex(regex_s, std::regex::icase);
}

class DirQueue { // потокобезопасная очередь директорий
public:
    void push(fs::path p) { // добавление в очередь
        std::lock_guard<std::mutex> lk(mtx); // блокировка мьютекса
        q.push(std::move(p));
        cv.notify_one(); // пробуждает один поток в ожидании
    }

    bool pop_or_wait(fs::path& out, std::atomic<int>& pending_dirs, std::atomic<bool>& stop_flag) { // вытаскивает директорию из очереди или ждёт появления новых
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [&] { return !q.empty() || stop_flag.load() || pending_dirs.load() == 0; }); // проверка для потока можно ли выходить из сна
        if (!q.empty()) { 
            out = std::move(q.front()); // получаем первый элемент из очереди и затем удаляем его
            q.pop();
            return true;
        }
        return false;
    }

    void notify_all() {
        cv.notify_all();
    }

private:
    std::queue<fs::path> q;
    std::mutex mtx;
    std::condition_variable cv; // поток
};

int main(int argc, char* argv[]) {
    setlocale(LC_ALL, "ru");

    if (argc < 3) {
        std::cout << "Использование:\n"
            << "  " << argv[0] << " <start_path> <pattern> [num_threads]\n\n"
            << "pattern: поддерживает '*' и '?' (например: *.txt, data_??.csv)\n"
            << "Если не указать num_threads — будет использовано количество аппаратных потоков.\n";
        return 1;
    }

    fs::path start_path = argv[1];
    std::string pattern = argv[2];
    int num_threads = (argc >= 4) ? std::max(1, std::stoi(argv[3])) : std::max(1u, std::thread::hardware_concurrency()); // если количество потоков не указано - количество аппаратных потоков системы

    if (!fs::exists(start_path)) {
        std::cerr << "Ошибка: стартовый путь не существует: " << start_path << "\n";
        return 1;
    }

    // если pattern не содержит '*' или '?', используем обычный подстрочный поиск, без использования wildcardToRegex
    bool use_wildcard = (pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos);
    std::regex pattern_regex = use_wildcard ? wildcardToRegex(pattern) : std::regex(".*", std::regex::icase);

    DirQueue dirq;
    std::atomic<int> pending_dirs{ 0 }; // количество директорий, которые либо в очереди, либо обрабатываются
    std::atomic<bool> stop_flag{ false };
    std::mutex print_mtx;

    dirq.push(start_path);
    pending_dirs.fetch_add(1);

    auto worker = [&](int id) {
        while (true) {
            fs::path dir;
            bool got = dirq.pop_or_wait(dir, pending_dirs, stop_flag);
            if (!got) {
                if (pending_dirs.load() == 0) break; // если нет заданий и pending_dirs==0 - завершение работы
                continue;
            }

            try {
                for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied)) {
                    try {
                        if (entry.is_directory()) {
                            // добавляем новую директорию в очередь
                            pending_dirs.fetch_add(1);
                            dirq.push(entry.path());
                        }
                        else if (entry.is_regular_file() || entry.is_symlink()) {
                            std::string filename = entry.path().filename().string();
                            bool matched = false;
                            if (use_wildcard) {
                                matched = std::regex_match(filename, pattern_regex);
                            }
                            else {
                                // нижний регистр
                                std::string lowername = filename;
                                std::string lowerpat = pattern;
                                std::transform(lowername.begin(), lowername.end(), lowername.begin(), ::tolower);
                                std::transform(lowerpat.begin(), lowerpat.end(), lowerpat.begin(), ::tolower);
                                matched = (lowername.find(lowerpat) != std::string::npos);
                            }

                            if (matched) {
                                std::lock_guard<std::mutex> lk(print_mtx);
                                std::cout << entry.path().string() << "\n";
                            }
                        }
                    }
                    catch (const fs::filesystem_error& e) {
                        // проблемы с конкретной записью - логируем и продолжаем
                        std::lock_guard<std::mutex> lk(print_mtx);
                        std::cerr << "[warn] " << e.what() << " (path: " << entry.path().string() << ")\n";
                    }
                }
            }
            catch (const fs::filesystem_error& e) {
                std::lock_guard<std::mutex> lk(print_mtx);
                std::cerr << "[warn] Не удалось открыть директорию " << dir.string() << ": " << e.what() << "\n";
            }

            // готово с этой директорией
            int remaining = pending_dirs.fetch_sub(1) - 1; // fetch_sub возвращяет старое значение
            // если теперь нет директорий — нужно разбудить всех, чтобы воркеры могли выйти
            if (remaining == 0) {
                dirq.notify_all();
            }
        }
        };

    // запуск пула потоков
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }

    // ждём завершения
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    return 0;
}
