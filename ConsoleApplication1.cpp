// file_search_mt.cpp
// Компиляция: MSVC: cl /std:c++17 file_search_mt.cpp
//            g++: g++ -std=c++17 file_search_mt.cpp -o file_search_mt
// Операционная система: Windows 10 (код использует std::filesystem)

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
// Преобразует шаблон с '*' и '?' в регулярное выражение
std::regex wildcardToRegex(const std::string& pattern) {
    std::string regex_s;
    regex_s.reserve(pattern.size() * 2);
    regex_s.push_back('^');
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
    regex_s.push_back('$');
    return std::regex(regex_s, std::regex::icase);
}

// Потокобезопасная очередь директорий — используем mutex + condvar
class DirQueue {
public:
    void push(fs::path p) {
        std::lock_guard<std::mutex> lk(mtx);
        q.push(std::move(p));
        cv.notify_one();
    }

    // Попытка получить элемент; блокирует если пусто, но вернёт false, если завершение (no more work)
    bool pop_or_wait(fs::path& out, std::atomic<int>& pending_dirs, std::atomic<bool>& stop_flag) {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [&] { return !q.empty() || stop_flag.load() || pending_dirs.load() == 0; });
        if (!q.empty()) {
            out = std::move(q.front());
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
    std::condition_variable cv;
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
    int num_threads = (argc >= 4) ? std::max(1, std::stoi(argv[3])) : std::max(1u, std::thread::hardware_concurrency());

    if (!fs::exists(start_path)) {
        std::cerr << "Ошибка: стартовый путь не существует: " << start_path << "\n";
        return 1;
    }

    // Поддержка: если pattern не содержит '*' или '?', используем подстрочный поиск (опция)
    bool use_wildcard = (pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos);
    std::regex pattern_regex = use_wildcard ? wildcardToRegex(pattern) : std::regex(".*", std::regex::icase);

    DirQueue dirq;
    std::atomic<int> pending_dirs{ 0 }; // количество директорий, которые либо в очереди, либо обрабатываются
    std::atomic<bool> stop_flag{ false };
    std::mutex print_mtx;

    // Положим стартовую директорию
    dirq.push(start_path);
    pending_dirs.fetch_add(1);

    // Воркер-функция
    auto worker = [&](int id) {
        while (true) {
            fs::path dir;
            bool got = dirq.pop_or_wait(dir, pending_dirs, stop_flag);
            if (!got) {
                // Проверка: если нет заданий и pending_dirs==0 => завершение
                if (pending_dirs.load() == 0) break;
                // Иначе — могло быть пробуждение, пробуем ещё раз
                continue;
            }

            // Обрабатываем директорию
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
                                // простое подстрочное, case-insensitive
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
                        // Проблемы с конкретной записью — логируем и продолжаем
                        std::lock_guard<std::mutex> lk(print_mtx);
                        std::cerr << "[warn] " << e.what() << " (path: " << entry.path().string() << ")\n";
                    }
                }
            }
            catch (const fs::filesystem_error& e) {
                std::lock_guard<std::mutex> lk(print_mtx);
                std::cerr << "[warn] Не удалось открыть директорию " << dir.string() << ": " << e.what() << "\n";
            }

            // Готово с этой директорией
            int remaining = pending_dirs.fetch_sub(1) - 1; // fetch_sub возвращяет старое значение
            // Если теперь нет директорий — нужно разбудить всех, чтобы воркеры могли выйти
            if (remaining == 0) {
                dirq.notify_all();
            }
        }
        };

    // Запускаем пул потоков
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }

    // Ждём завершения
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    return 0;
}
