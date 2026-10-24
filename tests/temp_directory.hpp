/// @file temp_directory.hpp
/// @brief A private directory under /tmp for file tests, removed with everything in it afterwards.

#ifndef COMMON_TEST_TEMP_DIRECTORY_HPP
#define COMMON_TEST_TEMP_DIRECTORY_HPP

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace test {

class TempDirectory final {
public:
    TempDirectory() {
        std::string pattern{"/tmp/common_test_XXXXXX"};
        const char* const created{::mkdtemp(pattern.data())};
        path_ = (created != nullptr) ? std::string{created} : std::string{};
    }
    ~TempDirectory() {
        if (!path_.empty()) {
            remove_tree(path_, 0U);
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

    std::string path_;
};

}  // namespace test

#endif  // COMMON_TEST_TEMP_DIRECTORY_HPP
