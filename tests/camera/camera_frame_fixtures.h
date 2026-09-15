#pragma once

// CVF-002 runtime frame fixtures (owner: test-engineer).
//
// Deterministic PPM/PGM files generated at run time into a per-test temporary
// directory, so the suite is hermetic, hardware-free, and independent of the
// process working directory.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cvf002 {

class TempFrameDir {
 public:
  explicit TempFrameDir(const std::string& tag) {
    static std::atomic<std::uint64_t> counter{0};
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    dir_ = std::filesystem::temp_directory_path() /
           ("cvforwin_cvf002_" + tag + "_" + std::to_string(stamp) + "_" +
            std::to_string(counter.fetch_add(1)));
    std::error_code error;
    std::filesystem::create_directories(dir_, error);
    if (error) {
      throw std::runtime_error("cannot create temporary directory: " + dir_.string());
    }
  }

  ~TempFrameDir() {
    std::error_code error;
    std::filesystem::remove_all(dir_, error);
  }

  TempFrameDir(const TempFrameDir&) = delete;
  TempFrameDir& operator=(const TempFrameDir&) = delete;

  std::string path(const std::string& name) const { return (dir_ / name).string(); }

  // Portable pixmap (P6), pixels in row-major order, each pixel as {r, g, b}.
  std::string write_ppm_p6(const std::string& name, int width, int height,
                           const std::vector<std::array<std::uint8_t, 3>>& rgb_pixels) {
    if (static_cast<std::size_t>(width) * static_cast<std::size_t>(height) != rgb_pixels.size()) {
      throw std::runtime_error("PPM pixel count does not match width*height");
    }
    const std::string file = path(name);
    std::ofstream out(file, std::ios::binary);
    out << "P6\n" << width << " " << height << "\n255\n";
    for (const auto& pixel : rgb_pixels) {
      const char bytes[3] = {static_cast<char>(pixel[0]), static_cast<char>(pixel[1]),
                             static_cast<char>(pixel[2])};
      out.write(bytes, 3);
    }
    out.flush();
    if (!out) {
      throw std::runtime_error("cannot write PPM fixture: " + file);
    }
    return file;
  }

  // Portable graymap (P5), samples in row-major order.
  std::string write_pgm_p5(const std::string& name, int width, int height,
                           const std::vector<std::uint8_t>& gray_samples) {
    if (static_cast<std::size_t>(width) * static_cast<std::size_t>(height) != gray_samples.size()) {
      throw std::runtime_error("PGM sample count does not match width*height");
    }
    const std::string file = path(name);
    std::ofstream out(file, std::ios::binary);
    out << "P5\n" << width << " " << height << "\n255\n";
    for (const auto sample : gray_samples) {
      const char byte = static_cast<char>(sample);
      out.write(&byte, 1);
    }
    out.flush();
    if (!out) {
      throw std::runtime_error("cannot write PGM fixture: " + file);
    }
    return file;
  }

  std::string write_undecodable(const std::string& name) {
    const std::string file = path(name);
    std::ofstream out(file, std::ios::binary);
    out << "CVF-002 fixture: this file is intentionally not a decodable image.\n";
    out.flush();
    if (!out) {
      throw std::runtime_error("cannot write undecodable fixture: " + file);
    }
    return file;
  }

 private:
  std::filesystem::path dir_;
};

}  // namespace cvf002
