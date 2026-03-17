/*
 * FILE BACKUP UTILITY
 * ===================
 * A simple C++ program to backup/copy files from one location to another.
 * Features:
 *   - Copy single files or entire directories
 *   - Recursive copying (preserves folder structure)
 *   - Skips files that are already up-to-date
 *   - Shows progress and summary statistics
 *
 * Compile: g++ -std=c++17 -o backup backup_utility.cpp
 * Usage:   backup.exe <source> <destination> [options]
 */

// ============ HEADER FILES ============
// iostream  - For console input/output (cout, cerr)
// fstream   - For file operations
// filesystem - For directory and path handling (C++17 feature)
// string    - For text manipulation
// vector    - For dynamic arrays
// chrono    - For timing operations
// iomanip   - For output formatting (setprecision, fixed)
// sstream   - For string stream operations (ostringstream)
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>

// Create a shorter alias for the filesystem namespace
// Instead of typing std::filesystem:: every time, we can just use fs::
namespace fs = std::filesystem;

// ============ DATA STRUCTURES ============

/**
 * BackupStats - Structure to keep track of backup operation statistics
 * This helps us count what happened during the backup process
 */
struct BackupStats {
    size_t filesCopied = 0;    // Count of files that were copied
    size_t filesSkipped = 0;   // Count of files skipped (already up-to-date)
    size_t errors = 0;           // Count of errors encountered
    size_t totalBytes = 0;     // Total bytes copied
};

// ============ HELPER FUNCTIONS ============

/**
 * formatBytes - Converts a byte count to human-readable format
 * Example: 1536 bytes -> "1.50 KB"
 *          2097152 bytes -> "2.00 MB"
 *
 * @param bytes - The number of bytes to format
 * @return A string like "1.50 MB" or "100.00 B"
 */
std::string formatBytes(size_t bytes) {
    // Array of unit labels from smallest to largest
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;  // Start with Bytes

    // Convert to double for division
    double size = static_cast<double>(bytes);

    // Keep dividing by 1024 until we get a reasonable number
    // or we run out of units (stop at TB)
    while (size >= 1024.0 && unitIndex < 4) {
        size /= 1024.0;  // Divide by 1024 (binary: 2^10)
        unitIndex++;      // Move to next unit (B -> KB -> MB, etc.)
    }

    // Create a string stream to format the output
    std::ostringstream oss;
    // Set fixed notation with 2 decimal places
    // Example: 1.5 becomes "1.50"
    oss << std::fixed << std::setprecision(2) << size << " " << units[unitIndex];
    return oss.str();
}

/**
 * copyFile - Copies a single file from source to destination
 * This function handles:
 *   - Creating parent directories if needed
 *   - Checking if file already exists and is up-to-date
 *   - Preserving file modification times
 *   - Error handling
 *
 * @param source - Path to the source file
 * @param dest   - Path where the file should be copied to
 * @param stats  - Reference to statistics structure (updated by this function)
 * @return true if successful, false if an error occurred
 */
bool copyFile(const fs::path& source, const fs::path& dest, BackupStats& stats) {
    try {
        // Step 1: Create the destination directory if it doesn't exist
        // parent_path() gets the folder containing the file
        // Example: "C:/backup/docs/file.txt" -> "C:/backup/docs"
        fs::create_directories(dest.parent_path());

        // Step 2: Check if destination file already exists
        // If it does, compare timestamps and sizes to see if we need to copy
        if (fs::exists(dest)) {
            // Get the last modification time of both files
            auto sourceTime = fs::last_write_time(source);
            auto destTime = fs::last_write_time(dest);

            // Get the file sizes
            auto sourceSize = fs::file_size(source);
            auto destSize = fs::file_size(dest);

            // If destination is newer or same age AND same size, skip it
            // This saves time by not copying files that haven't changed
            if (sourceTime <= destTime && sourceSize == destSize) {
                std::cout << "  [SKIP] " << source.filename().string() << " (up to date)" << std::endl;
                stats.filesSkipped++;  // Increment skip counter
                return true;           // Return success (nothing to do)
            }
        }

        // Step 3: Copy the file
        // fs::copy_options::overwrite_existing means replace if it already exists
        fs::copy_file(source, dest, fs::copy_options::overwrite_existing);

        // Step 4: Preserve the original file's modification time
        // This helps with the "up to date" check on future runs
        fs::last_write_time(dest, fs::last_write_time(source));

        // Step 5: Update statistics
        auto fileSize = fs::file_size(source);
        stats.totalBytes += fileSize;    // Add to total bytes copied
        stats.filesCopied++;              // Increment copy counter

        // Step 6: Print success message with file size
        std::cout << "  [COPY] " << source.filename().string()
                  << " (" << formatBytes(fileSize) << ")" << std::endl;

        return true;  // Success!

    } catch (const fs::filesystem_error& e) {
        // If anything goes wrong (permissions, disk full, etc.), catch it here
        std::cerr << "  [ERROR] Failed to copy " << source << ": " << e.what() << std::endl;
        stats.errors++;  // Increment error counter
        return false;    // Return failure
    }
}

