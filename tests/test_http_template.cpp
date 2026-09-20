// test_http_template.cpp — TemplateLoader 模板加载单元测试
// 验证：{{variable}} 替换正确、文件缺失降级到 fallback、
//       多变量替换、无变量时不破坏内容

#include "utils/TemplateLoader.h"
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <cstdio>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while(0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)
// SKIP：报告为"跳过"（只计入 g_testsSkipped，并置 g_skipFlag 让 run() 不要把它
//       算成通过；返回值是 false，因此退出码语义不变 —— 跳过不算失败）
#define SKIP(msg) do { std::cout << "跳过 (" << msg << ")" << std::endl; ++g_testsSkipped; g_skipFlag = true; return false; } while(0)

static int g_testsPassed = 0;
static int g_testsFailed = 0;
static int g_testsSkipped = 0;
// SKIP 用：让 run() 知道"这次返回 false 是跳过，不是失败"
[[maybe_unused]] static bool g_skipFlag = false;

// 辅助：shell 命令包装
static inline void sh(const char* cmd) { int r = system(cmd); (void)r; }
static inline void sh(const std::string& cmd) { int r = system(cmd.c_str()); (void)r; }

static bool test_load_builtin_template() {
    TEST("loadTemplate 成功加载内置模板文件");
    std::string content = utils::TemplateLoader::loadTemplate("index.html");
    CHECK(!content.empty(), "index.html 应可加载");
    CHECK(content.find("<!DOCTYPE html>") != std::string::npos
          || content.find("<html") != std::string::npos
          || content.find("<body") != std::string::npos,
          "模板应包含 HTML 标签");

    PASS();
    return true;
}

static bool test_load_with_variables() {
    TEST("loadTemplate 替换 {{变量}} 占位符");
    std::string tmplContent = "<div>用户: {{username}}, 年龄: {{age}}</div>";
    {
        std::ofstream f("/tmp/test_user.tmpl");
        f << tmplContent;
    }

    sh("mkdir -p templates");
    sh("cp /tmp/test_user.tmpl templates/test_user.tmpl");

    std::map<std::string, std::string> vars;
    vars["username"] = "张三";
    vars["age"] = "30";

    std::string result = utils::TemplateLoader::loadTemplate("test_user.tmpl", vars);

    CHECK(result.find("张三") != std::string::npos, "username 应替换为 张三");
    CHECK(result.find("30") != std::string::npos, "age 应替换为 30");
    CHECK(result.find("{{username}}") == std::string::npos, "占位符 {{username}} 应消失");
    CHECK(result.find("{{age}}") == std::string::npos, "占位符 {{age}} 应消失");

    std::remove("templates/test_user.tmpl");
    std::remove("/tmp/test_user.tmpl");

    PASS();
    return true;
}

static bool test_fallback_on_missing() {
    TEST("缺失模板返回 fallback HTML");
    std::string result = utils::TemplateLoader::loadTemplate("nonexistent_xyz123.tmpl");

    CHECK(!result.empty(), "fallback 不应为空");
    CHECK(result.find("Template Not Found") != std::string::npos
          || result.find("模板文件未找到") != std::string::npos,
          "fallback 应提示模板未找到");

    PASS();
    return true;
}

// 原实现直接加载**真实存在**的 templates/index.html（TEMPLATES_DIR 是编译期
// 绝对路径，从任何目录运行都能找到），因此根本没覆盖 fallback 分支 ——
// 断言只校验"包含 HTML 结构"，把 fallback 删掉也能通过。
// 这里把 index.html 临时移开，强制走 fallback，并断言 fallback 特有文案；
// 最后无论成败都恢复原文件。
static bool test_index_fallback() {
    TEST("index.html 缺失时返回 index 专用 fallback 页面");
    const std::string tmplDir = std::string(TEMPLATES_DIR);
    const std::string indexPath = tmplDir + "index.html";
    const std::string backupPath = tmplDir + "index.html.bak-test";

    auto restore = [&]() {
        sh("mv -f '" + backupPath + "' '" + indexPath + "' 2>/dev/null || true");
    };

    if (!std::ifstream(indexPath).good()) {
        SKIP(std::string("模板目录里没有 index.html（") + indexPath + "），无法做缺失对照");
    }

    sh("mv -f '" + indexPath + "' '" + backupPath + "'");
    if (std::ifstream(indexPath).good()) {
        restore();
        FAIL("无法临时移开 " + indexPath + "（权限？）");
    }

    std::string result = utils::TemplateLoader::loadTemplate("index.html");
    std::cout << "(fallback长度=" << result.size() << ") ";
    const bool fallbackText = result.find("Template Not Found") != std::string::npos ||
                              result.find("模板文件未找到") != std::string::npos;
    restore();

    CHECK(!result.empty(), "fallback 不应为空");
    CHECK(fallbackText,
          "缺失 index.html 时必须返回带 'Template Not Found'/'模板文件未找到' 的 fallback 页"
          "（说明确实走了 fallback 分支）, 实际前 80 字: " + result.substr(0, 80));
    // 反向对照：恢复后必须能加载真实模板（否则"返回 fallback"可能只是因为路径写错了）
    const std::string realResult = utils::TemplateLoader::loadTemplate("index.html");
    CHECK(realResult.find("Template Not Found") == std::string::npos,
          "恢复 index.html 后不应再返回 fallback");

    PASS();
    return true;
}

static bool test_no_variables_no_change() {
    TEST("空变量映射时模板内容不变");
    std::string tmplContent = "<html><body>静态内容</body></html>";
    {
        std::ofstream f("/tmp/test_static.tmpl");
        f << tmplContent;
    }

    sh("mkdir -p templates");
    sh("cp /tmp/test_static.tmpl templates/test_static.tmpl");

    std::map<std::string, std::string> emptyVars;
    std::string result = utils::TemplateLoader::loadTemplate("test_static.tmpl", emptyVars);

    CHECK(result == tmplContent, "无变量时内容应完全不变");

    std::remove("templates/test_static.tmpl");
    std::remove("/tmp/test_static.tmpl");

    PASS();
    return true;
}

int main() {
    std::cout << "=== test_http_template ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        g_skipFlag = false;   // fn() 里的 SKIP 会置位
        if (fn()) { if (!g_skipFlag) ++g_testsPassed; }
        else if (!g_skipFlag) { ++g_testsFailed; }
    };

    run(test_load_builtin_template,   "加载内置模板");
    run(test_load_with_variables,     "变量替换");
    run(test_fallback_on_missing,     "缺失模板降级");
    run(test_index_fallback,          "index fallback");
    run(test_no_variables_no_change,  "无变量不变");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败, "
              << g_testsSkipped << " 跳过（总用例 "
              << (g_testsPassed + g_testsFailed + g_testsSkipped) << "）" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
