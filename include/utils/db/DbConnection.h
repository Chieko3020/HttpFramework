#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <mysql_driver.h>
#include <mysql_connection.h>
#include <cppconn/statement.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>

namespace db {

class DbConnection {
public:
    DbConnection(const std::string& host, const std::string& user,
                 const std::string& password, const std::string& database, int port);
    ~DbConnection();

    // 禁用拷贝构造和赋值
    DbConnection(const DbConnection&) = delete;
    DbConnection& operator=(const DbConnection&) = delete;

    // 连接到数据库
    bool connect();

    // 断开连接
    void disconnect();

    // 检查连接是否有效
    bool isConnected() const;

    // 执行查询
    std::shared_ptr<sql::ResultSet> executeQuery(const std::string& sql);
    int executeUpdate(const std::string& sql);
    std::shared_ptr<sql::PreparedStatement> prepareStatement(const std::string& sql);

    // 事务管理
    void beginTransaction();
    void commit();
    void rollback();
    bool isInTransaction() const { return inTransaction_; }

    // 设置自动提交
    void setAutoCommit(bool autoCommit);

    // 设置当前数据库
    void setSchema(const std::string& schema);

    // 获取最后插入的ID
    uint64_t getLastInsertId();

    // 获取错误信息
    std::string getLastError() const;

private:
    std::string host_;
    std::string user_;
    std::string password_;
    std::string database_;
    int port_;
    bool inTransaction_;

    sql::mysql::MySQL_Driver* driver_;
    std::unique_ptr<sql::Connection> connection_;

    void initDriver();
};

} // namespace db