/**
 * backupDirectory - Backs up an entire directory (optionally recursively)
 *
 * @param sourceDir  - Path to the source directory
 * @param destDir    - Path to the destination directory
 * @param stats      - Reference to statistics structure
 * @param recursive  - If true, copy subdirectories too; if false, only copy files in root
 */
void backupDirectory(const fs::path& sourceDir, const fs::path& destDir,
                     BackupStats& stats, bool recursive = true) {
    // Validate: Make sure source directory exists
    if (!fs::exists(sourceDir)) {
        std::cerr << "Source directory does not exist: " << sourceDir << std::endl;
        return;  // Can't backup what doesn't exist
    }

    // Validate: Make sure source is actually a directory, not a file
    if (!fs::is_directory(sourceDir)) {
        std::cerr << "Source is not a directory: " << sourceDir << std::endl;
        return;
    }

    // Print header showing what we're backing up
    std::cout << "\nBacking up: " << sourceDir << " -> " << destDir << std::endl;
    std::cout << std::string(50, '-') << std::endl;  // Print 50 dashes as a separator

    try {
        if (recursive) {
            // RECURSIVE MODE: Walk through ALL files in ALL subdirectories
            // fs::recursive_directory_iterator goes into subfolders automatically
            for (const auto& entry : fs::recursive_directory_iterator(sourceDir)) {
                // Only process regular files (skip directories, symlinks, etc.)
                if (fs::is_regular_file(entry)) {
                    // Calculate the relative path to preserve folder structure
                    // Example: source="C:/docs", entry="C:/docs/work/file.txt"
                    //          relativePath = "work/file.txt"
                    fs::path relativePath = fs::relative(entry.path(), sourceDir);

                    // Build the full destination path
                    // Example: destDir="D:/backup", relativePath="work/file.txt"
                    //          destPath = "D:/backup/work/file.txt"
                    fs::path destPath = destDir / relativePath;

                    // Copy this file
                    copyFile(entry.path(), destPath, stats);
                }
            }
        } else {
            // SHALLOW MODE: Only copy files in the root directory
            // fs::directory_iterator only looks at immediate children
            for (const auto& entry : fs::directory_iterator(sourceDir)) {
                if (fs::is_regular_file(entry)) {
                    // Just use the filename, no subdirectories
                    // Example: entry="C:/docs/file.txt"
                    //          destPath = "D:/backup/file.txt"
                    fs::path destPath = destDir / entry.path().filename();
                    copyFile(entry.path(), destPath, stats);
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        // Handle errors like permission denied when reading directory
        std::cerr << "Error accessing directory: " << e.what() << std::endl;
        stats.errors++;
    }
}

/**
 * backupSingleFile - Backs up a single file (not a directory)
 *
 * @param sourceFile - Path to the file to backup
 * @param destDir    - Directory where the file should be copied
 * @param stats      - Reference to statistics structure
 */
void backupSingleFile(const fs::path& sourceFile, const fs::path& destDir, BackupStats& stats) {
    // Validate: Make sure source file exists
    if (!fs::exists(sourceFile)) {
        std::cerr << "Source file does not exist: " << sourceFile << std::endl;
        return;
    }

    // Validate: Make sure it's a regular file (not a directory or special file)
    if (!fs::is_regular_file(sourceFile)) {
        std::cerr << "Source is not a regular file: " << sourceFile << std::endl;
        return;
    }

    // Build destination path: destination directory + source filename
    // Example: destDir="D:/backup", sourceFile="C:/docs/file.txt"
    //          destPath = "D:/backup/file.txt"
    fs::path destPath = destDir / sourceFile.filename();

    // Copy the file
    copyFile(sourceFile, destPath, stats);
}

/**
 * printUsage - Displays help message showing how to use the program
 *
 * @param programName - The name of the executable (argv[0])
 */
void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " <source> <destination> [options]" << std::endl;
    std::cout << "\nOptions:" << std::endl;
    std::cout << "  -r, --recursive    Copy directories recursively (default)" << std::endl;
    std::cout << "  -s, --shallow      Copy only files in root directory" << std::endl;
    std::cout << "  -h, --help         Show this help message" << std::endl;
    std::cout << "\nExamples:" << std::endl;
    std::cout << "  " << programName << " C:\\Users\\Me\\Documents D:\\Backup" << std::endl;
    std::cout << "  " << programName << " ./project ./backup --shallow" << std::endl;
}

// ============ MAIN FUNCTION ============

/**
 * main - Entry point of the program
 *
 * @param argc - Number of command-line arguments (including program name)
 * @param argv - Array of argument strings
 *             argv[0] = program name
 *             argv[1] = source path
 *             argv[2] = destination path
 *             argv[3+] = optional flags
 * @return 0 on success, 1 on error
 */
int main(int argc, char* argv[]) {
    // Check if user provided enough arguments
    // We need at least: program_name source destination
    if (argc < 3) {
        printUsage(argv[0]);  // Show help
        return 1;             // Return error code
    }

    // Convert command-line arguments to filesystem paths
    fs::path sourcePath = argv[1];   // First argument: what to backup
    fs::path destPath = argv[2];     // Second argument: where to put it
    bool recursive = true;           // Default to recursive mode

    // Parse optional command-line flags
    // Start from index 3 (after program name, source, and dest)
    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-r" || arg == "--recursive") {
            recursive = true;   // Enable recursive mode (already default)
        } else if (arg == "-s" || arg == "--shallow") {
            recursive = false;  // Disable recursive mode (shallow copy only)
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;  // Return success (help was shown as requested)
        }
    }

    // Validate: Source must exist
    if (!fs::exists(sourcePath)) {
        std::cerr << "Error: Source path does not exist: " << sourcePath << std::endl;
        return 1;  // Return error code
    }

    // Create destination directory if it doesn't exist
    // This prevents errors when copying to a new location
    if (!fs::exists(destPath)) {
        std::cout << "Creating destination directory: " << destPath << std::endl;
        fs::create_directories(destPath);
    }

    // Create a statistics object to track what we do
    BackupStats stats;

    // Record the start time for performance measurement
    auto startTime = std::chrono::steady_clock::now();

    // ============ PERFORM THE BACKUP ============
    // Check if source is a directory or a single file
    if (fs::is_directory(sourcePath)) {
        // Source is a directory - use directory backup function
        backupDirectory(sourcePath, destPath, stats, recursive);
    } else {
        // Source is a single file - use single file backup function
        backupSingleFile(sourcePath, destPath, stats);
    }

    // Record the end time and calculate duration
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime);

    // ============ PRINT SUMMARY ============
    std::cout << "\n" << std::string(50, '=') << std::endl;
    std::cout << "BACKUP SUMMARY" << std::endl;
    std::cout << std::string(50, '=') << std::endl;
    std::cout << "Files copied:   " << stats.filesCopied << std::endl;
    std::cout << "Files skipped:  " << stats.filesSkipped << std::endl;
    std::cout << "Errors:         " << stats.errors << std::endl;
    std::cout << "Total size:     " << formatBytes(stats.totalBytes) << std::endl;
    std::cout << "Time elapsed:   " << duration.count() << " seconds" << std::endl;
    std::cout << std::string(50, '=') << std::endl;

    // Return error code if there were any errors, otherwise success
    return (stats.errors > 0) ? 1 : 0;
}
