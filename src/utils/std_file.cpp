#include "../../include/utils/std_file.h"
#include <unistd.h>

namespace tiny_lsm {

bool StdFile::open(const std::string &filename, bool create) {
  filename_ = filename;

  if (create) {
    file_.open(filename, std::ios::in | std::ios::out | std::ios::binary |
                             std::ios::trunc);
  } else {
    file_.open(filename, std::ios::in | std::ios::out | std::ios::binary);
  }

  return file_.is_open();
}

bool StdFile::create(const std::string &filename, std::vector<uint8_t> &buf) {
  if (!this->open(filename, true)) {
    throw std::runtime_error("Failed to open file for writing");
  }
  if (!buf.empty()) {
    write(0, buf.data(), buf.size());
  }

  return true;
}

void StdFile::close() {
  if (file_.is_open()) {
    sync();
    file_.close();
  }
}

size_t StdFile::size() {
  file_.seekg(0, std::ios::end);
  return file_.tellg();
}

std::vector<uint8_t> StdFile::read(size_t offset, size_t length) {
  std::vector<uint8_t> buf(length);
  file_.seekg(offset, std::ios::beg);
  if (!file_.read(reinterpret_cast<char *>(buf.data()), length)) {
    throw std::runtime_error("Failed to read from file");
  }
  return buf;
}

bool StdFile::write(size_t offset, const void *data, size_t size) {
  file_.seekg(offset, std::ios::beg);
  file_.write(static_cast<const char *>(data), size);
  // this->sync();
  return true;
}

bool StdFile::sync() {
  if (!file_.is_open()) {
    return false;
  }
  file_.flush();
  return file_.good();
}

bool StdFile::remove() { return std::remove(filename_.c_str()) == 0; }

bool StdFile::truncate(size_t size) {
  if (file_.is_open())
    file_.close();
  int ret = ::truncate(filename_.c_str(), size);
  file_.open(filename_, std::ios::in | std::ios::out | std::ios::binary);
  return ret == 0 && file_.is_open();
}
} // namespace tiny_lsm