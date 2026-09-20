#pragma once

#include <functional>
#include <memory>

namespace http {

// 前向声明
class HttpRequest;
class HttpResponse;

// 任务类型定义
enum class TaskType {
    HTTP_REQUEST,    // HTTP请求处理任务
    CONNECTION_CLOSE, // 连接关闭任务
    CUSTOM          // 自定义任务
};

// 基础任务接口
class Task {
public:
    virtual ~Task() = default;
    virtual void execute() = 0;
    virtual TaskType getType() const = 0;
};

// HTTP请求处理任务
class HttpRequestTask : public Task {
public:
    HttpRequestTask(int clientFd, 
                   std::shared_ptr<HttpRequest> request,
                   std::shared_ptr<HttpResponse> response,
                   std::function<void(int, std::shared_ptr<HttpRequest>, std::shared_ptr<HttpResponse>)> handler)
        : clientFd_(clientFd), request_(request), response_(response), handler_(handler) {}

    void execute() override {
        if (handler_) {
            handler_(clientFd_, request_, response_);
        }
    }

    TaskType getType() const override {
        return TaskType::HTTP_REQUEST;
    }

    int getClientFd() const { return clientFd_; }
    std::shared_ptr<HttpRequest> getRequest() const { return request_; }
    std::shared_ptr<HttpResponse> getResponse() const { return response_; }

private:
    int clientFd_;
    std::shared_ptr<HttpRequest> request_;
    std::shared_ptr<HttpResponse> response_;
    std::function<void(int, std::shared_ptr<HttpRequest>, std::shared_ptr<HttpResponse>)> handler_;
};

// 连接关闭任务
class ConnectionCloseTask : public Task {
public:
    ConnectionCloseTask(int clientFd, std::function<void(int)> closeHandler)
        : clientFd_(clientFd), closeHandler_(closeHandler) {}

    void execute() override {
        if (closeHandler_) {
            closeHandler_(clientFd_);
        }
    }

    TaskType getType() const override {
        return TaskType::CONNECTION_CLOSE;
    }

    int getClientFd() const { return clientFd_; }

private:
    int clientFd_;
    std::function<void(int)> closeHandler_;
};

// 自定义任务
class CustomTask : public Task {
public:
    CustomTask(std::function<void()> func) : func_(func) {}

    void execute() override {
        if (func_) {
            func_();
        }
    }

    TaskType getType() const override {
        return TaskType::CUSTOM;
    }

private:
    std::function<void()> func_;
};

} // namespace http
