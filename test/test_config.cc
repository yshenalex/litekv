/**
 * @file test_config.cc
 * @brief 配置解析单测：loadConfigFile 的语法/键值/错误语义 + validateConfig。
 * 纯解析逻辑，不碰网络、不起服务（文件写在当前目录，用完即删）。
 * 跑法：ctest --test-dir build -R config
 */

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#include "Config.hpp"
#include "test_util.hpp"

namespace
{

// 把配置文本写进当前目录的临时文件，返回路径（调用方负责 remove）
std::string writeTemp(const std::string &name, const std::string &content)
{
    const std::string path =
        "test_config_" + std::to_string(::getpid()) + "_" + name + ".conf";
    std::ofstream out(path);
    out << content;
    return path;
}

void testFullFile()
{
    const std::string path = writeTemp("full",
        "# 顶部注释\n"
        "\n"
        "Ip 127.0.0.1   # 行内注释\n"
        "PORT 9000\n"
        "SubReactors 4\n"
        "Shards 32\n"
        "MaxEntriesPerShard 4096\n"
        "AppendOnly yes\n"
        "AofPath /data/kv/appendonly.aof\n"
        "Dir /data/kv\n"
        "InputHighWater 64mb\n"
        "OutputHighWater 1k\n"
        "StrictLru on\n");
    Config cfg;
    std::string err;
    CHECK(loadConfigFile(path, cfg, &err));
    CHECK(cfg.ip == "127.0.0.1");
    CHECK_EQ(cfg.port, 9000);
    CHECK_EQ(cfg.subReactors, 4);
    CHECK_EQ(cfg.shards, 32);
    CHECK_EQ(cfg.maxEntriesPerShard, 4096);
    CHECK(cfg.appendOnly);
    CHECK(cfg.aofPath == "/data/kv/appendonly.aof");
    CHECK(cfg.dir == "/data/kv");
    CHECK_EQ(cfg.inputHighWater, 64ULL << 20);
    CHECK_EQ(cfg.outputHighWater, 1024ULL);
    CHECK(cfg.strictLru);
    std::remove(path.c_str());
}

void testCommentsAndDefaults()
{
    // 只有注释/空行：cfg 完全不动
    const std::string path = writeTemp("comment",
        "# port 9000\n"
        "   # 前导空白的注释\n"
        "\n"
        "\t\n");
    Config cfg;
    const Config before = cfg;
    std::string err;
    CHECK(loadConfigFile(path, cfg, &err));
    CHECK_EQ(cfg.port, before.port);
    CHECK_EQ(cfg.subReactors, before.subReactors);
    CHECK(cfg.ip == before.ip);
    CHECK(cfg.dir == before.dir);
    std::remove(path.c_str());
}

void testBoolVariants()
{
    for (const char *yes : {"yes", "true", "on", "1", "YES", "True"}) {
        const std::string path = writeTemp("by", std::string("appendonly ") + yes + "\n");
        Config cfg;
        cfg.appendOnly = false;
        std::string err;
        CHECK(loadConfigFile(path, cfg, &err));
        CHECK(cfg.appendOnly);
        std::remove(path.c_str());
    }
    for (const char *no : {"no", "false", "off", "0", "NO", "Off"}) {
        const std::string path = writeTemp("bn", std::string("appendonly ") + no + "\n");
        Config cfg;
        cfg.appendOnly = true;
        std::string err;
        CHECK(loadConfigFile(path, cfg, &err));
        CHECK(!cfg.appendOnly);
        std::remove(path.c_str());
    }
}

void testSizeSuffix()
{
    struct Case { const char *text; unsigned long long want; };
    for (const Case &c : {Case{"512", 512}, Case{"1k", 1024}, Case{"2K", 2048},
                          Case{"3kb", 3072}, Case{"64mb", 64ULL << 20},
                          Case{"1g", 1ULL << 30}, Case{"1GB", 1ULL << 30}}) {
        const std::string path = writeTemp("sz", std::string("inputhighwater ") + c.text + "\n");
        Config cfg;
        std::string err;
        CHECK(loadConfigFile(path, cfg, &err));
        CHECK_EQ(cfg.inputHighWater, c.want);
        std::remove(path.c_str());
    }
}

void testErrors()
{
    struct Case { const char *name; const char *text; const char *needle; };
    for (const Case &c : {
        Case{"unknown", "port 9000\nbogus 1\n", "unknown config key 'bogus'"},
        Case{"missing", "port 9000\nsubreactors\n", "missing value for 'subreactors'"},
        Case{"badbool", "appendonly maybe\n", "invalid boolean 'maybe'"},
        Case{"badport", "port abc\n", "invalid port 'abc'"},
        Case{"portrange", "port 70000\n", "invalid port '70000'"},
        Case{"badnum", "subreactors -1\n", "invalid number '-1'"},
        Case{"badsize", "inputhighwater 64qq\n", "invalid size '64qq'"},
    }) {
        const std::string path = writeTemp(c.name, c.text);
        Config cfg;
        std::string err;
        CHECK(!loadConfigFile(path, cfg, &err));
        CHECK(err.find(c.needle) != std::string::npos);
        CHECK(err.find(":2:") != std::string::npos || err.find(":1:") != std::string::npos);
        std::remove(path.c_str());
    }
}

void testOpenFailure()
{
    Config cfg;
    std::string err;
    CHECK(!loadConfigFile("definitely_not_here.conf", cfg, &err));
    CHECK(err.find("cannot open") != std::string::npos);
}

void testNoPartialApply()
{
    // 第一行合法、第二行出错：整份文件不生效（解析进副本）
    const std::string path = writeTemp("partial", "port 9000\nbogus 1\n");
    Config cfg;
    std::string err;
    CHECK(!loadConfigFile(path, cfg, &err));
    CHECK_EQ(cfg.port, 8080); // 仍是默认值
    std::remove(path.c_str());
}

void testValidate()
{
    std::string err;
    Config ok;
    CHECK(validateConfig(ok, &err));

    Config zeroPort;
    zeroPort.port = 0;
    CHECK(!validateConfig(zeroPort, &err));
    CHECK(err.find("port") != std::string::npos);

    Config badShards;
    badShards.shards = 3;
    CHECK(!validateConfig(badShards, &err));
    CHECK(err.find("power of two") != std::string::npos);

    Config tooMany;
    tooMany.subReactors = 257;
    CHECK(!validateConfig(tooMany, &err));
    CHECK(err.find("subReactors") != std::string::npos);

    // 无 err 出参的兼容调用（Server.cc 用法）
    CHECK(validateConfig(ok));
}

} // namespace

int main()
{
    testFullFile();
    testCommentsAndDefaults();
    testBoolVariants();
    testSizeSuffix();
    testErrors();
    testOpenFailure();
    testNoPartialApply();
    testValidate();
    std::puts("test_config: all tests passed");
    return 0;
}
