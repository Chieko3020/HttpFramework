#include "router/Router.h"
#include "router/RouterHandler.h"
#include <iostream>

namespace router {

Router::Router() : handler_(std::make_unique<RouterHandler>()) {
}

void Router::get(const std::string& path, Handler handler) {
    addRouteInternal("GET", path, handler);
}

void Router::post(const std::string& path, Handler handler) {
    addRouteInternal("POST", path, handler);
}

void Router::put(const std::string& path, Handler handler) {
    addRouteInternal("PUT", path, handler);
}

void Router::del(const std::string& path, Handler handler) {
    addRouteInternal("DELETE", path, handler);
}

void Router::patch(const std::string& path, Handler handler) {
    addRouteInternal("PATCH", path, handler);
}

void Router::head(const std::string& path, Handler handler) {
    addRouteInternal("HEAD", path, handler);
}

void Router::options(const std::string& path, Handler handler) {
    addRouteInternal("OPTIONS", path, handler);
}

void Router::addRoute(const std::string& method, const std::string& path, Handler handler) {
    addRouteInternal(method, path, handler);
}

void Router::use(Middleware middleware) {
    handler_->addMiddleware("", middleware);
}

void Router::use(const std::string& path, Middleware middleware) {
    handler_->addMiddleware(path, middleware);
}

bool Router::handleRequest(const http::HttpRequest& request, http::HttpResponse& response) {
    return handler_->handleRequest(request, response);
}

void Router::setNotFoundHandler(Handler handler) {
    handler_->setNotFoundHandler(handler);
}

void Router::setErrorHandler(std::function<void(const std::exception&, const http::HttpRequest&, http::HttpResponse&)> handler) {
    handler_->setErrorHandler(handler);
}

void Router::addRouteInternal(const std::string& method, const std::string& path, Handler handler) {
    handler_->addRoute(method, path, handler);
}

} // namespace router
