// src/filename.h —— WAL / MANIFEST / CURRENT 文件命名与编号（docs/m2-design.md §3、docs/m4-design.md §3.1）
//
// 命名规则：`%06u.log`（固定宽度 ⇒ 字典序 == 数值序；A12 仍要求按数值解析，
// 因为 M3 会往里塞其它后缀的文件，不能靠字符串序偷懒）。
// M4 起追加 MANIFEST-<n> / MANIFEST-<n>.tmp / CURRENT / CURRENT.tmp。
#ifndef LSM_FILENAME_H_
#define LSM_FILENAME_H_

#include <cstdint>
#include <string>

namespace lsm {

constexpr const char* kLogFileSuffix = ".log";
constexpr const char* kManifestFilePrefix = "MANIFEST-";

std::string MakeFileName(const std::string& dbname, const std::string& suffix);
std::string LogFileName(const std::string& dbname, uint64_t number);
bool ParseLogFileName(const std::string& fname, uint64_t* number);   // 只认 "%06u.log"
std::string LockFileName(const std::string& dbname);

// ---- M4 追加（docs/m4-design.md §3.1 / docs/protocol.md §11.1）----
std::string ManifestFileName(const std::string& dbname, uint64_t number);      // MANIFEST-%06u
std::string ManifestTempFileName(const std::string& dbname, uint64_t number);  // MANIFEST-%06u.tmp
std::string CurrentFileName(const std::string& dbname);                        // CURRENT
std::string CurrentTempFileName(const std::string& dbname);                    // CURRENT.tmp
// 解析 `MANIFEST-<n>`；**必须拒绝** `MANIFEST-<n>.tmp`（精确后缀，禁止前缀匹配）。
bool ParseManifestFileName(const std::string& fname, uint64_t* number);
bool ParseManifestTempFileName(const std::string& fname, uint64_t* number);
// CURRENT 的内容必须是 `^[0-9]{1,20}\n$`（纯十进制编号 + 恰好一个换行，§3.1）。
bool ParseCurrentContents(const std::string& contents, uint64_t* number);

}  // namespace lsm

#endif  // LSM_FILENAME_H_
