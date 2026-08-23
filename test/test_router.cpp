#include "test_framework.h"
#include "Router.h"
#include <map>
#include <ostream>
#include <string>

// CHECK_EQ 打印需要 operator<<
std::ostream& operator<<(std::ostream& os, RouterResult r) {
    switch (r) {
        case RouterResult::kFound: return os << "kFound";
        case RouterResult::kNotFound: return os << "kNotFound";
        case RouterResult::kMethodNotAllowed: return os << "kMethodNotAllowed";
    }
    return os << "?";
}

// 标准路由表：精确 + 参数化 + 通配符 + 多方法
static Router makeRouter() {
    Router r;
    r.addRoute(HttpRequest::kGet, "/", [](const HttpRequest&, HttpResponse* resp,
                                         const std::map<std::string, std::string>&) {
        resp->setBody("root");
    });
    r.addRoute(HttpRequest::kGet, "/user/:id", [](const HttpRequest&, HttpResponse* resp,
                                                  const std::map<std::string, std::string>& params) {
        resp->setBody("user:" + params.at("id"));
    });
    r.addRoute(HttpRequest::kPost, "/user/:id", [](const HttpRequest&, HttpResponse* resp,
                                                   const std::map<std::string, std::string>& params) {
        resp->setBody("post-user:" + params.at("id"));
    });
    r.addRoute(HttpRequest::kGet, "/files/:dir/:name", [](const HttpRequest&, HttpResponse* resp,
                                                          const std::map<std::string, std::string>& params) {
        resp->setBody("file:" + params.at("dir") + "/" + params.at("name"));
    });
    r.addRoute(HttpRequest::kGet, "/static/*", [](const HttpRequest&, HttpResponse* resp,
                                                  const std::map<std::string, std::string>&) {
        resp->setBody("static");
    });
    return r;
}

static HttpRequest makeReq(HttpRequest::Method m, const std::string& path) {
    HttpRequest req;
    req.setMethod(m);
    req.setPath(path);
    return req;
}

// ---------- 精确匹配 ----------

TEST_CASE(ExactMatch) {
    Router r = makeRouter();
    HttpResponse resp;
    std::map<std::string, std::string> params;
    CHECK_EQ(r.route(makeReq(HttpRequest::kGet, "/"), &resp, &params), RouterResult::kFound);
    CHECK_EQ(resp.body(), "root");
}

// ---------- 参数化路由 ----------

TEST_CASE(ParamRoute) {
    Router r = makeRouter();
    HttpResponse resp;
    std::map<std::string, std::string> params;
    CHECK_EQ(r.route(makeReq(HttpRequest::kGet, "/user/42"), &resp, &params), RouterResult::kFound);
    CHECK_EQ(params.at("id"), "42");
    CHECK_EQ(resp.body(), "user:42");
}

TEST_CASE(MultiSegmentParam) {
    Router r = makeRouter();
    HttpResponse resp;
    std::map<std::string, std::string> params;
    CHECK_EQ(r.route(makeReq(HttpRequest::kGet, "/files/docs/report.txt"), &resp, &params),
             RouterResult::kFound);
    CHECK_EQ(params.at("dir"), "docs");
    CHECK_EQ(params.at("name"), "report.txt");
}

TEST_CASE(SamePatternDifferentMethods) {
    // GET /user/:id 与 POST /user/:id 分开注册：POST 请求必须命中 POST 的 entry
    Router r = makeRouter();
    HttpResponse resp;
    std::map<std::string, std::string> params;
    CHECK_EQ(r.route(makeReq(HttpRequest::kPost, "/user/7"), &resp, &params), RouterResult::kFound);
    CHECK_EQ(resp.body(), "post-user:7");
}

// ---------- 通配符 ----------

TEST_CASE(WildcardMatchesRemainingSegments) {
    Router r = makeRouter();
    HttpResponse resp;
    std::map<std::string, std::string> params;
    CHECK_EQ(r.route(makeReq(HttpRequest::kGet, "/static/css/app.css"), &resp, &params),
             RouterResult::kFound);
    CHECK_EQ(r.route(makeReq(HttpRequest::kGet, "/static"), &resp, &params),
             RouterResult::kNotFound);  // 贪婪匹配剩余"段"：0 段不匹配（pattern 更长）
}

