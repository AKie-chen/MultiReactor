#include "Config.h"
#include "Log.h"
#include <fstream>
#include <iostream>
#include <string>
#include <cstdlib>

// 返回 false 表示需要打印 help 并退出
bool ConfigParser::parse(int argc, char* argv[], ServerConfig& cfg)
{
    // 第一遍：加载配置文件作为基线
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" || arg == "-c") {
            if (i + 1 < argc) loadFromFile(argv[++i], cfg);
        }
    }

    // 第二遍：命令行参数覆盖（优先级高于配置文件）
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printHelp(argv[0]);
            return false;
        } else if (arg == "--config" || arg == "-c") {
            ++i;  // 已在第一遍处理，跳过值
        } else if (arg == "--port" || arg == "-p") {
            if (i + 1 < argc) cfg.port = std::atoi(argv[++i]);
        } else if (arg == "--io" || arg == "-i") {
            if (i + 1 < argc) cfg.ioThreads = std::atoi(argv[++i]);
        } else if (arg == "--workers" || arg == "-w") {
            if (i + 1 < argc) cfg.workerThreads = std::atoi(argv[++i]);
        } else if (arg == "--static-dir" || arg == "-d") {
            if (i + 1 < argc) cfg.staticDir = argv[++i];
        } else if (arg == "--max-file-size" || arg == "-m") {  // -m 与帮助文本一致
            if (i + 1 < argc) cfg.maxFileSizeMB = std::atoi(argv[++i]);
        } else if (arg == "--timeout" || arg == "-t") {
            if (i + 1 < argc) cfg.connectionTimeoutSec = std::atoi(argv[++i]);
        } else if (arg == "--log-level") {
            if (i + 1 < argc) cfg.logLevel = argv[++i];
        } else if (arg == "--max-queue-size") {
            if (i + 1 < argc) cfg.maxQueueSize = std::atoi(argv[++i]);
        } else {
            LOG_ERROR << "Unknown argument: " << arg;
            return false;
        }
    }

    return true;
}

// 从文件加载（key=value 格式）
bool ConfigParser::loadFromFile(const std::string& filepath, ServerConfig& cfg)
{
    std::ifstream fin(filepath);
    if(!fin.is_open()) {
        LOG_ERROR << "Failed to open config file: " << filepath;
        return false;
    }

    std::string line;
    while(std::getline(fin, line)) {
        if(line.empty()) continue;

        size_t pos = line.find('=');
        if(pos == std::string::npos) continue;

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        // trim whitespace from both ends of key and value
        auto trim = [](std::string &s) {
            const char* ws = " \t\r\n";
            size_t start = s.find_first_not_of(ws);
            if(start == std::string::npos) { s.clear(); return; }
            size_t end = s.find_last_not_of(ws);
            s = s.substr(start, end - start + 1);
        };
        trim(key);
        trim(value);
        if(key == "port") {
            cfg.port = std::atoi(value.c_str());
        } else if(key == "timeout") {
            cfg.connectionTimeoutSec = std::atoi(value.c_str());
        } else if(key == "log-level" || key == "log_level") {  // 兼容两种写法
            cfg.logLevel = value;
        } else if(key == "io_threads") {
            cfg.ioThreads = std::atoi(value.c_str());
        } else if(key == "worker_threads") {
            cfg.workerThreads = std::atoi(value.c_str());
        } else if(key == "static_dir") {
            cfg.staticDir = value;
        } else if(key == "max_file_size") {
            cfg.maxFileSizeMB = std::atoi(value.c_str());
        }else if(key == "max_queue_size") {
            cfg.maxQueueSize = std::atoi(value.c_str());
        } else if(key == "max_connections") {
            cfg.maxConnections = std::atoi(value.c_str());
        } else {
            LOG_WARN << "Unknown config key: " << key;
        }
    }

    return true;
}

void ConfigParser::printHelp(const char* program)
{
    std::cout << "Usage: " << program << " [options]" << std::endl
         << "Options:" << std::endl
         << "  -h, --help           Print this help message" << std::endl
         << "  -c, --config <file>  Specify the configuration file" << std::endl
         << "  -p, --port <port>     Specify the port to listen on" << std::endl
         << "  -t, --timeout <sec>  Specify the connection timeout in seconds" << std::endl
         << "  --log-level <level>  Specify the log level" << std::endl
         << "  -i, --io  Specify the number of IO threads" << std::endl
         << "  -w, --workers  Specify the number of worker threads" << std::endl
         << "  -d, --static-dir <dir>  Specify the directory for static files" << std::endl
         << "  -m, --max-file-size <size>  Specify the maximum file size in MB" << std::endl
         << "  --max-queue-size <size>  Specify the maximum queue size for the thread pool" << std::endl;
}