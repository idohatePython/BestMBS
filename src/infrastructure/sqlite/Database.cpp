#include "mbs/infrastructure/sqlite/Database.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <utility>

namespace mbs::infrastructure::sqlite {
namespace {

std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[noreturn]] void throw_database_error(sqlite3* database, const int fallback_code,
                                       const std::string_view context) {
    const int code = database == nullptr ? fallback_code : sqlite3_extended_errcode(database);
    const char* detail =
        database == nullptr ? sqlite3_errstr(fallback_code) : sqlite3_errmsg(database);
    throw SqliteError{code, std::string{context} + ": " + detail};
}

} // namespace

SqliteError::SqliteError(const int code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

int SqliteError::code() const noexcept { return code_; }

Connection::Connection(const std::filesystem::path& path, const bool enable_wal) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    const auto filename = path_utf8(path);
    const int result = sqlite3_open_v2(filename.c_str(), &database_,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (result != SQLITE_OK) {
        sqlite3* failed = database_;
        database_ = nullptr;
        const std::string detail =
            failed == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(failed);
        if (failed != nullptr) {
            sqlite3_close_v2(failed);
        }
        throw SqliteError{result, "open SQLite database: " + detail};
    }
    try {
        sqlite3_extended_result_codes(database_, 1);
        sqlite3_busy_timeout(database_, 10'000);
        execute("PRAGMA foreign_keys=ON");
        if (enable_wal) {
            execute("PRAGMA journal_mode=WAL");
        }
    } catch (...) {
        sqlite3_close_v2(database_);
        database_ = nullptr;
        throw;
    }
}

Connection::~Connection() {
    if (database_ != nullptr) {
        sqlite3_close_v2(database_);
    }
}

Connection::Connection(Connection&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)) {}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        if (database_ != nullptr) {
            sqlite3_close_v2(database_);
        }
        database_ = std::exchange(other.database_, nullptr);
    }
    return *this;
}

void Connection::execute(const std::string_view sql) {
    char* error_message = nullptr;
    const std::string owned_sql{sql};
    const int result = sqlite3_exec(database_, owned_sql.c_str(), nullptr, nullptr, &error_message);
    if (result != SQLITE_OK) {
        const std::string detail =
            error_message == nullptr ? sqlite3_errmsg(database_) : error_message;
        sqlite3_free(error_message);
        throw SqliteError{sqlite3_extended_errcode(database_),
                          "execute SQLite statement: " + detail};
    }
}

sqlite3* Connection::handle() const noexcept { return database_; }

int Connection::changes() const noexcept { return sqlite3_changes(database_); }

Statement::Statement(Connection& connection, const std::string_view sql) {
    const std::string owned_sql{sql};
    const int result = sqlite3_prepare_v2(connection.handle(), owned_sql.c_str(),
                                          static_cast<int>(owned_sql.size()), &statement_, nullptr);
    if (result != SQLITE_OK) {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
            statement_ = nullptr;
        }
        throw_database_error(connection.handle(), result, "prepare SQLite statement");
    }
}

Statement::~Statement() {
    if (statement_ != nullptr) {
        sqlite3_finalize(statement_);
    }
}

void Statement::check_bind(const int result) const {
    if (result != SQLITE_OK) {
        throw_database_error(sqlite3_db_handle(statement_), result, "bind SQLite parameter");
    }
}

void Statement::bind(const int index, const std::string_view value) {
    const char* text = value.empty() ? "" : value.data();
    check_bind(sqlite3_bind_text(statement_, index, text, static_cast<int>(value.size()),
                                 SQLITE_TRANSIENT));
}

void Statement::bind(const int index, const double value) {
    check_bind(sqlite3_bind_double(statement_, index, value));
}

void Statement::bind(const int index, const int value) {
    check_bind(sqlite3_bind_int(statement_, index, value));
}

void Statement::bind(const int index, const std::int64_t value) {
    check_bind(sqlite3_bind_int64(statement_, index, value));
}

void Statement::bind_null(const int index) { check_bind(sqlite3_bind_null(statement_, index)); }

void Statement::bind_optional(const int index, const std::optional<std::string>& value) {
    if (value.has_value()) {
        bind(index, *value);
    } else {
        bind_null(index);
    }
}

void Statement::bind_optional(const int index, const std::optional<std::uint64_t>& value) {
    if (value.has_value()) {
        if (*value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::overflow_error{"SQLite INTEGER cannot represent size_bytes"};
        }
        bind(index, static_cast<std::int64_t>(*value));
    } else {
        bind_null(index);
    }
}

bool Statement::step() {
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
        return true;
    }
    if (result == SQLITE_DONE) {
        return false;
    }
    throw_database_error(sqlite3_db_handle(statement_), result, "step SQLite statement");
}

void Statement::execute() {
    if (step()) {
        throw SqliteError{SQLITE_MISUSE, "SQLite command unexpectedly returned a row"};
    }
}

void Statement::reset() {
    const int reset_result = sqlite3_reset(statement_);
    const int clear_result = sqlite3_clear_bindings(statement_);
    if (reset_result != SQLITE_OK || clear_result != SQLITE_OK) {
        throw_database_error(sqlite3_db_handle(statement_),
                             reset_result != SQLITE_OK ? reset_result : clear_result,
                             "reset SQLite statement");
    }
}

int Statement::column_int(const int index) const { return sqlite3_column_int(statement_, index); }

std::int64_t Statement::column_int64(const int index) const {
    return sqlite3_column_int64(statement_, index);
}

double Statement::column_double(const int index) const {
    return sqlite3_column_double(statement_, index);
}

std::string Statement::column_text(const int index) const {
    const auto* text = sqlite3_column_text(statement_, index);
    const int bytes = sqlite3_column_bytes(statement_, index);
    if (text == nullptr) {
        return {};
    }
    return {reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)};
}

std::optional<std::string> Statement::column_optional_text(const int index) const {
    if (sqlite3_column_type(statement_, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return column_text(index);
}

std::optional<std::uint64_t> Statement::column_optional_uint64(const int index) const {
    if (sqlite3_column_type(statement_, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    const auto value = column_int64(index);
    if (value < 0) {
        throw SqliteError{SQLITE_CORRUPT, "negative value stored for unsigned SQLite field"};
    }
    return static_cast<std::uint64_t>(value);
}

Transaction::Transaction(Connection& connection) : connection_(connection) {
    connection_.execute("BEGIN IMMEDIATE");
}

Transaction::~Transaction() {
    if (active_) {
        try {
            connection_.execute("ROLLBACK");
        } catch (...) {
        }
    }
}

void Transaction::commit() {
    connection_.execute("COMMIT");
    active_ = false;
}

std::string now_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string generate_id(const std::string_view prefix) {
    static std::atomic<std::uint64_t> counter{};
    thread_local std::mt19937_64 generator{std::random_device{}()};
    const auto sequence = ++counter;
    std::ostringstream output;
    output << prefix << '-' << std::hex << std::setfill('0') << std::setw(16) << generator()
           << std::setw(16) << sequence;
    return output.str();
}

} // namespace mbs::infrastructure::sqlite
