#pragma once

#include <exception>
#include <string>

namespace db {

class DbException : public std::exception {
public:
    explicit DbException(const std::string& message);
    explicit DbException(const char* message);
    
    virtual ~DbException() noexcept = default;
    
    virtual const char* what() const noexcept override;

private:
    std::string message_;
};

} // namespace db
