#pragma once

#include "mmap_file.h"
#include "std_file.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tiny_lsm {

class Cursor;

class FileObj {
private:
  std::unique_ptr<StdFile> m_file;

public:
  FileObj();
  ~FileObj();

  // 禁用拷贝
  FileObj(const FileObj &) = delete;
  FileObj &operator=(const FileObj &) = delete;

  // 实现移动语义
  FileObj(FileObj &&other) noexcept;

  FileObj &operator=(FileObj &&other) noexcept;

  // 文件大小
  size_t size() const;

  // 删除文件
  void del_file();

  bool truncate(size_t offset);

  // 创建文件对象, 并写入到磁盘
  static FileObj create_and_write(const std::string &path,
                                  std::vector<uint8_t> buf);

  // 打开文件对象
  static FileObj open(const std::string &path, bool create);

  // 读取并返回切片
  std::vector<uint8_t> read_to_slice(size_t offset, size_t length);

  // 读取 uint8_t
  uint8_t read_uint8(size_t offset);

  // 读取 uint16_t
  uint16_t read_uint16(size_t offset);

  // 读取 uint32_t
  uint32_t read_uint32(size_t offset);

  // 读取 uint64_t
  uint64_t read_uint64(size_t offset);

  // 写入到文件
  bool write(size_t offset, std::vector<uint8_t> &buf);

  bool write_int(size_t offset, int value);
  bool write_uint8(size_t offset, uint8_t value);
  bool write_uint16(size_t offset, uint16_t value);
  bool write_uint32(size_t offset, uint32_t value);
  bool write_uint64(size_t offset, uint64_t value);

  // 追加写入到文件
  bool append(std::vector<uint8_t> &buf);

  bool append_int(int value);
  bool append_uint8(uint8_t value);
  bool append_uint16(uint16_t value);
  bool append_uint32(uint32_t value);
  bool append_uint64(uint64_t value);

  bool sync();

  Cursor get_cursor(FileObj &file_obj);
};
} // namespace tiny_lsm