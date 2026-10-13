#pragma once

#include "parser/DefParser.h"
#include "parser/LefParser.h"
#include "parser/ThermalParser.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace thdic {
namespace parser {

// 网端点: 绝对坐标(DBU), 已含 orient 变换; IO pin 用解析器的 absRect。
struct NetEndpoint {
    int x = 0, y = 0;   // pin 矩形中心
    int layer = 0;      // 0..19 对应 metal1..metal20
    bool fromIO = false;
};

// 四张表的统计(供 main 打印 / 自检), 全部由 build 一次算出。
struct DesignStats {
    // 端点表
    long long nets = 0, endpoints = 0, ioEndpoints = 0, instEndpoints = 0;
    long long connsTotal = 0;  // 解析器侧连接总数(对账基准)
    long long outOfDieEndpoints = 0;  // 越出 DIEAREA 但在引脚环余量内
    // 容量表
    int cols = 0, rows = 0, layers = 0;
    long long cells = 0, blockedCells = 0;
    double capMin = 0, capAvg = 0, capMax = 0;  // 非阻塞格
    // 代价表
    double unitMoveDbu = 0;  // 同层相邻格中心距(均匀网格 = STEP)
    double viaCostDbu = 0.0;
    double tsvCostDbu = 0.0;
    // 温度表
    long long probes = 0;
    double tMin = 0, tAvg = 0, tMax = 0, tDefault = 0;
    int thermalCols = 0, thermalRows = 0;  // 温度网格实际尺寸(供与 gcell 对照)
};

// 第 5 步: 把三个解析器的数据整合成"网格 + 端点 + 温度"。
// 三条底线: 查询全 O(1)/O(log n) 无线性扫; 尺寸单位全部来自文件; 只读解析器数据。
class DesignDatabase {
public:
    // 步骤 1 建表 + 步骤 2 扣障碍 + 步骤 4 自检; 失败返回 false(不进布线)。
    bool build(const LefParser& lef, const DefParser& def,
               const ThermalParser& th);

    const DesignStats& stats() const { return stats_; }

    // 查询 1: 查端点 —— 网下标 -> 端点列表, O(1)。
    const std::vector<NetEndpoint>& endpoints(int netIdx) const;
    // 查询 2: 查容量 —— (col,row,layer), 0 = 障碍, O(1)。
    int capacity(int col, int row, int layer) const;
    // 查询 3: 查移动代价 —— 同层相邻格 / 同格相邻层; 不可行返回 -1。
    double moveCost(int col, int row, int layer, int toCol, int toRow,
                    int toLayer) const;
    // 查询 4: 查温度 —— DBU 坐标 + tier(0=bottom, 1=top)。
    double temperature(int xDbu, int yDbu, int tier) const;

    // 网格几何(布线必需的基础设施, 二分定位, 非数据查询)
    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int colOf(int xDbu) const;
    int rowOf(int yDbu) const;
    int colCenterDbu(int col) const;
    int rowCenterDbu(int row) const;

private:
    void buildAxis(const std::vector<int>& gcell, int dieLo, int dieHi,
                   std::vector<int>& out) const;
    int layerIdFromName(const std::string& name) const;
    void blockCells(const Rect& absRect, int layer);
    bool cellBlocked(int col, int row, int layer) const;
    size_t capIndex(int col, int row, int layer) const {
        return (static_cast<size_t>(layer) * rows_ + row) * cols_ + col;
    }

    const ThermalParser* th_ = nullptr;
    int dbu_ = 2000;
    Rect die_{};

    std::vector<int> bx_, by_;   // GCell 边界(DBU), 单调, size = 格数+1
    int cols_ = 0, rows_ = 0;
    static constexpr int kLayers = 20;
    std::vector<unsigned short> cap_;  // cols*rows*20, 0 = 障碍

    std::vector<std::vector<NetEndpoint>> netEndpoints_;
    std::unordered_map<std::string, int> layerId_;  // 层名 -> 0..19

    double viaCostDbu_ = 0.0;     // 相邻金属层过孔(与评分器缺省一致)
    double tsvCostDbu_ = 2000.0;  // metal10 <-> metal11
    DesignStats stats_;
};

}  // namespace parser
}  // namespace thdic
