// 微信读书官方 Skill/Gateway 客户端（main/weread_skill_client.cpp）纯函数的主机对拍。
// 只测三件事：body 拼装、搜索响应解析、推荐响应解析。网络路径不测（stubs.cpp 里
// net::request 永远失败，测例根本不会走到）。
//
//   tests/host/weread_skill/run.sh
//
// 退出码 0 = 全部通过。

#include "weread_skill_client.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_pass = 0;

static void ok(const char *name, bool cond) {
    if (cond) { ++g_pass; return; }
    ++g_fail;
    std::printf("FAIL: %s\n", name);
}

static void eqStr(const char *name, const std::string &got, const std::string &want) {
    if (got == want) { ++g_pass; return; }
    ++g_fail;
    std::printf("FAIL: %s\n  got : %s\n  want: %s\n", name, got.c_str(), want.c_str());
}

using P = WereadGwParam;

static void testBuildBody() {
    // 基本形状：api_name / skill_version 先来，params 按插入顺序摊平。
    eqStr("body: search",
          WereadSkillClient::buildGatewayBody(
              "/store/search",
              {P{"keyword", "三体", false}, P{"scope", "10", true},
               P{"maxIdx", "0", true}, P{"count", "12", true}}),
          "{\"api_name\":\"/store/search\",\"skill_version\":\"1.0.5\","
          "\"keyword\":\"三体\",\"scope\":10,\"maxIdx\":0,\"count\":12}");

    // 无参数：只剩两个固定字段。
    eqStr("body: no params",
          WereadSkillClient::buildGatewayBody("/shelf/sync", {}),
          "{\"api_name\":\"/shelf/sync\",\"skill_version\":\"1.0.5\"}");

    // 字符串里的引号 / 反斜杠 / 换行 / Tab / 控制字符都要转义。
    eqStr("body: escape",
          WereadSkillClient::buildGatewayBody(
              "/x", {P{"k", "a\"b\\c\nd\te", false}}),
          "{\"api_name\":\"/x\",\"skill_version\":\"1.0.5\",\"k\":\"a\\\"b\\\\c\\nd\\te\"}");

    eqStr("body: control byte",
          WereadSkillClient::buildGatewayBody("/x", {P{"k", std::string("a\x01""b"), false}}),
          "{\"api_name\":\"/x\",\"skill_version\":\"1.0.5\",\"k\":\"a\\u0001b\"}");

    // numeric 但值为空 → 退化成带引号的空串（不能让 JSON 出现裸 `:`）。
    eqStr("body: numeric empty",
          WereadSkillClient::buildGatewayBody("/x", {P{"n", "", true}}),
          "{\"api_name\":\"/x\",\"skill_version\":\"1.0.5\",\"n\":\"\"}");

    // UTF-8 高位字节原样透传（不能按 signed char 误判成控制字符）。
    eqStr("body: utf8 passthrough",
          WereadSkillClient::buildGatewayBody("/x", {P{"k", "微信读书", false}}),
          "{\"api_name\":\"/x\",\"skill_version\":\"1.0.5\",\"k\":\"微信读书\"}");
}

static void testParseSearch() {
    const char *json = R"JSON(
    {"results":[
      {"books":[
        {"bookInfo":{"bookId":"939309","title":"三体","author":"刘慈欣","category":"科幻",
                     "intro":"地球往事","newRating":890,"readingCount":12345},
         "reason":"因为你读过"},
        {"bookId":"111","title":"直接项","author":"某人","newRating":0,"readingCount":7}
      ]},
      {"books":[{"bookInfo":{"bookId":"222","title":"第二组"}}]},
      {"nope":true}
    ]})JSON";

    JsonValue root = JsonValue::parse(json);
    auto items = WereadSkillClient::parseSearch(root);
    ok("search: count == 3", items.size() == 3);
    if (items.size() == 3) {
        eqStr("search[0].bookId", items[0].bookId, "939309");
        eqStr("search[0].title", items[0].title, "三体");
        eqStr("search[0].author", items[0].author, "刘慈欣");
        eqStr("search[0].category", items[0].category, "科幻");
        eqStr("search[0].intro", items[0].intro, "地球往事");
        ok("search[0].rating == 89", items[0].rating == 89.0f);
        ok("search[0].readingCount == 12345", items[0].readingCount == 12345);
        // 直接是 bookInfo（无外层包裹）也要认。
        eqStr("search[1].bookId (unwrapped)", items[1].bookId, "111");
        eqStr("search[1].title", items[1].title, "直接项");
        eqStr("search[2].bookId", items[2].bookId, "222");
        eqStr("search[2].title", items[2].title, "第二组");
    }

    // 缺 bookId 的条目要丢掉；根上没有 results 要返回空。
    auto empty = WereadSkillClient::parseSearch(
        JsonValue::parse(R"({"results":[{"books":[{"title":"没有id"}]}]})"));
    ok("search: drop empty bookId", empty.empty());

    auto none = WereadSkillClient::parseSearch(JsonValue::parse(R"({"books":[]})"));
    ok("search: no results key", none.empty());
}

static void testParseRecommend() {
    const char *json = R"JSON(
    {"books":[
      {"bookId":"222","title":"推荐一","author":"作者甲","category":"文学",
       "intro":"简介","reason":"热门","newRating":770,"readingCount":999},
      {"title":"没有id"},
      {"bookId":"333","title":"推荐二","newRating":1000}
    ]})JSON";

    JsonValue root = JsonValue::parse(json);
    auto items = WereadSkillClient::parseRecommend(root);
    ok("recommend: count == 2", items.size() == 2);
    if (items.size() == 2) {
        eqStr("rec[0].bookId", items[0].bookId, "222");
        eqStr("rec[0].reason", items[0].reason, "热门");
        ok("rec[0].rating == 77", items[0].rating == 77.0f);
        ok("rec[0].readingCount == 999", items[0].readingCount == 999);
        eqStr("rec[1].bookId", items[1].bookId, "333");
        ok("rec[1].rating == 100", items[1].rating == 100.0f);
    }

    // 根上不是对象 / 没有 books → 空。
    auto none = WereadSkillClient::parseRecommend(JsonValue::parse("[1,2,3]"));
    ok("recommend: non-object root", none.empty());
}

int main() {
    testBuildBody();
    testParseSearch();
    testParseRecommend();

    std::printf("weread_skill: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
