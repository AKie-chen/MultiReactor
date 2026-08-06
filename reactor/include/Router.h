#pragma once
#include <functional>
#include <unordered_map>
#include <set>
#include <vector>
#include "HttpRequest.h"
#include "HttpResponse.h"

enum class RouterResult { kFound, kNotFound, kMethodNotAllowed };

class Router {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse*,
                                    const std::map<std::string, std::string>&)>;

    Router() = default;
    ~Router() = default;

    // 注册路由：精确匹配 method + path
    void addRoute(HttpRequest::Method method, const std::string& path, Handler handler);

    // 查找并执行，返回 true 表示匹配到了
    RouterResult route(const HttpRequest& req, HttpResponse* resp, std::map<std::string, std::string>* params);

private:
    struct RouteKey { // 存储 method 和 path
        HttpRequest::Method method;
        std::string path;
        bool operator<(const RouteKey& other) const;  // 用于 map key
        bool operator==(const RouteKey& other) const; // 用于 unordered_map key
    };
    std::map<RouteKey, Handler> routes_;  // 使用 std::map 以便按 method 和 path 排序
    std::unordered_map<std::string, std::set<HttpRequest::Method>> pathToMethods_;  // 用于快速查找路径对应的支持方法
    std::vector<std::pair<std::string,std::map<HttpRequest::Method, Handler>>> paramRoutes_; // 存储动态路由，支持路径参数

};
