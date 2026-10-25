/// @file temp_directory.hpp
/// @brief A private directory under /tmp (on Windows, under %TEMP%) for file tests, removed with
///        everything in it afterwards.

#ifndef COMMON_TEST_TEMP_DIRECTORY_HPP
#define COMMON_TEST_TEMP_DIRECTORY_HPP

#if defined(_WIN32)
#include <process.h>

#include <atomic>
#include <filesystem>
#include <system_error>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace test {

class TempDirectory final {
public:
    TempDirectory() {
#if defined(_WIN32)
        // Like mkdtemp(): a new directory whose name no other process or test uses.
        static std::atomic<std::uint32_t> next{0U};
        std::error_code error{};
        const std::filesystem::path base{std::filesystem::temp_directory_path(error)};
        for (std::uint32_t attempt{0U}; (!error) && path_.empty() && (attempt < 100U); ++attempt) {
            const std::string name{"common_test_" + std::to_string(::_getpid()) + "_" + std::to_string(next.fetch_add(1U))};
            const std::filesystem::path candidate{base / name};
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate.u8string();
            }
        }
#else
        std::string pattern{"/tmp/common_test_XXXXXX"};
        const char* const created{::mkdtemp(pattern.data())};
        path_ = (created != nullptr) ? std::string{created} : std::string{};
#endif
    }
    ~TempDirectory() {
        if (!path_.empty()) {
#if defined(_WIN32)
            std::error_code error{};
            static_cast<void>(std::filesystem::remove_all(std::filesystem::u8path(path_), error));
#else
            remove_tree(path_, 0U);
#endif
        }
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;

    [[nodiscard]] bool valid() const {
        return !path_.empty();
    }
    [[nodiscard]] const std::string& path() const {
        return path_;
    }
    /// Path of an entry in the directory.
    [[nodiscard]] std::string entry(const std::string& name) const {
        return path_ + "/" + name;
    }
    /// Names of the entries in the directory, without "." and "..".
    [[nodiscard]] std::vector<std::string> list() const {
        return names_in(path_);
    }

private:
#if defined(_WIN32)
    static std::vector<std::string> names_in(const std::string& directory) {
        std::vector<std::string> names{};
        std::error_code error{};
        std::filesystem::directory_iterator entry{std::filesystem::u8path(directory), error};
        for (std::uint32_t count{0U}; (!error) && (entry != std::filesystem::directory_iterator{}) && (count < 10000U);
             ++count) {
            names.push_back(entry->path().filename().u8string());
            entry.increment(error);
        }
        return names;
    }
#else
    static std::vector<std::string> names_in(const std::string& directory) {
        std::vector<std::string> names{};
        DIR* const stream{::opendir(directory.c_str())};
        if (stream != nullptr) {
            for (const dirent* entry{::readdir(stream)}; entry != nullptr; entry = ::readdir(stream)) {
                const std::string name{entry->d_name};
                if ((name != ".") && (name != "..")) {
                    names.push_back(name);
                }
            }
            static_cast<void>(::closedir(stream));
        }
        return names;
    }

    /// Removes @p path and, if it is a directory, everything below it (to a fixed depth).
    static void remove_tree(const std::string& path, const std::uint32_t depth) {
        struct stat info {};
        if ((::lstat(path.c_str(), &info) == 0) && S_ISDIR(info.st_mode) && (depth < 8U)) {
            for (const std::string& name : names_in(path)) {
                remove_tree(path + "/" + name, depth + 1U);
            }
            static_cast<void>(::rmdir(path.c_str()));
        } else {
            static_cast<void>(::unlink(path.c_str()));
        }
    }
#endif

    std::string path_;
};

}  // namespace test

#endif  // COMMON_TEST_TEMP_DIRECTORY_HPP
