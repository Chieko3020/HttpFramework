#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>

namespace utils {

class TemplateLoader {
public:
    static std::string loadTemplate(const std::string& templateName) {
        // 1) 编译期注入的绝对路径（优先）
#ifdef TEMPLATES_DIR
        {
            std::string path = std::string(TEMPLATES_DIR) + templateName;
            std::ifstream file(path);
            if (file.is_open()) {
                std::stringstream buffer;
                buffer << file.rdbuf();
                return buffer.str();
            }
        }
#endif
        // 2) 相对路径回退（兼容从 build/ 目录启动）
        {
            std::string path = "templates/" + templateName;
            std::ifstream file(path);
            if (file.is_open()) {
                std::stringstream buffer;
                buffer << file.rdbuf();
                return buffer.str();
            }
        }
        // 3) 都没找到，返回 fallback 页面
        std::cerr << "[WARN][模板]：无法打开模板文件: " << templateName << std::endl;
        return generateFallbackTemplate(templateName);
    }

    static std::string loadTemplate(const std::string& templateName, const std::map<std::string, std::string>& variables) {
        std::string content = loadTemplate(templateName);
        return replaceVariables(content, variables);
    }

private:
    static std::string replaceVariables(const std::string& content, const std::map<std::string, std::string>& variables) {
        std::string result = content;
        
        for (const auto& pair : variables) {
            std::string placeholder = "{{" + pair.first + "}}";
            size_t pos = 0;
            while ((pos = result.find(placeholder, pos)) != std::string::npos) {
                result.replace(pos, placeholder.length(), pair.second);
                pos += pair.second.length();
            }
        }
        
        return result;
    }
    
private:
    static std::string generateFallbackTemplate(const std::string& templateName) {
        if (templateName == "index.html") {
            return R"(
<!DOCTYPE html>
<html>
<head>
    <title>HttpFramework - Template Not Found</title>
</head>
<body>
    <h1>HttpFramework 综合测试服务器</h1>
    <p>模板文件未找到，请检查 templates/index.html 文件</p>
    <p><a href="/api/status">API状态</a> | <a href="/db/test">数据库测试</a></p>
</body>
</html>
            )";
        }
        
        return R"(
<!DOCTYPE html>
<html>
<head>
    <title>Template Not Found</title>
</head>
<body>
    <h1>模板文件未找到</h1>
    <p>请检查模板文件: )" + templateName + R"(</p>
    <p><a href="/">返回首页</a></p>
</body>
</html>
        )";
    }
};

} // namespace utils
