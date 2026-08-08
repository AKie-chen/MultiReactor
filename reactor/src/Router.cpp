#include "Router.h"

static std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> segments;
    size_t start = 0, end = 0;
    while ((end = path.find('/', start)) != std::string::npos) {
        if (end != start) { // 忽略连续的 '/'
            segments.push_back(path.substr(start, end - start));
        }
        start = end + 1;
    }
    if (start < path.size()) {
        segments.push_back(path.substr(start));
    }
    return segments;
}

static bool matchPattern(const std::string& pattern, const std::string& path,
                         std::map<std::string, std::string>* param)
{
    auto patternSegments = splitPath(pattern);
    auto pathSegments = splitPath(path);

    // pattern比path长不必匹配
    if (patternSegments.size() > pathSegments.size()) {
        return false;
    }

    for (size_t i = 0; i < patternSegments.size(); ++i) {
        const std::string& pSeg = patternSegments[i];
        const std::string& pathSeg = pathSegments[i];

        if (!pSeg.empty() && pSeg[0] == ':') { // 动态参数
            if (param) {
                (*param)[pSeg.substr(1)] = pathSeg; // 提取参数值
            }
        } else if (!pSeg.empty() && pSeg[0] == '*') { // 通配符
            // 必须放在相等比较之前：`*` 与任意路径段都不等，
            // 放后面会被 pSeg != pathSeg 提前 return false（原实现 bug）
            return true; // 贪婪匹配剩余所有段（含 0 段以上）
        } else if (pSeg != pathSeg) { // 静态部分不匹配
            return false;
        }
    }
    return true;
}

// 注册路由：精确匹配 method + path
void Router::addRoute(HttpRequest::Method method, const std::string& path, Handler handler) {
    routes_[{method, path}] = handler;
    pathToMethods_[path].insert(method);
    if(path.find(':') != std::string::npos || path.find('*') != std::string::npos) { // 如果路径中包含参数标记
        paramRoutes_.emplace_back(path, std::map<HttpRequest::Method, Handler>{{method, handler}});
    }
}

// 查找并执行
RouterResult Router::route(const HttpRequest& req, HttpResponse* resp, std::map<std::string, std::string>* params) {

    // 先尝试精确匹配
    auto it = routes_.find({req.method(), req.path()});
    if (it != routes_.end()) {
        it->second(req, resp, *params); // 执行处理函数
        return RouterResult::kFound;
    }

    // 如果没有精确匹配，检查是否有其他方法支持该路径
    auto pathIt = pathToMethods_.find(req.path());
    if (pathIt != pathToMethods_.end()) {
        // 该路径存在，但方法不支持
        if(pathIt->second.find(req.method()) == pathIt->second.end()) {
            return RouterResult::kMethodNotAllowed;
        }
        // 如果方法支持，但没有找到对应的处理函数，返回 404
        return RouterResult::kNotFound;
    }

    // 如果路径不存在，检查动态路由
    // 注意：同一模式注册多个方法时 paramRoutes_ 会有多个 entry（addRoute 每次追加一个）。
    // 某个 entry 模式匹配但方法不匹配时不能立即返回 405——后面的 entry 可能有匹配的方法
    // （如 GET /user/:id 和 POST /user/:id 分开注册时，POST 请求必须命中第二个 entry）。
    // 先全部遍历找方法匹配的，找不到再回退 405。
    bool patternMatchedButMethodNot = false;
    for (const auto& paramRoute : paramRoutes_) {
        if (matchPattern(paramRoute.first, req.path(), params)) {
            patternMatchedButMethodNot = true;
            auto methodIt = paramRoute.second.find(req.method());
            if (methodIt != paramRoute.second.end()) {
                methodIt->second(req, resp, *params);  // handler 第三参就是 :id 提取出的值
                return RouterResult::kFound;
            }
            // 方法不匹配：记住，继续找下一个 entry
        }
    }

    if (patternMatchedButMethodNot) return RouterResult::kMethodNotAllowed;
    return RouterResult::kNotFound;
}

bool Router::RouteKey::operator<(const RouteKey& other) const {
    if (method != other.method) return method < other.method;
    return path < other.path;
}
