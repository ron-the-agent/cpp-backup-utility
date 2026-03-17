/*
 * MULTI-THREADED BACKUP UTILITY - Version 2 (Fixed)
 * ==================================================
 * Uses a thread pool to copy multiple files concurrently.
 * Best for: SSDs, network drives, or when copying many small files.
 *
 * FIXES from Version 1:
 *  1. ThreadPool now stores std::function<void()> instead of CopyTask,
 *     so worker threads can actually execute submitted work.
 *  2. workerLoop() now calls task() to run the function.
 *  3. Lock is released before executing the task so other threads
 *     can pick up work concurrently (was the main bottleneck).
 *  4. main() now enqueues lambdas into the pool instead of calling
 *     copyFile() directly on the main thread.
 *  5. waitForCompletion() is now actually called in main().
 */

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <functional>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────
// Thread-safe statistics
// ─────────────────────────────────────────────
struct BackupStats {
    std::atomic<size_t> filesCopied{0};
    std::atomic<size_t> filesSkipped{0};
    std::atomic<size_t> errors{0};
    std::atomic<size_t> totalBytes{0};
};

// ─────────────────────────────────────────────
// Thread Pool
// FIX 1: Queue holds std::function<void()> so any callable can be submitted.
// ─────────────────────────────────────────────
class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads) : stop(false), activeTasks(0) {
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back([this] { workerLoop(); });
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }

    // Submit any callable (e.g. a lambda) as a task.
    void enqueue(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            tasks.push(std::move(task));
        }
        condition.notify_one();
    }

    // Block until the queue is empty and all active tasks have finished.
    void waitForCompletion() {
        std::unique_lock<std::mutex> lock(queueMutex);
        done.wait(lock, [this] { return tasks.empty() && activeTasks == 0; });
    }

private:
    std::vector<std::thread>          workers;
    std::queue<std::function<void()>> tasks;   // FIX 1: was queue<CopyTask>
    std::mutex                        queueMutex;
    std::condition_variable           condition;
    std::condition_variable           done;     // separate CV for waitForCompletion
    std::atomic<size_t>               activeTasks;
    bool                              stop;

    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                condition.wait(lock, [this] { return stop || !tasks.empty(); });
                if (stop && tasks.empty()) return;

                task = std::move(tasks.front());
                tasks.pop();
                ++activeTasks;
                // FIX 3: unlock before executing so other threads can
                //        dequeue their own tasks concurrently.
            }

            task(); // FIX 2: actually run the work

            {
                std::unique_lock<std::mutex> lock(queueMutex);
                --activeTasks;
            }
            // Wake waitForCompletion() in case we just finished the last task.
            done.notify_one();
        }
    }
};

// ─────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────
std::string formatBytes(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unitIndex < 4) {
        size /= 1024.0;
        ++unitIndex;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unitIndex];
    return oss.str();
}

std::mutex printMutex;
void safePrint(const std::string& msg) {
    std::lock_guard<std::mutex> lock(printMutex);
    std::cout << msg << "\n";
}

// ─────────────────────────────────────────────
// Core copy logic (unchanged, already thread-safe)
// ─────────────────────────────────────────────
bool copyFile(const fs::path& source, const fs::path& dest, BackupStats& stats) {
    try {
        fs::create_directories(dest.parent_path());

        if (fs::exists(dest)) {
            auto sourceTime = fs::last_write_time(source);
            auto destTime   = fs::last_write_time(dest);
            auto sourceSize = fs::file_size(source);
            auto destSize   = fs::file_size(dest);

            if (sourceTime <= destTime && sourceSize == destSize) {
                safePrint("  [SKIP]  " + source.filename().string() + " (up to date)");
                ++stats.filesSkipped;
                return true;
            }
        }

        std::ifstream src(source.string(), std::ios::binary);
        std::ofstream dst(dest.string(),   std::ios::binary);

        const size_t       bufferSize = 1024 * 1024; // 1 MB
        std::vector<char>  buffer(bufferSize);

        while (src.good()) {
            src.read(buffer.data(), bufferSize);
            dst.write(buffer.data(), src.gcount());
        }

        dst.close();
        fs::last_write_time(dest, fs::last_write_time(source));

        auto fileSize = fs::file_size(source);
        stats.totalBytes += fileSize;
        ++stats.filesCopied;

        safePrint("  [COPY]  " + source.filename().string() +
                  " (" + formatBytes(fileSize) + ")");
        return true;

    } catch (const std::exception& e) {
        safePrint("  [ERROR] " + source.string() + " → " + e.what());
        ++stats.errors;
        return false;
    }
}

// ─────────────────────────────────────────────
// CLI
// ─────────────────────────────────────────────
void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " <source> <destination> [options]\n"
              << "\nOptions:\n"
              << "  -t, --threads N    Number of threads (default: hardware concurrency)\n"
              << "  -s, --shallow      Copy only files in root directory\n"
              << "  -h, --help         Show this help message\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    fs::path sourcePath = argv[1];
    fs::path destPath   = argv[2];
    bool     recursive  = true;
    size_t   numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 4;

    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
            numThreads = std::stoul(argv[++i]);
        } else if (arg == "-s" || arg == "--shallow") {
            recursive = false;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (!fs::exists(sourcePath)) {
        std::cerr << "Error: Source path does not exist: " << sourcePath << "\n";
        return 1;
    }

    fs::create_directories(destPath);

    BackupStats stats;
    auto startTime = std::chrono::steady_clock::now();

    std::cout << "Using " << numThreads << " threads\n";

    // Collect file pairs
    struct CopyTask { fs::path source, dest; };
    std::vector<CopyTask> fileTasks;

    if (fs::is_directory(sourcePath)) {
        std::cout << "\nScanning directory...\n";
        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(sourcePath)) {
                if (fs::is_regular_file(entry)) {
                    fs::path rel = fs::relative(entry.path(), sourcePath);
                    fileTasks.push_back({entry.path(), destPath / rel});
                }
            }
        } else {
            for (const auto& entry : fs::directory_iterator(sourcePath)) {
                if (fs::is_regular_file(entry)) {
                    fileTasks.push_back({entry.path(),
                                         destPath / entry.path().filename()});
                }
            }
        }
    } else {
        fileTasks.push_back({sourcePath, destPath / sourcePath.filename()});
    }

    std::cout << "Found " << fileTasks.size() << " files to process\n"
              << std::string(50, '-') << "\n";

    // FIX 4 & 5: enqueue lambdas into the pool, then wait for all to finish.
    {
        ThreadPool pool(numThreads);

        for (const auto& task : fileTasks) {
            // Capture by value so each lambda owns its own paths.
            pool.enqueue([src = task.source, dst = task.dest, &stats]() {
                copyFile(src, dst, stats);
            });
        }

        pool.waitForCompletion(); // FIX 5: this is now actually reached
    } // pool destructor joins all threads cleanly

    auto endTime  = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        endTime - startTime);

    std::cout << "\n" << std::string(50, '=') << "\n"
              << "BACKUP SUMMARY (Threaded)\n"
              << std::string(50, '=') << "\n"
              << "Files copied:   " << stats.filesCopied  << "\n"
              << "Files skipped:  " << stats.filesSkipped << "\n"
              << "Errors:         " << stats.errors       << "\n"
              << "Total size:     " << formatBytes(stats.totalBytes) << "\n"
              << "Time elapsed:   " << duration.count() << " ms\n"
              << "Threads used:   " << numThreads << "\n"
              << std::string(50, '=') << "\n";

    return (stats.errors > 0) ? 1 : 0;
}