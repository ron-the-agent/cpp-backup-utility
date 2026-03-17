# C++ Backup Utility

A simple, fast, and efficient file backup tool written in C++17. Copy files and directories while preserving structure and skipping unchanged files.

## Features

- **Copy single files or entire directories** - Flexible backup options
- **Recursive copying** - Preserves directory structure automatically
- **Smart skipping** - Skips files that are already up-to-date (checks size and modification time)
- **Progress reporting** - Shows each file being copied with human-readable sizes
- **Error handling** - Continues on errors, reports them at the end
- **Cross-platform** - Works on Windows, Linux, and macOS (requires C++17 compiler)

## Compilation

### Windows (Visual Studio)
```bash
cl /std:c++17 /EHsc backup_utility.cpp /Fe:backup.exe
```

### Linux/macOS (GCC/Clang)
```bash
g++ -std=c++17 -o backup backup_utility.cpp
```

## Usage

### Basic Syntax
```bash
backup <source> <destination> [options]
```

### Examples

**Backup a directory recursively (default):**
```bash
backup C:\Users\Me\Documents D:\Backup
backup ./my-project ./backups/project-backup
```

**Backup only root-level files (no subdirectories):**
```bash
backup ./project ./backup --shallow
```

**Backup a single file:**
```bash
backup important.txt D:\Backup
```

**Show help:**
```bash
backup --help
```

## Options

| Option | Description |
|--------|-------------|
| `-r, --recursive` | Copy directories recursively (default) |
| `-s, --shallow` | Copy only files in root directory, ignore subdirectories |
| `-h, --help` | Display help message |

## How It Works

### Directory Structure Preservation

When backing up a directory, the tool maintains the same folder hierarchy:

**Source:**
```
Documents/
├── Work/
│   ├── report.docx
│   └── budget.xlsx
├── Personal/
│   └── letter.txt
└── notes.txt
```

**After backup to `D:\Backup`:**
```
Backup/
├── Work/
│   ├── report.docx
│   └── budget.xlsx
├── Personal/
│   └── letter.txt
└── notes.txt
```

### Smart File Skipping

The tool checks two things before copying:
1. **File size** - If different, file has changed
2. **Modification time** - If source is newer, file has been modified

If both match the destination, the file is skipped (saves time on repeated backups).

## Output Format

```
Backing up: "test_source" -> "test_backup"
--------------------------------------------------
  [COPY] file1.txt (12.00 B)
  [SKIP] file2.txt (up to date)
  [COPY] large-file.zip (5.25 MB)
  [ERROR] Failed to copy restricted.pdf: Permission denied

==================================================
BACKUP SUMMARY
==================================================
Files copied:   2
Files skipped:  1
Errors:         1
Total size:     5.26 MB
Time elapsed:   3 seconds
==================================================
```

**Status indicators:**
- `[COPY]` - File was copied successfully
- `[SKIP]` - File already exists and is up-to-date
- `[ERROR]` - Failed to copy (permissions, disk full, etc.)

## Debug Options

### Build with Debug Symbols

**Windows (MSVC):**
```bash
cl /std:c++17 /EHsc /Zi /DEBUG backup_utility.cpp /Fe:backup.exe
```
- `/Zi` - Generate complete debugging information
- `/DEBUG` - Create PDB file for debugging

**Linux/macOS (GCC):**
```bash
g++ -std=c++17 -g -o backup backup_utility.cpp
```
- `-g` - Include debug information

**Linux/macOS (Clang):**
```bash
clang++ -std=c++17 -g -o backup backup_utility.cpp
```

### Runtime Debugging

The code includes several debugging features:

#### 1. Verbose Output (Modify Source)

Add this at the top of `main()` to see detailed path calculations:

```cpp
// Add after line: fs::path destPath = argv[2];
std::cout << "DEBUG: Source = " << fs::absolute(sourcePath) << std::endl;
std::cout << "DEBUG: Dest = " << fs::absolute(destPath) << std::endl;
std::cout << "DEBUG: Recursive = " << (recursive ? "true" : "false") << std::endl;
```

#### 2. Trace File Operations

In `copyFile()`, add before the try block:

```cpp
std::cout << "DEBUG: Attempting to copy: " << source << " -> " << dest << std::endl;
```

#### 3. Check File Existence

Test if the tool can see your files:

```cpp
// Add in main() before backup
std::cout << "DEBUG: Source exists: " << fs::exists(sourcePath) << std::endl;
std::cout << "DEBUG: Is directory: " << fs::is_directory(sourcePath) << std::endl;
```

### Common Issues & Solutions

| Issue | Cause | Solution |
|-------|-------|----------|
| `Source path does not exist` | Wrong path or permissions | Check path with `dir` (Windows) or `ls` (Linux) |
| `Permission denied` | Insufficient rights | Run as administrator or check file permissions |
| `Failed to copy` | Disk full or read-only | Check disk space and destination folder permissions |
| `not recognized as a command` | Not in PATH | Use full path: `C:\path\to\backup.exe` |

### Testing the Build

Create test files and verify functionality:

```bash
# Create test structure
mkdir test_source\subdir
echo "Hello" > test_source\file1.txt
echo "World" > test_source\subdir\file2.txt

# Run backup
backup.exe test_source test_backup

# Verify output
ls test_backup
cat test_backup\file1.txt
ls test_backup\subdir
```

## File Size Formatting

The tool automatically formats file sizes:
- Bytes (< 1024 B): `500 B`
- Kilobytes (1-1024 KB): `1.50 KB`
- Megabytes (1-1024 MB): `5.25 MB`
- Gigabytes (1-1024 GB): `2.50 GB`
- Terabytes (> 1024 GB): `1.50 TB`

## Requirements

- **Compiler:** C++17 compatible (MSVC 2017+, GCC 7+, Clang 5+)
- **OS:** Windows 7+, Linux, macOS
- **Libraries:** Standard library only (uses `<filesystem>`)

## License

This is a simple example project. Feel free to use and modify as needed.

## Contributing

To contribute:
1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit a pull request

## See Also

- [C++17 Filesystem Library](https://en.cppreference.com/w/cpp/filesystem)
- [std::filesystem::path](https://en.cppreference.com/w/cpp/filesystem/path)
- [std::filesystem::copy_file](https://en.cppreference.com/w/cpp/filesystem/copy_file)
