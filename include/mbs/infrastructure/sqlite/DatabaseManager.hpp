#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace mbs::infrastructure::sqlite {

inline constexpr int current_schema_version = 2;

struct DatabaseStatus final {
    std::filesystem::path path;
    int schema_version{};
    std::string integrity;
    std::string journal_mode;
    std::map<std::string, int, std::less<>> table_counts;
};

class DatabaseManager final {
  public:
    explicit DatabaseManager(std::filesystem::path path, std::string project_id = "default");

    [[nodiscard]] bool ensure();
    [[nodiscard]] DatabaseStatus status();
    void backup_to(const std::filesystem::path& destination);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] const std::string& project_id() const noexcept;

  private:
    std::filesystem::path path_;
    std::string project_id_;
};

} // namespace mbs::infrastructure::sqlite
