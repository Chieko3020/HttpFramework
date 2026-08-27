#include "utils/db/DbException.h"

namespace db {

DbException::DbException(const std::string& message) : message_(message) {
}

DbException::DbException(const char* message) : message_(message) {
}

const char* DbException::what() const noexcept {
    return message_.c_str();
}

} // namespace db
