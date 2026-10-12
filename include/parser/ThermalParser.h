#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace thdic {
namespace parser {

class ThermalParser;

namespace detail {
struct JsonScanner;  // 实现在 .cpp, 头文件只见前置声明
void parseMeta(JsonScanner& j, ThermalParser& out);
void parseNodes(JsonScanner& j, ThermalParser& out);
}  // namespace detail

// 温度文件格式（自动识别）:
//   GRID   官方真实格式: {"meta":{...},"nodes":[[x_um,y_um,z,temp],...]}
//          meta 可能极大(内嵌工艺栈, 数十MB), 解析时流式跳过。
//          z 值按首次出现顺序编号 => die 0=bottom, 1=top。
//   SERIES 方案假设格式: {"metadata":{"numNodes","numTimeSteps","unit"},
//          "nodes":[{"name":...,"temp":[每时间步]},...]}
enum class ThermalFormat { Unknown, Grid, Series };

// SERIES 模式节点: 实例名 -> 各时间步温度
struct ThermalNode {
    std::string name;
    std::vector<double> temp;  // temp[t] = 第t步温度
};

// GRID 模式节点: 温度探针(文件原值, 坐标为相对 die 原点的微米)
struct GridNode {
    double x = 0, y = 0;  // 微米
    int die = 0;          // 0=bottom, 1=top (按 z 首次出现顺序)
    double temp = 0;      // 文件温度单位
};

// GRID 模式对账输入: 一个 comp 实例的位置(DEF 的 DBU 绝对坐标)
struct CompLoc {
    std::string name;
    int x = 0, y = 0;
};

class ThermalParser {
public:
    // 纯解析, 零依赖, 不做对账。JSON 语法错误/截断返回 false, 不 segfault。
    bool parse(const std::string& path);

    // 对账是独立的一步, 不是解析的一部分; 在 main/verify 里拿到 DEF 数据后调用。
    // SERIES: 按实例名双向核对, 产出 nodeOfComp_/hasTemp_。
    bool reconcile(const std::unordered_map<std::string, int>& compIdx);
    // GRID: 每个 comp 用名字后缀(_bottom/_top)选 die, 按位置找同 die 最近温度
    //       探针, 产出 nodeOfComp_/hasTemp_ (布线最终接口)。
    bool reconcile(const std::vector<CompLoc>& comps, int dieX1Dbu,
                   int dieY1Dbu, int dbuPerMicron);

    // SERIES 查询: 实例名+时间步 -> 温度; 查不到返回默认温度+warning。
    double query(const std::string& inst, int t) const;
    // GRID 查询: 位置(相对 die 原点的微米)+tier(0=bottom,1=top) -> 温度。
    // 找不到同 die 节点返回 false (调用方用 defaultTemp())。
    bool queryGrid(double xUmu, double yUmu, int tier, double& out) const;

    ThermalFormat format() const { return format_; }
    int numTimeSteps() const { return numTimeSteps_; }  // GRID 恒为 0(单快照)
    const std::string& unit() const { return unit_; }
    double defaultTemp() const { return defaultTemp_; }  // 全场均值, 勿用 0
    double ambientTemp() const { return ambientTemp_; }
    int gridCols() const { return gridCols_; }
    int gridRows() const { return gridRows_; }
    int dieCount() const { return dieCount_; }
    const std::vector<ThermalNode>& nodes() const { return nodes_; }
    const std::unordered_map<std::string, int>& nodeIdx() const { return nodeIdx_; }
    const std::vector<GridNode>& gridNodes() const { return gridNodes_; }
    // 对账产物(布线最终接口): comps 下标 -> 温度节点下标 / 有无真实温度
    const std::vector<int>& nodeOfComp() const { return nodeOfComp_; }
    const std::vector<char>& hasTemp() const { return hasTemp_; }
    // 最近一次 reconcile 的告警计数(verify 用)
    int lastOrphans() const { return lastOrphans_; }
    int lastMissing() const { return lastMissing_; }

private:
    void reset();
    void finishParse();
    int locateGrid(double xUmu, double yUmu, int die) const;

    friend void detail::parseMeta(detail::JsonScanner&, ThermalParser&);
    friend void detail::parseNodes(detail::JsonScanner&, ThermalParser&);

    ThermalFormat format_ = ThermalFormat::Unknown;
    int numTimeSteps_ = 0;       // 不写死, 以文件实际内容为准
    std::string unit_ = "K";
    double defaultTemp_ = 300.0;  // 兜底常数, parse 后覆盖为全场均值(勿用 0)
    double ambientTemp_ = 0.0;
    int gridCols_ = 0, gridRows_ = 0, dieCount_ = 0;

    std::vector<ThermalNode> nodes_;                   // SERIES 存储
    std::unordered_map<std::string, int> nodeIdx_;     // 名字 -> 下标
    std::vector<GridNode> gridNodes_;                  // GRID 存储
    std::vector<double> gridXs_, gridYs_;              // 排序去重的坐标轴
    // (die, xIdx, yIdx) -> gridNodes_ 下标; 完整规则网格下查询 O(log n)
    std::unordered_map<uint64_t, int> gridAt_;

    std::vector<int> nodeOfComp_;  // comps 下标 -> 温度节点下标(-1=无)
    std::vector<char> hasTemp_;    // 该 comp 有无真实温度
    int lastOrphans_ = 0, lastMissing_ = 0;
};

}  // namespace parser
}  // namespace thdic
