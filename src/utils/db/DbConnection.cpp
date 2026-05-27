#include "utils/db/DbConnection.h"
#include <iostream>
#include <stdexcept>

namespace db {

DbConnection::DbConnection(const std::string& host, const std::string& user,
                           const std::string& password, const std::string& database, int port)
    : host_(host), user_(user), password_(password), database_(database), 
      port_(port), inTransaction_(false), driver_(nullptr) {
    initDriver();
}

DbConnection::~DbConnection() {
    disconnect();
}

void DbConnection::initDriver() {
    try {
        driver_ = sql::mysql::get_mysql_driver_instance();
    } catch (sql::SQLException& e) {
        std::cerr << "Failed to get MySQL driver: " << e.what() << std::endl;
        throw;
    }
}

bool DbConnection::connect() {
    try {
        if (connection_ && !connection_->isClosed()) {
            return true; // Already connected
        }

        std::string connectionString = "tcp://" + host_ + ":" + std::to_string(port_);
        connection_ = std::unique_ptr<sql::Connection>(
            driver_->connect(connectionString, user_, password_)
        );
        
        connection_->setSchema(database_);
        connection_->setAutoCommit(true);
        
        return true;
    } catch (sql::SQLException& e) {
        std::cerr << "MySQL connection failed: " << e.what() << std::endl;
        connection_.reset();
        return false;
    }
}

void DbConnection::disconnect() {
    if (connection_) {
        try {
            if (!connection_->isClosed()) {
                connection_->close();
            }
        } catch (sql::SQLException& e) {
            std::cerr << "Error closing connection: " << e.what() << std::endl;
        }
        connection_.reset();
    }
}

bool DbConnection::isConnected() const {
    return connection_ && !connection_->isClosed();
}

std::shared_ptr<sql::ResultSet> DbConnection::executeQuery(const std::string& sql) {
    if (!isConnected()) {
        std::cerr << "Database not connected" << std::endl;
        return nullptr;
    }

    try {
        std::unique_ptr<sql::Statement> stmt(connection_->createStatement());
        std::unique_ptr<sql::ResultSet> result(stmt->executeQuery(sql));
        
        // 将unique_ptr转换为shared_ptr
        return std::shared_ptr<sql::ResultSet>(result.release());
    } catch (sql::SQLException& e) {
        std::cerr << "Query failed: " << e.what() << std::endl;
        return nullptr;
    }
}

int DbConnection::executeUpdate(const std::string& sql) {
    if (!isConnected()) {
        std::cerr << "Database not connected" << std::endl;
        return -1;
    }

    try {
        std::unique_ptr<sql::Statement> stmt(connection_->createStatement());
        return stmt->executeUpdate(sql);
    } catch (sql::SQLException& e) {
        std::cerr << "Update failed: " << e.what() << std::endl;
        return -1;
    }
}

std::shared_ptr<sql::PreparedStatement> DbConnection::prepareStatement(const std::string& sql) {
    if (!isConnected()) {
        std::cerr << "Database not connected" << std::endl;
        return nullptr;
    }

    try {
        std::unique_ptr<sql::PreparedStatement> stmt(connection_->prepareStatement(sql));
        return std::shared_ptr<sql::PreparedStatement>(stmt.release());
    } catch (sql::SQLException& e) {
        std::cerr << "Prepare statement failed: " << e.what() << std::endl;
        return nullptr;
    }
}

void DbConnection::beginTransaction() {
    if (isConnected()) {
        try {
            connection_->setAutoCommit(false);
            inTransaction_ = true;
        } catch (sql::SQLException& e) {
            std::cerr << "Begin transaction failed: " << e.what() << std::endl;
        }
    }
}

void DbConnection::commit() {
    if (isConnected() && inTransaction_) {
        try {
            connection_->commit();
            connection_->setAutoCommit(true);
            inTransaction_ = false;
        } catch (sql::SQLException& e) {
            std::cerr << "Commit failed: " << e.what() << std::endl;
        }
    }
}

void DbConnection::rollback() {
    if (isConnected() && inTransaction_) {
        try {
            connection_->rollback();
            connection_->setAutoCommit(true);
            inTransaction_ = false;
        } catch (sql::SQLException& e) {
            std::cerr << "Rollback failed: " << e.what() << std::endl;
        }
    }
}

void DbConnection::setAutoCommit(bool autoCommit) {
    if (isConnected()) {
        try {
            connection_->setAutoCommit(autoCommit);
        } catch (sql::SQLException& e) {
            std::cerr << "Set auto commit failed: " << e.what() << std::endl;
        }
    }
}

void DbConnection::setSchema(const std::string& schema) {
    if (isConnected()) {
        try {
            connection_->setSchema(schema);
        } catch (sql::SQLException& e) {
            std::cerr << "Set schema failed: " << e.what() << std::endl;
        }
    }
}

uint64_t DbConnection::getLastInsertId() {
    if (isConnected()) {
        try {
            std::unique_ptr<sql::Statement> stmt(connection_->createStatement());
            std::unique_ptr<sql::ResultSet> result(stmt->executeQuery("SELECT LAST_INSERT_ID()"));
            if (result->next()) {
                return result->getUInt64(1);
            }
        } catch (sql::SQLException& e) {
            std::cerr << "Get last insert ID failed: " << e.what() << std::endl;
        }
    }
    return 0;
}

std::string DbConnection::getLastError() const {
    if (connection_) {
        try {
            const sql::SQLWarning* warning = connection_->getWarnings();
            if (warning) {
                return warning->getMessage();
            }
            return "No warnings";
        } catch (sql::SQLException& e) {
            return e.what();
        }
    }
    return "No connection";
}

} // namespace db