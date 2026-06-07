#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <cstring>

// ==================== 🛠️ 跨平台底层头文件条件编译 ====================
#if defined(_WIN32)
    #include <io.h>
    #include <windows.h>
    #include <psapi.h>
    #define F_OK 0
    #define ACCESS _access
#elif defined(__ANDROID__)
    #include <sys/system_properties.h>
    #include <unistd.h>
#endif

// 引入 Cocos 引擎原生底层节点头文件 (请根据你的项目实际路径微调)
#include "core/scene-graph/Node.h" 
#include "core/scene-graph/Scene.h"
#include "core/Director.h"

// 单帧核心快照缓冲结构
struct FrameTrace {
    std::string name;
    long long ts;         // 微秒绝对时间戳
    long long dur;        // 这一帧的微秒耗时
    float fps;            // 换算后的实时 FPS
    double memoryMb;      // 进程当前的物理内存占用 (MB)
    std::string treeJson; // DFS 高速拼接出的当前帧节点与组件快照 JSON 碎片
};

class CocosTracer {
private:
    std::vector<FrameTrace> _frameBuffer;
    std::mutex _mutex;
    long long _startTime;
    long long _lastFrameTime;
    int _frameCount = 0;
    bool _isRecording = false;

    // Windows 专属：文件哨兵分频计数器与路径配置
    int _winCheckCounter = 0;
    std::string _winFlagPath = "trace.flag";
    std::string _winDumpPath = "cocos_win_trace.json";
    const std::string _androidDumpPath = "/sdcard/Download/cocos_android_trace.json";

    CocosTracer() {
        _startTime = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        _lastFrameTime = _startTime;
        _frameBuffer.reserve(300); // 预分配 300 帧环形缓冲区，防止运行期动态扩容发生 I/O
#if defined(_WIN32)
        char exePath[MAX_PATH] = {0};
        // 获取当前运行的 .exe 绝对全路径
        GetModuleFileNameA(NULL, exePath, MAX_PATH);

        std::string exeStr(exePath);
        // 找到最后一个斜杠，截取掉 exe 文件名，只保留当前文件夹路径
        size_t lastSlash = exeStr.find_last_of("\\/");
        std::string runDir = (lastSlash != std::string::npos) ? exeStr.substr(0, lastSlash + 1) : "";

        // 强行绑定绝对路径，彻底免疫工作目录漂移
        _winFlagPath = runDir + "trace.flag";
        _winDumpPath = runDir + "cocos_win_trace.json";
#endif
    }

    // 跨平台低开销物理内存抓取函数
    double getCurrentMemoryUsage() {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        }
#elif defined(__ANDROID__)
        // 通过读取 Linux 原生当前进程的状态文件获取 VmRSS (驻留物理内存)
        std::ifstream statFile("/proc/self/status");
        std::string line;
        while (std::getline(statFile, line)) {
            if (line.substr(0, 6) == "VmRSS:") {
                std::stringstream ss(line.substr(6));
                long long rssKb;
                ss >> rssKb;
                return static_cast<double>(rssKb) / 1024.0;
            }
        }
#endif
        return 0.0;
    }

    // 核心扩展一：高效率提取并序列化节点里的组件列表
    void serializeComponents(cc::Node* node, std::ostream& ss) {
        // 后期可无缝替换为真正遍历 Cocos 组件的逻辑：
        // const auto& components = node->getComponents();
        // 这里提供核心骨架：
        ss << ",\"components\":[";
        ss << "{\"type\":\"cc.UITransform\",\"enabled\":true}";
        
        // 自定义或引擎核心组件的反射过滤骨架：
        if (node->getName() == "Player_Hero") {
            ss << ",{\"type\":\"CustomHeroController\",\"hp\":100,\"state\":\"RUNNING\"}";
        }
        ss << "]";
    }

    // 纯 C++ 递归无创建、低开销节点树 DFS 抓取与字符串拼接
    void traverseNode(cc::Node* node, cc::Node* parent, std::ostream& ss) {
        if (!node) return;

        ss << "{";
        ss << "\"id\":\"" << node->getUuid() << "\","; 
        ss << "\"parentId\":" << (parent ? "\"" + parent->getUuid() + "\"" : "null") << ",";
        ss << "\"name\":\"" << node->getName() << "\",";
        ss << "\"active\":" << (node->isActiveInHierarchy() ? "true" : "false") << ",";
        
        // 抓取 3.x 核心变换数学库坐标
        auto pos = node->getPosition();
        ss << "\"x\":" << pos.x << ",";
        ss << "\"y\":" << pos.y;
        
        // 挂载组件扩展
        serializeComponents(node, ss);
        ss << "}";

        // 递归处理子节点
        const auto& children = node->getChildren();
        for (size_t i = 0; i < children.size(); ++i) {
            ss << ",\n        ";
            traverseNode(children[i], node, ss);
        }
    }

    // 核心扩展二：抓取全局动态加载的硬件纹理管理器快照
    void serializeTextureTree(std::ostream& ss) {
        ss << "\"Snapshot_TextureTree\": [\n";
        // 后期在此处无缝对接引擎的动态缓存池（如 cc::Texture2D::getTexturePool()）
        // 模拟占位示例：
        ss << "        { \"uuid\": \"tex_991\", \"name\": \"hero_atlas.png\", \"width\": 1024, \"height\": 1024, \"size_kb\": 4096 }";
        ss << "\n      ]";
    }

