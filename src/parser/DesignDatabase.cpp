#include "parser/DesignDatabase.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <unordered_map>

namespace thdic {
namespace parser {
namespace {

constexpr int kMetalLayers = 20;

// orient 变换(与 verify_parser 区块4.1 同一套公式, 权威实现在此):
// N 不变; S:(W-x,H-y); W:(H-y,x); E:(y,W-x); F 前缀 = 先上下翻再旋转。
Rect transformRect(int W, int H, uint8_t orient, int px, int py,
                   const Rect& r) {
    Rect t;
    switch (orient) {
        case ORIENT_S:  t = Rect{W - r.xr, H - r.yt, W - r.xl, H - r.yb}; break;
        case ORIENT_W:  t = Rect{H - r.yt, r.xl, H - r.yb, r.xr}; break;
        case ORIENT_E:  t = Rect{r.yb, W - r.xr, r.yt, W - r.xl}; break;
        case ORIENT_FN: t = Rect{r.xl, H - r.yt, r.xr, H - r.yb}; break;
        case ORIENT_FS: t = Rect{W - r.xr, r.yb, W - r.xl, r.yt}; break;
        case ORIENT_FW: t = Rect{r.yb, r.xl, r.yt, r.xr}; break;
        case ORIENT_FE: t = Rect{H - r.yt, W - r.xr, H - r.yb, W - r.xl}; break;
        case ORIENT_N:
        default:        t = r; break;
    }
    return Rect{px + t.xl, py + t.yb, px + t.xr, py + t.yt};
}

int rectCenterX(const Rect& r) {
    return static_cast<int>((static_cast<long long>(r.xl) + r.xr) / 2);
}

int rectCenterY(const Rect& r) {
    return static_cast<int>((static_cast<long long>(r.yb) + r.yt) / 2);
}

}  // namespace

void DesignDatabase::buildAxis(const std::vector<int>& gcell, int dieLo,
                               int dieHi, std::vector<int>& out) const {
    out.clear();
    for (int g : gcell) {
        if (g < dieLo) continue;   // 理论上不出现, 防御
        if (g > dieHi) break;      // 超出版图边界的线全部丢弃
        if (!out.empty() && g <= out.back()) continue;  // 防重复/防倒序
        out.push_back(g);
    }
    if (out.empty() || out.front() != dieLo) out.insert(out.begin(), dieLo);
    if (out.back() < dieHi) out.push_back(dieHi);
    else out.back() = dieHi;  // 末线越界时收敛到版图边界(边缘格被裁剪)
}

int DesignDatabase::layerIdFromName(const std::string& name) const {
    auto it = layerId_.find(name);
    return it == layerId_.end() ? -1 : it->second;
}

bool DesignDatabase::cellBlocked(int col, int row, int layer) const {
    if (col < 0 || row < 0 || layer < 0 || col >= cols_ || row >= rows_ ||
        layer >= kLayers)
        return true;
    return cap_[capIndex(col, row, layer)] == 0;
}

void DesignDatabase::blockCells(const Rect& absRect, int layer) {
    if (layer < 0 || layer >= kLayers) return;
    int c1 = colOf(absRect.xl), c2 = colOf(absRect.xr);
    int r1 = rowOf(absRect.yb), r2 = rowOf(absRect.yt);
    for (int r = r1; r <= r2; ++r)
        for (int c = c1; c <= c2; ++c)
            cap_[capIndex(c, r, layer)] = 0;  // 覆盖到的格子容量清零
}

bool DesignDatabase::build(const LefParser& lef, const DefParser& def,
                           const ThermalParser& th) {
    th_ = &th;
    dbu_ = def.dbu();
    die_ = def.dieArea();
    stats_ = DesignStats{};

    if (def.gcellX().size() < 2 || def.gcellY().size() < 2) {
        std::cerr << "[DesignDatabase] GCELLGRID 缺失, 无法建网格\n";
        return false;
    }
    buildAxis(def.gcellX(), die_.xl, die_.xr, bx_);
    buildAxis(def.gcellY(), die_.yb, die_.yt, by_);
    cols_ = static_cast<int>(bx_.size()) - 1;
    rows_ = static_cast<int>(by_.size()) - 1;

    layerId_.clear();
    for (const auto& kv : lef.layers()) {
        const std::string& n = kv.first;
        if (n.rfind("metal", 0) == 0) {
            int id = 0;
            bool ok = true;
            for (size_t i = 5; i < n.size(); ++i) {
                if (n[i] < '0' || n[i] > '9') { ok = false; break; }
                id = id * 10 + (n[i] - '0');
            }
            if (ok && id >= 1 && id <= kMetalLayers)
                layerId_[n] = id - 1;
        }
    }

    // 步骤 1: TRACKS -> 每层容量(与评分器同模型: x轨按列 + y轨按行)
    cap_.assign(static_cast<size_t>(cols_) * rows_ * kMetalLayers, 0);
    for (int l = 0; l < kMetalLayers; ++l) {
        auto it = def.tracks().find("metal" + std::to_string(l + 1));
        if (it == def.tracks().end()) {
            std::cerr << "[DesignDatabase] 层 metal" << (l + 1)
                      << " 无 TRACKS, 该层容量为 0\n";
            continue;
        }
        std::vector<int> xCnt(cols_, 0), yCnt(rows_, 0);
        size_t ti = 0;
        for (int c = 0; c < cols_ && ti < it->second.xs.size(); ++c) {
            int hi = bx_[c + 1];
            while (ti < it->second.xs.size() && it->second.xs[ti] < hi) {
                if (it->second.xs[ti] >= bx_[c]) ++xCnt[c];
                ++ti;
            }
        }
        ti = 0;
        for (int r = 0; r < rows_ && ti < it->second.ys.size(); ++r) {
            int hi = by_[r + 1];
            while (ti < it->second.ys.size() && it->second.ys[ti] < hi) {
                if (it->second.ys[ti] >= by_[r]) ++yCnt[r];
                ++ti;
            }
        }
        for (int r = 0; r < rows_; ++r)
            for (int c = 0; c < cols_; ++c)
                cap_[capIndex(c, r, l)] =
                    static_cast<unsigned short>(xCnt[c] + yCnt[r]);
    }

    // 步骤 2: 扣障碍 —— 宏 OBS(含 TSV KOZ 宏的 OBS) + 固定 IO pin 覆盖格
    for (const Comp& comp : def.comps()) {
        auto it = lef.macros().find(comp.macro);
        if (it == lef.macros().end() || it->second.obs.empty()) continue;
        const Macro& macro = it->second;
        for (const Port& obs : macro.obs) {
            int layer = layerIdFromName(obs.layer);
            if (layer < 0) continue;
            Rect absRect =
                transformRect(macro.w, macro.h, comp.orient, comp.x, comp.y,
                              obs.r);
            blockCells(absRect, layer);
        }
    }
    for (const IOPin& io : def.ioPins()) {
        int layer = layerIdFromName(io.layer);
        blockCells(io.absRect, layer);
    }

    // 步骤 1(续): 端点表 —— 实例 pin 走 orient 变换, IO pin 直接用 absRect
    // 宏 pin 名 -> 下标的哈希(每宏 pin 数 <= 8, 但仍走哈希以满足底线)
    std::unordered_map<std::string,
                       std::unordered_map<std::string, size_t>> pinIdx;
    for (const auto& kv : lef.macros()) {
        std::unordered_map<std::string, size_t>& m = pinIdx[kv.first];
        for (size_t i = 0; i < kv.second.pins.size(); ++i)
            m.emplace(kv.second.pins[i].name, i);
    }

    netEndpoints_.assign(def.nets().size(), {});
    long long built = 0, ioCnt = 0, instCnt = 0;
    for (size_t ni = 0; ni < def.nets().size(); ++ni) {
        const Net& net = def.nets()[ni];
        std::vector<NetEndpoint>& eps = netEndpoints_[ni];
        eps.reserve(net.conns.size());
        for (const Conn& conn : net.conns) {
            NetEndpoint ep;
            if (conn.isIOPin) {
                const IOPin& io = def.ioPins()[conn.ownerIdx];
                ep.x = rectCenterX(io.absRect);
                ep.y = rectCenterY(io.absRect);
                ep.layer = layerIdFromName(io.layer);
                ep.fromIO = true;
            } else {
                const Comp& comp = def.comps()[conn.ownerIdx];
                auto mit = lef.macros().find(comp.macro);
                auto pit = mit == lef.macros().end()
                               ? std::unordered_map<std::string, size_t>()
                               : pinIdx[comp.macro];
                auto pi = pit.find(conn.pinName);
                if (mit == lef.macros().end() || pi == pit.end() ||
                    mit->second.pins.empty()) {
                    std::cerr << "[DesignDatabase] net '" << net.name
                              << "': pin " << comp.macro << '/' << conn.pinName
                              << " 无法定位, 端点缺失\n";
                    continue;  // 计入对账失败
                }
                const MacroPin& pin = mit->second.pins[pi->second];
                if (pin.ports.empty()) {
                    std::cerr << "[DesignDatabase] net '" << net.name
                              << "': pin " << comp.macro << '/' << conn.pinName
                              << " 无 PORT, 端点缺失\n";
                    continue;
                }
                Rect absRect = transformRect(mit->second.w, mit->second.h,
                                             comp.orient, comp.x, comp.y,
                                             pin.ports[0].r);
                ep.x = rectCenterX(absRect);
                ep.y = rectCenterY(absRect);
                ep.layer = layerIdFromName(pin.ports[0].layer);
            }
            if (ep.layer < 0 || ep.layer >= kMetalLayers) {
                std::cerr << "[DesignDatabase] net '" << net.name
                          << "': 端点层号非法, 端点缺失\n";
                continue;
            }
            eps.push_back(ep);
            ++built;
            if (ep.fromIO) ++ioCnt; else ++instCnt;
        }
    }

    // 步骤 4: 自检 —— pin 数对账 / 坐标在版图内 / 容量无负值
    long long conns = 0;
    for (const Net& net : def.nets()) conns += static_cast<long long>(net.conns.size());
    bool ok = built == conns;
    if (!ok)
        std::cerr << "[DesignDatabase] pin 数对账失败: 端点 " << built
                  << " != 连接 " << conns << "\n";

    // 坐标自检: 允许一个 GCell 步长的引脚环余量(ispd19 的边界引脚
    // 矩形中心可略微越出 DIEAREA), 超出余量才是变换错误。
    int marginX = cols_ > 1 ? bx_[1] - bx_[0] : 0;
    int marginY = rows_ > 1 ? by_[1] - by_[0] : 0;
    long long outOfDie = 0;
    for (size_t ni = 0; ni < def.nets().size() && ok; ++ni) {
        for (const NetEndpoint& e : netEndpoints_[ni]) {
            bool inDie = e.x >= die_.xl && e.x <= die_.xr && e.y >= die_.yb &&
                         e.y <= die_.yt;
            if (!inDie) ++outOfDie;
            if (e.x < die_.xl - marginX || e.x > die_.xr + marginX ||
                e.y < die_.yb - marginY || e.y > die_.yt + marginY) {
                std::cerr << "[DesignDatabase] net '" << def.nets()[ni].name
                          << "' 端点越界(超引脚环余量): (" << e.x << ","
                          << e.y << ")\n";
                ok = false;
                break;
            }
        }
    }
    stats_.outOfDieEndpoints = outOfDie;

    // 四张表统计
    stats_.nets = static_cast<long long>(def.nets().size());
    stats_.endpoints = built;
    stats_.ioEndpoints = ioCnt;
    stats_.instEndpoints = instCnt;
    stats_.connsTotal = conns;
    stats_.cols = cols_;
    stats_.rows = rows_;
    stats_.layers = kMetalLayers;
    stats_.cells = static_cast<long long>(cols_) * rows_ * kMetalLayers;

    long long blocked = 0, freeCells = 0;
    double capMin = std::numeric_limits<double>::max(), capMax = 0, capSum = 0;
    for (unsigned short c : cap_) {
        if (c == 0) {
            ++blocked;
        } else {
            ++freeCells;
            double v = static_cast<double>(c);
            capMin = std::min(capMin, v);
            capMax = std::max(capMax, v);
            capSum += v;
        }
    }
    stats_.blockedCells = blocked;
    stats_.capMin = freeCells ? capMin : 0;
    stats_.capMax = capMax;
    stats_.capAvg = freeCells ? capSum / freeCells : 0;

    stats_.unitMoveDbu = cols_ > 1 ? static_cast<double>(bx_[1] - bx_[0]) : 0;
    stats_.viaCostDbu = viaCostDbu_;
    stats_.tsvCostDbu = tsvCostDbu_;

    double tSum = 0;
    double tMin = std::numeric_limits<double>::max(), tMax = 0;
    for (const GridNode& n : th.gridNodes()) {
        if (!std::isfinite(n.temp)) continue;
        tMin = std::min(tMin, n.temp);
        tMax = std::max(tMax, n.temp);
        tSum += n.temp;
    }
    long long probes = static_cast<long long>(th.gridNodes().size());
    stats_.probes = probes;
    stats_.tMin = probes ? tMin : 0;
    stats_.tMax = tMax;
    stats_.tAvg = probes ? tSum / probes : 0;
    stats_.tDefault = th.defaultTemp();
    stats_.thermalCols = th.gridCols();
    stats_.thermalRows = th.gridRows();
    return ok;
}

const std::vector<NetEndpoint>& DesignDatabase::endpoints(int netIdx) const {
    static const std::vector<NetEndpoint> kEmpty;
    if (netIdx < 0 ||
        netIdx >= static_cast<int>(netEndpoints_.size()))
        return kEmpty;
    return netEndpoints_[netIdx];
}

int DesignDatabase::capacity(int col, int row, int layer) const {
    if (col < 0 || row < 0 || layer < 0 || col >= cols_ || row >= rows_ ||
        layer >= kLayers)
        return 0;
    return cap_[capIndex(col, row, layer)];
}

int DesignDatabase::colOf(int xDbu) const {
    auto it = std::upper_bound(bx_.begin(), bx_.end(), xDbu);
    int c = static_cast<int>(it - bx_.begin()) - 1;
    return std::clamp(c, 0, cols_ - 1);
}

int DesignDatabase::rowOf(int yDbu) const {
    auto it = std::upper_bound(by_.begin(), by_.end(), yDbu);
    int r = static_cast<int>(it - by_.begin()) - 1;
    return std::clamp(r, 0, rows_ - 1);
}

int DesignDatabase::colCenterDbu(int col) const {
    if (col < 0 || col >= cols_) return 0;
    return static_cast<int>(
        (static_cast<long long>(bx_[col]) + bx_[col + 1]) / 2);
}

int DesignDatabase::rowCenterDbu(int row) const {
    if (row < 0 || row >= rows_) return 0;
    return static_cast<int>(
        (static_cast<long long>(by_[row]) + by_[row + 1]) / 2);
}

double DesignDatabase::moveCost(int col, int row, int layer, int toCol,
                                int toRow, int toLayer) const {
    if (cellBlocked(col, row, layer) || cellBlocked(toCol, toRow, toLayer))
        return -1.0;
    if (col == toCol && row == toRow) {
        if (layer == toLayer) return 0.0;
        int lo = std::min(layer, toLayer), hi = std::max(layer, toLayer);
        if (hi - lo != 1) return -1.0;  // 只允许相邻层
        if (lo == 9 && hi == 10) return tsvCostDbu_;  // metal10 <-> metal11
        return viaCostDbu_;
    }
    if (layer == toLayer) {
        int dc = std::abs(col - toCol), dr = std::abs(row - toRow);
        if (dc + dr != 1) return -1.0;  // 只支持四邻域
        int x1 = colCenterDbu(col), y1 = rowCenterDbu(row);
        int x2 = colCenterDbu(toCol), y2 = rowCenterDbu(toRow);
        return static_cast<double>(std::abs(x2 - x1) + std::abs(y2 - y1));
    }
    return -1.0;
}

double DesignDatabase::temperature(int xDbu, int yDbu, int tier) const {
    double xUmu = (xDbu - die_.xl) / static_cast<double>(dbu_);
    double yUmu = (yDbu - die_.yb) / static_cast<double>(dbu_);
    double t = 0;
    if (th_ && th_->queryGrid(xUmu, yUmu, tier, t)) return t;
    return th_ ? th_->defaultTemp() : 300.0;
}

}  // namespace parser
}  // namespace thdic
