// src/filename.h —— WAL 文件命名与编号（docs/m2-design.md §3）
//
// 命名规则：`%06u.log`（固定宽度 ⇒ 字典序 == 数值序；A12 仍要求按数值解析，
// 因为 M3 会往里塞其它后缀的文件，不能靠字符串序偷懒）。
#ifndef LSM_FILENAME_H_
#define LSM_FILENAME_H_

#include <cstdint>
#include <string>

namespace lsm {

constexpr const char* kLogFileSuffix = ".log";

std::string MakeFileName(const std::string& dbname, const std::string& suffix);
std::string LogFileName(const std::string& dbname, uint64_t number);
bool ParseLogFileName(const std::string& fname, uint64_t* number);   // 只认 "%06u.log"
std::string LockFileName(const std::string& dbname);

}  // namespace lsm

#endif  // LSM_FILENAME_H_
