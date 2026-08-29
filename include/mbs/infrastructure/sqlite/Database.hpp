#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mbs::infrastructure::sqlite {

class SqliteError final : public std::runtime_error {
  public:
    SqliteError(int code, std::string message);
    [[nodiscard]] int code() const noexcept;

  private:
    int code_;
};

class Connection final {
  public:
    explicit Connection(const std::filesystem::path& path, bool enable_wal = true);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    void execute(std::string_view sql);
    [[nodiscard]] sqlite3* handle() const noexcept;
    [[nodiscard]] int changes() const noexcept;

  private:
    sqlite3* database_{};
};

class Statement final {
  public:
    Statement(Connection& connection, std::string_view sql);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, std::string_view value);
    void bind(int index, double value);
    void bind(int index, int value);
    void bind(int index, std::int64_t value);
    void bind_null(int index);
    void bind_optional(int index, const std::optional<std::string>& value);
    void bind_optional(int index, const std::optional<std::uint64_t>& value);

    [[nodiscard]] bool step();
    void execute();
    void reset();

    [[nodiscard]] int column_int(int index) const;
    [[nodiscard]] std::int64_t column_int64(int index) const;
    [[nodiscard]] double column_double(int index) const;
    [[nodiscard]] std::string column_text(int index) const;
    [[nodiscard]] std::optional<std::string> column_optional_text(int index) const;
    [[nodiscard]] std::optional<std::uint64_t> column_optional_uint64(int index) const;

  private:
    void check_bind(int result) const;
    sqlite3_stmt* statement_{};
};

class Transaction final {
  public:
    explicit Transaction(Connection& connection);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit();

  private:
    Connection& connection_;
    bool active_{true};
};

[[nodiscard]] std::string now_timestamp();
[[nodiscard]] std::string generate_id(std::string_view prefix);

} // namespace mbs::infrastructure::sqlite
