#include "HttpFramework.h"
#include "utils/TemplateLoader.h"

int main() {
    http::App app;

    app.enableLogging()
       .enableSession()
       .enableMemoryPool();

    app.get("/", [](auto& req, auto& res) {
        (void)req;
        res.setHtml(utils::TemplateLoader::loadTemplate("index.html"));
    });

    app.get("/api/hello", [](auto& req, auto& res) {
        (void)req;
        res.setJson(R"({"message": "Hello from HttpFramework!"})");
    });

    app.get("/users/:id", [](auto& req, auto& res) {
        res.setJson(R"({"user_id": ")" + req.getParam("id") + R"("})");
    });

    app.get("/session", [&app](auto& req, auto& res) {
        auto s = app.sessionManager()->getSession(req.getUserData("session"));
        int count = 1;
        if (s && s->has("visits")) count = std::stoi(s->get("visits")) + 1;
        if (s) s->set("visits", std::to_string(count));
        res.setJson(R"({"visits": )" + std::to_string(count) + "}");
    });

    app.notFound([](auto& req, auto& res) {
        (void)req;
        res.setStatus(http::HttpStatus::NOT_FOUND);
        res.setJson(R"({"error": "Not Found"})");
    });

    app.start(8080, 4);
}