// ---------- 方法语义 ----------

TEST_CASE(MethodNotAllowed) {
    Router r = makeRouter();
    HttpResponse resp;
    std::map<std::string, std::string> params;
    CHECK_EQ(r.route(makeReq(HttpRequest::kPost, "/"), &resp, &params),
             RouterResult::kMethodNotAllowed);   // 路径存在但方法不支持 → 405
}

TEST_CASE(HeadResolvesAsGet) {
    // HEAD 与 GET 语义等价（RFC 7231 §4.3.2）：按 GET 查找路由
    Router r = makeRouter();
    HttpResponse resp;
    std::map<std::string, std::string> params;
    CHECK_EQ(r.route(makeReq(HttpRequest::kHead, "/user/42"), &resp, &params), RouterResult::kFound);
}

TEST_CASE(ParamMethodMismatchFallsBack) {
    // 动态模式方法不匹配不能立即 405：继续找后面的 entry
    Router r;
    r.addRoute(HttpRequest::kGet, "/user/:id", [](const HttpRequest&, HttpResponse* resp,
                                                  const std::map<std::string, std::string>& params) {
        resp->setBody("get:" + params.at("id"));
    });
    r.addRoute(HttpRequest::kPost, "/user/:id", [](const HttpRequest&, HttpResponse* resp,
                                                   const std::map<std::string, std::string>& params) {
        resp->setBody("post:" + params.at("id"));
    });
    HttpResponse resp;
    std::map<std::string, std::string> params;
    CHECK_EQ(r.route(makeReq(HttpRequest::kPost, "/user/3"), &resp, &params), RouterResult::kFound);
    CHECK_EQ(resp.body(), "post:3");
}

// ---------- 未命中 ----------

TEST_CASE(NotFound) {
    Router r = makeRouter();
    HttpResponse resp;
    std::map<std::string, std::string> params;
    CHECK_EQ(r.route(makeReq(HttpRequest::kGet, "/nope"), &resp, &params), RouterResult::kNotFound);
}

// ---------- 优先级与边界 ----------

TEST_CASE(ExactBeatsParam) {
    // 精确匹配优先于动态模式（同一路径同时注册两种时）
    Router r;
    int exactHits = 0, paramHits = 0;
    r.addRoute(HttpRequest::kGet, "/x", [&](const HttpRequest&, HttpResponse*,
                                            const std::map<std::string, std::string>&) {
        ++exactHits;
    });
    r.addRoute(HttpRequest::kGet, "/:x", [&](const HttpRequest&, HttpResponse*,
                                             const std::map<std::string, std::string>&) {
        ++paramHits;
    });
    HttpResponse resp;
    std::map<std::string, std::string> params;
    CHECK_EQ(r.route(makeReq(HttpRequest::kGet, "/x"), &resp, &params), RouterResult::kFound);
    CHECK_EQ(exactHits, 1);
    CHECK_EQ(paramHits, 0);
    // 其他路径落到动态模式
    CHECK_EQ(r.route(makeReq(HttpRequest::kGet, "/y"), &resp, &params), RouterResult::kFound);
    CHECK_EQ(paramHits, 1);
}

TEST_CASE(LiteralParamLikePathDoesNotCrash) {
    // 请求 /user/%3Aid 解码后是 /user/:id，字面命中参数模式：params["id"]=":id"，
    // handler 里 params.at() 必须取到值而不能抛 out_of_range
    Router r = makeRouter();
    HttpResponse resp;
    std::map<std::string, std::string> params;
    CHECK_EQ(r.route(makeReq(HttpRequest::kGet, "/user/:id"), &resp, &params), RouterResult::kFound);
    CHECK_EQ(params.at("id"), ":id");
}

TEST_CASE(DoubleSlashNormalized) {
    // 连续斜杠被 splitPath 忽略：/a//b 等价 /a/b（HttpContext 解码后的一致性行为）
    Router r = makeRouter();
    HttpResponse resp;
    std::map<std::string, std::string> params;
    CHECK_EQ(r.route(makeReq(HttpRequest::kGet, "/files/docs//report.txt"), &resp, &params),
             RouterResult::kFound);
    CHECK_EQ(params.at("name"), "report.txt");
}
