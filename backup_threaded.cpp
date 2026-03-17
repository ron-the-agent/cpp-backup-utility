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
#include <unordered_set>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────
// Thread-safe statistics
// ─────────────────────────────────────────────
struct BackupStats {
    // std::atomic ensures concurrent increments from multiple threads
    // don't race — no mutex needed for these counters.
    std::atomic<size_t> filesCopied{0};
    std::atomic<size_t> filesSkipped{0};
    std::atomic<size_t> errors{0};
    std::atomic<size_t> totalBytes{0};
};

// ─────────────────────────────────────────────
// Thread-safe directory creation cache
// ─────────────────────────────────────────────
struct DirCache {
    // Tracks which destination directories have already been created.
    // Prevents redundant create_directories() calls when many files
    // share the same parent folder (e.g. 1000 files → 1 syscall, not 1000).
    std::unordered_set<std::string> created;
    std::mutex                      mutex;

    // Creates the directory only if it hasn't been seen before.
    void ensureExists(const fs::path& dir) {
        const std::string key = dir.string();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (created.count(key)) return;  // Already created, skip.
            created.insert(key);
        }
        // Call create_directories outside the lock — it's slow on network
        // drives and we don't want to block other threads while it runs.
        fs::create_directories(dir);
    }
};
// FIX 1: Queue holds std::function<void()> so any callable can be submitted.
// ─────────────────────────────────────────────
class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads) : stop(false), activeTasks(0) {
        // Spin up N persistent worker threads — they'll sleep until work arrives.
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back([this] { workerLoop(); });
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            stop = true;  // Signal all workers to exit once their queue is empty.
        }
        condition.notify_all();  // Wake every sleeping thread so they can see stop=true.
        for (std::thread& worker : workers) {
            if (worker.joinable()) worker.join();  // Wait for each thread to finish cleanly.
        }
    }

    // Submit any callable (e.g. a lambda) as a task.
    void enqueue(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            tasks.push(std::move(task));  // move avoids an extra copy of the callable.
        }
        condition.notify_one();  // Wake exactly one sleeping worker to pick it up.
    }

    // Block the calling thread until the queue is empty and all active tasks have finished.
    void waitForCompletion() {
        std::unique_lock<std::mutex> lock(queueMutex);
        // 'done' CV is notified by workers after each task completes.
        // We re-check both conditions to guard against spurious wakeups.
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
                // Sleep until there's work to do or the pool is shutting down.
                condition.wait(lock, [this] { return stop || !tasks.empty(); });

                // If stop is set and nothing is left, this thread's job is done.
                if (stop && tasks.empty()) return;

                task = std::move(tasks.front());
                tasks.pop();
                ++activeTasks;
                // Lock is released here (end of scope) before task() runs,
                // so other workers can dequeue their own tasks concurrently.
            }

            task();  // Execute the actual work (e.g. copyFile).

            {
                std::unique_lock<std::mutex> lock(queueMutex);
                --activeTasks;
            }
            // Notify waitForCompletion() in case this was the last in-flight task.
            done.notify_one();
        }
    }
};

// ─────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────
// Converts a raw byte count into a human-readable string (e.g. 1048576 → "1.00 MB").
std::string formatBytes(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double size = static_cast<double>(bytes);
    // Repeatedly divide by 1024 until the value fits under the next unit.
    while (size >= 1024.0 && unitIndex < 4) {
        size /= 1024.0;
        ++unitIndex;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unitIndex];
    return oss.str();
}

// printMutex ensures that concurrent threads don't interleave their output lines.
std::mutex printMutex;
void safePrint(const std::string& msg) {
    std::lock_guard<std::mutex> lock(printMutex);
    std::cout << msg << "\n";
}

// ─────────────────────────────────────────────
// Core copy logic
// ─────────────────────────────────────────────
bool copyFile(const fs::path& source, const fs::path& dest, BackupStats& stats, DirCache& dirCache) {
    try {
        // Use the cache to avoid redundant filesystem calls for the same directory.
        dirCache.ensureExists(dest.parent_path());

        // Skip the file if it already exists at the destination and hasn't changed.
        // We compare both timestamp and size to avoid unnecessary copies.
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

        // Use a 1MB buffer instead of byte-by-byte copies — significantly faster
        // for large files as it reduces the number of system calls made.
        const size_t       bufferSize = 1024 * 1024;
        std::vector<char>  buffer(bufferSize);

        while (src.good()) {
            src.read(buffer.data(), bufferSize);
            dst.write(buffer.data(), src.gcount());  // gcount() = bytes actually read
        }

        dst.close();
        // Preserve the original file's last-modified timestamp on the copy.
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
    if (numThreads == 0) numThreads = 4;  // hardware_concurrency() can return 0 if unknown.

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
    DirCache    dirCache;  // Shared across all threads to deduplicate directory creation.
    auto startTime = std::chrono::steady_clock::now();

    std::cout << "Using " << numThreads << " threads\n";

    // Collect file pairs
    struct CopyTask { fs::path source, dest; };
    std::vector<CopyTask> fileTasks;

    if (fs::is_directory(sourcePath)) {
        std::cout << "\nScanning directory...\n";
        if (recursive) {
            // recursive_directory_iterator walks all subdirectories automatically.
            for (const auto& entry : fs::recursive_directory_iterator(sourcePath)) {
                if (fs::is_regular_file(entry)) {
                    // Compute path relative to source so the folder structure
                    // is mirrored exactly under the destination.
                    fs::path rel = fs::relative(entry.path(), sourcePath);
                    fileTasks.push_back({entry.path(), destPath / rel});
                }
            }
        } else {
            // Shallow mode: only files directly inside sourcePath, no subdirectories.
            for (const auto& entry : fs::directory_iterator(sourcePath)) {
                if (fs::is_regular_file(entry)) {
                    fileTasks.push_back({entry.path(),
                                         destPath / entry.path().filename()});
                }
            }
        }
    } else {
        // Single file was passed — wrap it in a task directly.
        fileTasks.push_back({sourcePath, destPath / sourcePath.filename()});
    }

    std::cout << "Found " << fileTasks.size() << " files to process\n"
              << std::string(50, '-') << "\n";

    // Scoped block so the pool destructor runs before we print the summary,
    // guaranteeing all threads have finished and stats are fully updated.
    {
        ThreadPool pool(numThreads);

        for (const auto& task : fileTasks) {
            // Capture src and dst by value so each lambda owns its own copy of the
            // paths — avoids a dangling reference if the loop variable changes.
            pool.enqueue([src = task.source, dst = task.dest, &stats, &dirCache]() {
                copyFile(src, dst, stats, dirCache);
            });
        }

        // Block here until every enqueued task has completed.
        pool.waitForCompletion();
    } // pool destructor joins all worker threads cleanly before we continue.

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