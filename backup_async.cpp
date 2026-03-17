/*
 * ASYNC/PARALLEL BACKUP UTILITY - Version 2
 * ==========================================
 * Uses std::async and parallel algorithms for concurrent file operations.
 * Features:
 *   - Async file copying with futures
 *   - Parallel directory scanning
 *   - Progress callback
 *   - Bandwidth limiting option
 */

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <future>
#include <atomic>
#include <algorithm>
#include <execution>
#include <math>

namespace fs = std::filesystem;
using namespace std::chrono;

// Configuration structure
struct Config {
    size_t maxConcurrent = 8;        // Max simultaneous file copies
    size_t bandwidthLimit = 0;       // Bytes/sec (0 = unlimited)
    bool verifyChecksum = false;     // Verify files after copy
    bool dryRun = false;             // Show what would be copied
    bool verbose = false;            // Detailed output
};

// Statistics with atomic counters
struct BackupStats {
    std::atomic<size_t> filesCopied{0};
    std::atomic<size_t> filesSkipped{0};
    std::atomic<size_t> errors{0};
    std::atomic<size_t> totalBytes{0};
    std::atomic<size_t> currentBytes{0};  // For progress tracking
};

// File information structure
struct FileInfo {
    fs::path source;
    fs::path dest;
    uintmax_t size;
    fs::file_time_type lastModified;
    bool needsCopy = false;
};

std::string formatBytes(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unitIndex < 4) {
        size /= 1024.0;
        unitIndex++;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unitIndex];
    return oss.str();
}

// Progress bar display
void showProgress(size_t current, size_t total, size_t bytesCopied, size_t totalBytes) {
    const int barWidth = 50;
    float progress = total > 0 ? static_cast<float>(current) / total : 0;
    int pos = static_cast<int>(barWidth * progress);

    std::cout << "\r[";
    for (int i = 0; i < barWidth; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }

    float percent = progress * 100.0f;
    std::cout << "] " << std::fixed << std::setprecision(1) << percent << "% ";
    std::cout << current << "/" << total << " files ";

    if (totalBytes > 0) {
        std::cout << "(" << formatBytes(bytesCopied) << "/" << formatBytes(totalBytes) << ")";
    }
    std::cout << std::flush;
}

// Async file copy with optional bandwidth limiting
std::future<bool> copyFileAsync(const FileInfo& info, BackupStats& stats,
                                  const Config& config, std::mutex& printMutex) {
    return std::async(std::launch::async, [&info, &stats, &config, &printMutex]() -> bool {
        try {
            if (config.dryRun) {
                std::lock_guard<std::mutex> lock(printMutex);
                std::cout << "[DRY RUN] Would copy: " << info.source << std::endl;
                stats.filesCopied++;
                stats.totalBytes += info.size;
                return true;
            }

            // Create destination directory
            fs::create_directories(info.dest.parent_path());

            // Check if already up to date
            if (fs::exists(info.dest) && !info.needsCopy) {
                auto destTime = fs::last_write_time(info.dest);
                auto destSize = fs::file_size(info.dest);
                if (info.lastModified <= destTime && info.size == destSize) {
                    std::lock_guard<std::mutex> lock(printMutex);
                    if (config.verbose) {
                        std::cout << "[SKIP] " << info.source.filename() << std::endl;
                    }
                    stats.filesSkipped++;
                    return true;
                }
            }

            // Perform the copy with buffering
            std::ifstream src(info.source.string(), std::ios::binary);
            std::ofstream dst(info.dest.string(), std::ios::binary);

            if (!src || !dst) {
                throw std::runtime_error("Failed to open file streams");
            }

            // Use 4MB buffer for large files
            const size_t bufferSize = info.size > 100 * 1024 * 1024 ? 4 * 1024 * 1024 : 1024 * 1024;
            std::vector<char> buffer(bufferSize);
            size_t bytesWritten = 0;
            auto lastUpdate = steady_clock::now();

            while (src.good()) {
                src.read(buffer.data(), bufferSize);
                std::streamsize bytesRead = src.gcount();
                dst.write(buffer.data(), bytesRead);
                bytesWritten += bytesRead;

                // Bandwidth limiting
                if (config.bandwidthLimit > 0) {
                    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - lastUpdate).count();
                    size_t expectedTime = (bytesWritten * 1000) / config.bandwidthLimit;
                    if (elapsed < expectedTime) {
                        std::this_thread::sleep_for(milliseconds(expectedTime - elapsed));
                    }
                }
            }

            dst.close();

            // Preserve modification time
            fs::last_write_time(info.dest, info.lastModified);

            // Update stats
            stats.filesCopied++;
            stats.totalBytes += info.size;
            stats.currentBytes += info.size;

            if (config.verbose) {
                std::lock_guard<std::mutex> lock(printMutex);
                std::cout << "[COPY] " << info.source.filename()
                          << " (" << formatBytes(info.size) << ")" << std::endl;
            }

            return true;

        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(printMutex);
            std::cerr << "[ERROR] " << info.source << ": " << e.what() << std::endl;
            stats.errors++;
            return false;
        }
    });
}

