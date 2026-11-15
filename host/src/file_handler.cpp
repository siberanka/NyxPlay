/**
 * @file file_handler.cpp
 * @brief Definitions for file handling functions.
 */

// standard includes
#include <filesystem>
#include <fstream>

// local includes
#include "file_handler.h"
#include "logging.h"

namespace file_handler {
  std::string get_parent_directory(const std::string &path) {
    // remove any trailing path separators
    std::string trimmed_path = path;
    while (!trimmed_path.empty() && trimmed_path.back() == '/') {
      trimmed_path.pop_back();
    }

    std::filesystem::path p(trimmed_path);
    return p.parent_path().string();
  }

  bool make_directory(const std::string &path) {
    // first, check if the directory already exists
    if (std::filesystem::exists(path)) {
      return true;
    }

    return std::filesystem::create_directories(path);
  }

  std::string read_file(const char *path) {
    if (!std::filesystem::exists(path)) {
      BOOST_LOG(debug) << "Missing file: " << path;
      return {};
    }

    std::ifstream in(path);
    return std::string {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
  }

  int write_file(const char *path, const std::string_view &contents) {
    std::ofstream out(path);

    if (!out.is_open()) {
      return -1;
    }

    out << contents;

    return 0;
  }

  int write_file_atomic(const char *path, const std::string_view &contents) {
    namespace fs = std::filesystem;

    const fs::path target {path};
    fs::path temp = target;
    temp += ".tmp";

    std::error_code ec;
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      if (!out.is_open()) {
        return -1;
      }
      out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
      out.flush();
      if (!out.good()) {
        out.close();
        fs::remove(temp, ec);
        return -1;
      }
    }

    // The rename replaces the target in one step: readers see the old or the new file, never a partial one
    fs::rename(temp, target, ec);
    if (ec) {
      fs::remove(temp, ec);
      return -1;
    }
    return 0;
  }
}  // namespace file_handler