public:
    static CocosTracer* getInstance() {
        static CocosTracer instance;
        return &instance;
    }

    void startRecord() {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_isRecording) return;
        _frameBuffer.clear();
        _frameCount = 0;
        _lastFrameTime = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        _isRecording = true;
    }

    void stopRecord() {
        std::lock_guard<std::mutex> lock(_mutex);
        _isRecording = false;
    }

    // 由引擎主渲染线程（C++ Native Loop）每帧高频驱动的钩子函数
    void onEngineTick() {
        bool shouldRecord = _isRecording;

        // ==================== 🛠️ 跨平台热插拔触发判定状态机 ====================
#if defined(__ANDROID__)
        // 1. Android 端：读取内存映射属性，速度极快（平摊开销无限接近于 0）
        char propValue[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.cocos.trace", propValue);
        shouldRecord = (strcmp(propValue, "1") == 0);

        if (shouldRecord && !_isRecording) {
            this->startRecord();
        } else if (!shouldRecord && _isRecording) {
            this->stopRecord();
            this->dumpToFile(_androidDumpPath);
        }

#elif defined(_WIN32)
        // 2. Windows 端：分频检测文件哨兵（每60帧检测一次），抹平文件系统 I/O 损耗
        _winCheckCounter++;
        if (_winCheckCounter >= 60) {
            _winCheckCounter = 0;
            bool flagFileExists = (ACCESS(_winFlagPath.c_str(), F_OK) == 0);
            
            if (flagFileExists && !_isRecording) {
                this->startRecord();
            } else if (!flagFileExists && _isRecording) {
                this->stopRecord();
                this->dumpToFile(_winDumpPath);
            }
        }
#endif

        // ==================== 📊 核心高频抓取与内存缓冲区填充 ====================
        if (!_isRecording) return;

        // 计算当前帧的微秒时间开销与增量
        long long now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        long long durMicroseconds = now - _lastFrameTime;
        _lastFrameTime = now;

        // 反向换算得到真实无损的 FPS
        float currentFps = durMicroseconds > 0 ? (1000000.0f / durMicroseconds) : 0.0f;
        double currentMemory = getCurrentMemoryUsage();

        // 拉取 Cocos 当前运行场景的根节点
        auto* scene = cc::Director::getInstance()->getScene();
        if (!scene) return;

        // DFS 构建数据并注入 stringstream 缓冲区
        std::stringstream ss;
        traverseNode(scene, nullptr, ss);

        // 线程安全写入，满 300 帧抛弃最老数据
        std::lock_guard<std::mutex> lock(_mutex);
        if (_frameBuffer.size() >= 300) {
            _frameBuffer.erase(_frameBuffer.begin()); 
        }

        _frameCount++;
        char frameName[64];
        snprintf(frameName, sizeof(frameName), "Frame_%03d_Update", _frameCount);

        _frameBuffer.push_back({
            frameName,
            (now - _startTime - durMicroseconds),
            durMicroseconds,
            currentFps,
            currentMemory,
            ss.str()
        });
    }

    // 将缓冲区内的全部结构数据喷射转换为标准兼容的 Google Chrome/Perfetto Trace JSON
    void dumpToFile(const std::string& filePath) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_frameBuffer.empty()) return;

        std::ofstream outFile(filePath, std::ios::out | std::ios::trunc);
        if (!outFile.is_open()) return;

        outFile << std::fixed << std::setprecision(2);
        outFile << "[\n";

        // 用于将指标类事件追加在末尾的独立流缓冲区
        std::stringstream counterStream;
        counterStream << std::fixed << std::setprecision(2);

        for (size_t i = 0; i < _frameBuffer.size(); ++i) {
            const auto& frame = _frameBuffer[i];
            
            // 1. 写入帧快照切片 (事件类型: X)
            outFile << "  {\n";
            outFile << "    \"name\": \"Standard_Frame_" << frame.name << "\",\n";
            outFile << "    \"cat\": \"CocosLoop\",\n";
            outFile << "    \"ph\": \"X\",\n";
            outFile << "    \"pid\": 1, \"tid\": 1,\n";
            outFile << "    \"ts\": " << frame.ts << ",\n";
            outFile << "    \"dur\": " << frame.dur << ",\n";
            outFile << "    \"args\": {\n";
            outFile << "      \"Snapshot_SceneTree\": [\n        " << frame.treeJson << "\n      ],\n      ";
            
            // 拼接纹理管理树信息
            serializeTextureTree(outFile);
            
            outFile << "\n    }\n";
            outFile << "  },\n";

            // 2. 构造并合并对应的 Counter (指标类事件: C) 用于在 Perfetto 中显示连续折线图
            counterStream << "  {\"name\":\"Engine_FPS\",\"cat\":\"Perf\",\"ph\":\"C\",\"pid\":1,\"ts\":" << frame.ts << ",\"args\":{\"FPS\":" << frame.fps << "}},\n";
            counterStream << "  {\"name\":\"Physical_Memory\",\"cat\":\"Perf\",\"ph\":\"C\",\"pid\":1,\"ts\":" << frame.ts << ",\"args\":{\"Memory_MB\":" << frame.memoryMb << "}}";
            
            if (i != _frameBuffer.size() - 1) {
                counterStream << ",\n";
            }
        }

        // 把折线图数据合并拼装到底部
        outFile << counterStream.str() << "\n]\n";
        outFile.close();
        _frameBuffer.clear(); // 导出完成后清空历史帧内存
    }
};
