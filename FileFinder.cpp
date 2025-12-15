// запуск программы FileFinder.exe <путь_к_каталогу> <шаблон_поиска> [количество_потоков]

#include <iostream>
#include <filesystem>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <locale.h> 
#include <fstream>
#include <algorithm>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <ctime>

namespace fs = std::filesystem;

// сопоставление имени файла с шаблоном
bool matchWildcard(const std::string& name, const std::string& pattern) {
    size_t n = 0, p = 0;
    size_t star = std::string::npos, match = 0;

    while (n < name.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '?' ||
                std::tolower(static_cast<unsigned char>(pattern[p])) ==
                std::tolower(static_cast<unsigned char>(name[n])))) {
            ++n;
            ++p;
        }
        else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = n;
        }
        else if (star != std::string::npos) {
            p = star + 1;
            n = ++match;
        }
        else {
            return false;
        }
    }

    while (p < pattern.size() && pattern[p] == '*')
        ++p;

    return p == pattern.size();
}

std::mutex log_mtx; // мьютекс для лога
std::ofstream log_file; // лог файл
std::atomic<bool> any_file_found{ false };  // найден ли хотя бы один файл

std::string get_current_time() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm local_tm;  // структура для локального времени
    localtime_s(&local_tm, &time_t_now);

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%d-%m-%Y %H:%M:%S");
    oss << '.' << std::setprecision(3) << std::fixed << ms.count();
    return oss.str();
}

// точка отсчёта времени — запуск main
const auto program_start = std::chrono::high_resolution_clock::now();

// логирование
void log(const std::string& msg) {
    std::lock_guard<std::mutex> lk(log_mtx);
    log_file << msg << std::endl;
}

// потокобезопасная очередь директорий
class DirQueue { 
public:
    void push(fs::path p) { // добавление директории в очередь
        std::lock_guard<std::mutex> lk(mtx);
        q.push(std::move(p));
        cv.notify_one();
    }

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

    void notify_all() { // пробуждение всех потоков
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

    log_file.open("filefinder.log", std::ios::out | std::ios::app);
    if (!log_file.is_open()) {
        std::cerr << "Не удалось открыть файл лога filefinder.log\n";
        return 1;
    }

    log("\n=== FileFinder started at " + get_current_time() + " ===");
    log("Start path: " + start_path.string());
    log("Pattern: " + pattern);
    log("Threads: " + std::to_string(num_threads));

    if (!fs::exists(start_path)) {
        log("[error] Start path does not exist");
        std::cerr << "Ошибка: стартовый путь не существует: " << start_path << "\n";
        log_file.close();
        return 1;
    }

    DirQueue dirq;
    std::atomic<int> pending_dirs{ 0 };
    std::atomic<bool> stop_flag{ false };

    dirq.push(start_path);
    pending_dirs.fetch_add(1); // стартовую директорию добавляем в очередь

    auto worker = [&](int id) {
        {
            std::ostringstream oss;
            oss << "Thread started. ID = " << std::this_thread::get_id();
            log(oss.str());
        }

        while (true) {
            fs::path dir;
            bool got = dirq.pop_or_wait(dir, pending_dirs, stop_flag);
            if (!got) {
                if (pending_dirs.load() == 0) break;
                continue;
            }

            try {
                for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied)) {
                    if (entry.is_directory()) {
                        pending_dirs.fetch_add(1);
                        dirq.push(entry.path());
                    }
                    else if (entry.is_regular_file() || entry.is_symlink()) {
                        std::string filename = entry.path().filename().string();
                        if (matchWildcard(filename, pattern)) {
                            any_file_found.store(true);  // пометка, что что-то нашли

                            std::string full_path = entry.path().string();
                            std::string output = "Time: " + get_current_time() + " ms | Path: " + full_path;

                            std::cout << full_path << "\n";
                            log(output);
                        }
                    }
                }
            }
            catch (const fs::filesystem_error& e) {
                log(std::string("[warn] Access denied or error in directory: ") + dir.string() + " - " + e.what());
            }
            catch (const std::exception& e) {
                log(std::string("[error] Unexpected exception in directory: ") + dir.string() + " - " + e.what());
            }
            catch (...) {
                log(std::string("[error] Unknown exception in directory: ") + dir.string());
            }

            int remaining = pending_dirs.fetch_sub(1) - 1; // уменьшаем количество директорий на 1, если закончили с текущей
            if (remaining == 0) {
                dirq.notify_all();
            }
        }
        {
            std::ostringstream oss;
            oss << "Thread finished. ID = " << std::this_thread::get_id();
            log(oss.str());
        }
        };

    // запуск потоков
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }

    // ожидание завершения
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    if (!any_file_found.load()) { // если не нашли ни одного файла по шаблону
        std::cout << "Искомый файл не найден\n";
        log("[info] No files matched the pattern");
    }

    log("=== FileFinder finished ===");
    log_file.close();

    return 0;
}