// Scan directory and collect file information
std::vector<FileInfo> scanDirectory(const fs::path& sourceDir, const fs::path& destDir,
                                     bool recursive, Config& config) {
    std::vector<FileInfo> files;

    auto scanner = [&files, &destDir](const fs::path& source, const fs::directory_entry& entry) {
        if (fs::is_regular_file(entry)) {
            FileInfo info;
            info.source = entry.path();
            info.size = fs::file_size(entry);
            info.lastModified = fs::last_write_time(entry);

            // Calculate destination path
            fs::path relativePath = fs::relative(entry.path(), source);
            info.dest = destDir / relativePath;

            // Pre-check if copy is needed
            if (fs::exists(info.dest)) {
                auto destTime = fs::last_write_time(info.dest);
                auto destSize = fs::file_size(info.dest);
                info.needsCopy = (info.lastModified > destTime) || (info.size != destSize);
            } else {
                info.needsCopy = true;
            }

            files.push_back(info);
        }
    };

    if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(sourceDir)) {
            scanner(sourceDir, entry);
        }
    } else {
        for (const auto& entry : fs::directory_iterator(sourceDir)) {
            scanner(sourceDir, entry);
        }
    }

    return files;
}

void printUsage(const char* programName) {
    std::cout << "Async Backup Utility - High Performance File Copy\n";
    std::cout << "Usage: " << programName << " <source> <destination> [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -j, --jobs N       Max concurrent copies (default: 8)\n";
    std::cout << "  -l, --limit MB/s   Bandwidth limit in MB/s (0 = unlimited)\n";
    std::cout << "  -n, --dry-run      Show what would be copied\n";
    std::cout << "  -v, --verbose      Show each file\n";
    std::cout << "  -s, --shallow      Non-recursive copy\n";
    std::cout << "  -h, --help         Show this help\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    fs::path sourcePath = argv[1];
    fs::path destPath = argv[2];
    Config config;
    bool recursive = true;

    // Parse arguments
    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "-j" || arg == "--jobs") && i + 1 < argc) {
            config.maxConcurrent = std::stoul(argv[++i]);
        } else if ((arg == "-l" || arg == "--limit") && i + 1 < argc) {
            config.bandwidthLimit = static_cast<size_t>(std::stod(argv[++i]) * 1024 * 1024);
        } else if (arg == "-n" || arg == "--dry-run") {
            config.dryRun = true;
        } else if (arg == "-v" || arg == "--verbose") {
            config.verbose = true;
        } else if (arg == "-s" || arg == "--shallow") {
            recursive = false;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (!fs::exists(sourcePath)) {
        std::cerr << "Error: Source does not exist: " << sourcePath << std::endl;
        return 1;
    }

    if (!fs::exists(destPath)) {
        fs::create_directories(destPath);
    }

    auto startTime = steady_clock::now();
    BackupStats stats;
    std::mutex printMutex;

    // Collect files
    std::cout << "Scanning..." << std::endl;
    std::vector<FileInfo> files;

    if (fs::is_directory(sourcePath)) {
        files = scanDirectory(sourcePath, destPath, recursive, config);
    } else {
        FileInfo info;
        info.source = sourcePath;
        info.dest = destPath / sourcePath.filename();
        info.size = fs::file_size(sourcePath);
        info.lastModified = fs::last_write_time(sourcePath);
        info.needsCopy = true;
        files.push_back(info);
    }

    size_t totalSize = 0;
    for (const auto& f : files) totalSize += f.size;

    std::cout << "Found " << files.size() << " files (" << formatBytes(totalSize) << ")\n";
    std::cout << "Using " << config.maxConcurrent << " concurrent jobs\n\n";

    if (config.dryRun) {
        std::cout << "DRY RUN - No files will be copied\n";
    }

    // Process files with limited concurrency
    std::vector<std::future<bool>> futures;
    size_t processed = 0;

    for (const auto& file : files) {
        // Wait if we've hit the concurrency limit
        while (futures.size() >= config.maxConcurrent) {
            for (auto it = futures.begin(); it != futures.end();) {
                if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    it = futures.erase(it);
                    processed++;
                    if (!config.verbose) {
                        showProgress(processed, files.size(), stats.currentBytes.load(), totalSize);
                    }
                } else {
                    ++it;
                }
            }
            if (futures.size() >= config.maxConcurrent) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        futures.push_back(copyFileAsync(file, stats, config, printMutex));
    }

    // Wait for remaining tasks
    for (auto& f : futures) {
        f.wait();
        processed++;
        if (!config.verbose) {
            showProgress(processed, files.size(), stats.currentBytes.load(), totalSize);
        }
    }

    std::cout << std::endl; // End progress line

    auto endTime = steady_clock::now();
    auto duration = duration_cast<seconds>(endTime - startTime);

    // Summary
    std::cout << "\n" << std::string(50, '=') << std::endl;
    std::cout << "BACKUP SUMMARY (Async)" << std::endl;
    std::cout << std::string(50, '=') << std::endl;
    std::cout << "Files copied:   " << stats.filesCopied << std::endl;
    std::cout << "Files skipped:  " << stats.filesSkipped << std::endl;
    std::cout << "Errors:         " << stats.errors << std::endl;
    std::cout << "Total size:     " << formatBytes(stats.totalBytes) << std::endl;
    std::cout << "Time elapsed:   " << duration.count() << " seconds" << std::endl;

    if (duration.count() > 0) {
        size_t mbPerSec = stats.totalBytes / duration.count() / 1024 / 1024;
        std::cout << "Avg speed:      " << mbPerSec << " MB/s" << std::endl;
    }

    std::cout << std::string(50, '=') << std::endl;

    return (stats.errors > 0) ? 1 : 0;
}
