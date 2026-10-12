#include "parser/ThermalParser.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

namespace thdic {
namespace parser {
namespace detail {

constexpr int kWarnCap = 12;  // 每类告警最多打印条数, 计数不受限

double quantize(double v) { return std::llround(v * 1e6) / 1e6; }

uint64_t packKey(int die, int xIdx, int yIdx) {
    return (static_cast<uint64_t>(die) << 60) |
           (static_cast<uint64_t>(xIdx) << 30) |
           static_cast<uint64_t>(yIdx);
}

// 在升序坐标轴上找离 v 最近的下标。
int nearestIndex(const std::vector<double>& axis, double v) {
    auto it = std::lower_bound(axis.begin(), axis.end(), v);
    if (it == axis.end()) return static_cast<int>(axis.size()) - 1;
    int hi = static_cast<int>(it - axis.begin());
    if (hi == 0) return 0;
    double dHi = axis[hi] - v, dLo = v - axis[hi - 1];
    return dHi < dLo ? hi : hi - 1;
}

// ------------------------------------------------------------
// 极简容错 JSON 扫描器: 只抽需要的字段, 其余一概 skipValue 单遍扫过
// (官方文件的 meta 内嵌数十 MB 工艺栈, 绝不能建树)
// ------------------------------------------------------------
struct JsonScanner {
    const std::string& s;
    size_t p = 0;
    bool fail = false;
    int warns = 0;

    explicit JsonScanner(const std::string& text) : s(text) {}

    bool eof() const { return p >= s.size(); }
    char peek() { return eof() ? '\0' : s[p]; }

    void skipWs() {
        while (!eof()) {
            char c = s[p];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                ++p;
            else
                break;
        }
    }

    bool eat(char c) {
        skipWs();
        if (!eof() && s[p] == c) {
            ++p;
            return true;
        }
        return false;
    }

    void warn(const std::string& msg) {
        ++warns;
        if (warns <= kWarnCap) std::cerr << "[ThermalParser] " << msg << "\n";
    }

    bool parseString(std::string& out) {
        if (!eat('"')) return false;
        out.clear();
        while (!eof()) {
            char c = s[p++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (eof()) return false;
            char e = s[p++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                    for (int i = 0; i < 4; ++i) {
                        if (eof() || !std::isxdigit(
                                         static_cast<unsigned char>(s[p])))
                            return false;
                        ++p;
                    }
                    out += '?';
                    break;
                default: return false;
            }
        }
        return false;  // 引号未闭合
    }

    bool parseNumber(double& out) {
        skipWs();
        if (eof()) return false;
        const char* start = s.c_str() + p;
        char* end = nullptr;
        double v = std::strtod(start, &end);
        if (end == start) return false;
        p += static_cast<size_t>(end - start);
        out = v;
        return true;
    }

    void skipValue() {
        skipWs();
        if (fail || eof()) {
            fail = true;
            return;
        }
        char c = s[p];
        if (c == '"') {
            std::string t;
            if (!parseString(t)) fail = true;
            return;
        }
        if (c == '[' || c == '{') {
            char open = c, close = c == '[' ? ']' : '}';
            ++p;
            for (;;) {
                skipWs();
                if (eof()) {
                    fail = true;
                    return;
                }
                if (s[p] == close) {
                    ++p;
                    return;
                }
                if (open == '{') {
                    std::string key;
                    if (!parseString(key) || !eat(':')) {
                        fail = true;
                        return;
                    }
                }
                skipValue();
                if (fail) return;
                skipWs();
                if (!eof() && s[p] == ',')
                    ++p;
                else if (eof() || s[p] != close) {
                    fail = true;
                    return;
                }
            }
        }
        if (c == '-' || c == '+' || c == '.' ||
            std::isdigit(static_cast<unsigned char>(c))) {
            double v;
            if (!parseNumber(v)) fail = true;
            return;
        }
        for (const char* w : {"true", "false", "null"}) {
            size_t n = std::strlen(w);
            if (s.compare(p, n, w) == 0) {
                p += n;
                return;
            }
        }
        fail = true;
    }

    void skipRestArray() {  // 光标停在未消费元素上, 跳到 ']' 后
        for (;;) {
            skipWs();
            if (eof()) {
                fail = true;
                return;
            }
            if (s[p] == ']') {
                ++p;
                return;
            }
            skipValue();
            if (fail) return;
            skipWs();
            if (!eof() && s[p] == ',')
                ++p;
            else if (eof() || s[p] != ']') {
                fail = true;
                return;
            }
        }
    }
};

// meta 里值得抽的小字段, 其余(工艺栈等)一律跳过
void parseMeta(JsonScanner& j, ThermalParser& out) {
    if (!j.eat('{')) {
        j.warn("meta 不是对象, 整体跳过");
        j.skipValue();
        return;
    }
    for (;;) {
        j.skipWs();
        if (j.eat('}')) return;
        std::string key;
        if (!j.parseString(key) || !j.eat(':')) {
            j.fail = true;
            return;
        }
        if (key == "ambientTemp") {
            j.parseNumber(out.ambientTemp_);
        } else if (key == "temperatureUnit" || key == "unit") {
            std::string u;
            if (j.parseString(u)) out.unit_ = u;
        } else if (key == "gridResolution") {
            if (j.eat('[')) {
                double v[2] = {0, 0};
                for (int i = 0; i < 2; ++i) {
                    j.parseNumber(v[i]);
                    if (i == 0) j.eat(',');
                }
                out.gridCols_ = static_cast<int>(v[0]);
                out.gridRows_ = static_cast<int>(v[1]);
                j.skipRestArray();
            } else {
                j.skipValue();
            }
        } else {
            j.skipValue();
        }
        if (j.fail) return;
        j.skipWs();
        if (j.eat(',')) continue;
        if (j.eat('}')) return;
        j.fail = true;
        return;
    }
}

// GRID 条目: [x_um, y_um, z, temp] (允许尾部多余元素, temp 非数值置 NaN)
bool readGridEntry(JsonScanner& j, double v[4], int& zKey) {
    if (!j.eat('[')) return false;
    for (int i = 0; i < 4; ++i) {
        j.skipWs();
        if (!j.eof() && j.peek() == ']') {  // 元素不足 4 个
            j.warn("nodes 条目不足 4 个数值, 跳过");
            j.eat(']');
            return false;
        }
        if (!j.parseNumber(v[i])) {
            j.skipValue();
            if (j.fail) return false;
            if (i < 3) {
                j.warn("x/y/z 出现非数值, 整条跳过");
                j.skipRestArray();
                return false;
            }
            v[i] = std::numeric_limits<double>::quiet_NaN();
            j.warn("temp 出现非数值, 该点置 NaN");
        }
        if (i < 3 && !j.eat(',')) {
            j.warn("nodes 条目不足 4 个数值, 跳过");
            j.eat(']');
            return false;
        }
    }
    zKey = static_cast<int>(std::llround(v[2] * 1000.0));
    if (!j.eat(']')) {
        // 容忍尾部多余元素
        for (;;) {
            j.skipWs();
            if (j.eof()) {
                j.fail = true;
                return false;
            }
            if (j.s[j.p] == ']') {
                ++j.p;
                return true;
            }
            j.skipValue();
            if (j.fail) return false;
            j.skipWs();
            if (!j.eof() && j.s[j.p] == ',')
                ++j.p;
            else if (j.eof() || j.s[j.p] != ']') {
                j.fail = true;
                return false;
            }
        }
    }
    return true;
}

void parseNodes(JsonScanner& j, ThermalParser& out) {
    if (!j.eat('[')) {
        j.warn("nodes 不是数组");
        j.fail = true;
        return;
    }
    std::unordered_map<int, int> zToDie;  // z*1000 -> die 下标(首次出现顺序)
    std::vector<double> xs, ys;           // 原始值, 最后量化去重
    bool seriesMode = false, gridMode = false;

    for (;;) {
        j.skipWs();
        if (j.eof()) {
            j.fail = true;
            return;
        }
        if (j.peek() == ']') {
            ++j.p;
            break;
        }
        char c = j.peek();
        if (c == '[') {
            if (seriesMode) {
                j.warn("nodes 条目类型混杂, 跳过");
                j.skipValue();
            } else {
                gridMode = true;
                double v[4] = {0, 0, 0, 0};
                int zKey = 0;
                if (readGridEntry(j, v, zKey)) {
                    auto it = zToDie.find(zKey);
                    int die = static_cast<int>(zToDie.size());
                    if (it != zToDie.end()) {
                        die = it->second;
                    } else {
                        zToDie.emplace(zKey, die);
                    }
                    out.gridNodes_.push_back(GridNode{v[0], v[1], die, v[3]});
                    xs.push_back(v[0]);
                    ys.push_back(v[1]);
                }
            }
        } else if (c == '{') {
            if (gridMode) {
                j.warn("nodes 条目类型混杂, 跳过");
                j.skipValue();
            } else {
                seriesMode = true;
                ThermalNode node;
                bool hasName = false;
                j.eat('{');
                for (;;) {
                    j.skipWs();
                    if (j.eat('}')) break;
                    std::string key;
                    if (!j.parseString(key) || !j.eat(':')) {
                        j.fail = true;
                        return;
                    }
                    if (key == "name") {
                        hasName = j.parseString(node.name);
                    } else if (key == "temp") {
                        if (j.eat('[')) {
                            for (;;) {
                                j.skipWs();
                                if (j.eat(']')) break;
                                double v;
                                if (j.parseNumber(v)) {
                                    node.temp.push_back(v);
                                } else {
                                    j.skipValue();
                                    if (j.fail) return;
                                    node.temp.push_back(
                                        std::numeric_limits<double>::quiet_NaN());
                                    j.warn("temp 出现非数值, 该步置 NaN");
                                }
                                j.skipWs();
                                if (j.eat(',')) continue;
                                if (j.eat(']')) break;
                                j.fail = true;
                                return;
                            }
                        } else {
                            j.skipValue();
                            j.warn("temp 缺失或不是数组");
                        }
                    } else {
                        j.skipValue();
                    }
                    if (j.fail) return;
                    j.skipWs();
                    if (j.eat(',')) continue;
                    if (j.eat('}')) break;
                    j.fail = true;
                    return;
                }
                if (!hasName || node.name.empty()) {
                    j.warn("nodes 条目缺 name, 跳过");
                } else if (out.nodeIdx_.count(node.name)) {
                    j.warn("重复实例名 " + node.name + ", 后者覆盖");
                    out.nodes_[out.nodeIdx_[node.name]] = std::move(node);
                } else {
                    out.nodeIdx_.emplace(node.name,
                                         static_cast<int>(out.nodes_.size()));
                    out.nodes_.push_back(std::move(node));
                }
            }
        } else {
            j.warn("nodes 条目既不是对象也不是数组, 跳过");
            j.skipValue();
        }
        if (j.fail) return;
        j.skipWs();
        if (j.eat(',')) continue;
        if (j.eat(']')) break;
        j.fail = true;
        return;
    }

    if (gridMode) {
        out.format_ = ThermalFormat::Grid;
        out.dieCount_ = static_cast<int>(zToDie.size());
        std::sort(xs.begin(), xs.end());
        xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
        std::sort(ys.begin(), ys.end());
        ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
        out.gridXs_.reserve(xs.size());
        for (double v : xs) out.gridXs_.push_back(quantize(v));
        out.gridYs_.reserve(ys.size());
        for (double v : ys) out.gridYs_.push_back(quantize(v));
        if (out.gridCols_ == 0)
            out.gridCols_ = static_cast<int>(out.gridXs_.size());
        if (out.gridRows_ == 0)
            out.gridRows_ = static_cast<int>(out.gridYs_.size());
        for (int i = 0; i < static_cast<int>(out.gridNodes_.size()); ++i) {
            const GridNode& n = out.gridNodes_[i];
            int xi = nearestIndex(out.gridXs_, n.x);
            int yi = nearestIndex(out.gridYs_, n.y);
            out.gridAt_[packKey(n.die, xi, yi)] = i;
        }
        if (out.unit_ == "K") out.unit_ = "degC";  // GRID 缺省单位
    } else if (seriesMode) {
        out.format_ = ThermalFormat::Series;
    } else {
        j.warn("nodes 为空, 无法识别格式");
    }
}

}  // namespace detail

void ThermalParser::reset() {
    format_ = ThermalFormat::Unknown;
    numTimeSteps_ = 0;
    unit_ = "K";
    defaultTemp_ = 300.0;
    ambientTemp_ = 0.0;
    gridCols_ = gridRows_ = dieCount_ = 0;
    nodes_.clear();
    nodeIdx_.clear();
    gridNodes_.clear();
    gridXs_.clear();
    gridYs_.clear();
    gridAt_.clear();
    nodeOfComp_.clear();
    hasTemp_.clear();
    lastOrphans_ = lastMissing_ = 0;
}

void ThermalParser::finishParse() {
    // 默认温度 = 全场均值(勿用 0); 无有效数据时兜底 300
    double sum = 0;
    long long cnt = 0;
    auto accum = [&](double v) {
        if (std::isfinite(v)) {
            sum += v;
            ++cnt;
        }
    };
    for (const auto& n : nodes_)
        for (double v : n.temp) accum(v);
    for (const auto& n : gridNodes_) accum(n.temp);
    if (cnt > 0) {
        defaultTemp_ = sum / static_cast<double>(cnt);
    } else {
        std::cerr << "[ThermalParser] 无有效温度数据, 默认温度兜底为 300\n";
        defaultTemp_ = 300.0;
    }
    if (format_ == ThermalFormat::Series) {
        size_t maxLen = 0;
        for (const auto& n : nodes_) maxLen = std::max(maxLen, n.temp.size());
        if (numTimeSteps_ > 0 && numTimeSteps_ != static_cast<int>(maxLen)) {
            std::cerr << "[ThermalParser] numTimeSteps(" << numTimeSteps_
                      << ") 与实际长度(" << maxLen << ")不符, 以实际为准\n";
        }
        numTimeSteps_ = static_cast<int>(maxLen);
    }
}

bool ThermalParser::parse(const std::string& path) {
    reset();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "[ThermalParser] cannot open " << path << "\n";
        return false;
    }
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::string text;
    text.resize(static_cast<size_t>(size > 0 ? size : 0));
    if (size > 0) in.read(&text[0], size);

    detail::JsonScanner j(text);
    bool bom = text.size() >= 3 &&
               (unsigned char)text[0] == 0xEF &&
               (unsigned char)text[1] == 0xBB &&
               (unsigned char)text[2] == 0xBF;
    if (bom) j.p = 3;  // UTF-8 BOM

    j.skipWs();
    if (!j.eat('{')) {
        std::cerr << "[ThermalParser] 顶层不是 JSON 对象\n";
        return false;
    }
    bool hasNodes = false;
    for (;;) {
        j.skipWs();
        if (j.eof()) {
            j.fail = true;
            break;
        }
        if (j.eat('}')) break;
        std::string key;
        if (!j.parseString(key) || !j.eat(':')) {
            j.fail = true;
            break;
        }
        if (key == "nodes") {
            hasNodes = true;
            detail::parseNodes(j, *this);
        } else if (key == "meta" || key == "metadata") {
            detail::parseMeta(j, *this);
        } else {
            j.skipValue();
        }
        if (j.fail) break;
        j.skipWs();
        if (j.eat(',')) continue;
        if (j.eat('}')) break;
        j.fail = true;
        break;
    }
    if (j.fail) {
        std::cerr << "[ThermalParser] JSON 语法错误或文件截断: " << path << "\n";
        return false;
    }
    if (!hasNodes) {
        std::cerr << "[ThermalParser] 缺少 nodes 数组: " << path << "\n";
        return false;
    }
    if (j.warns > 0)
        std::cerr << "[ThermalParser] 解析完成, 共 " << j.warns
                  << " 条 warning\n";
    finishParse();
    return true;
}

bool ThermalParser::reconcile(
    const std::unordered_map<std::string, int>& compIdx) {
    lastOrphans_ = lastMissing_ = 0;
    nodeOfComp_.assign(compIdx.size(), -1);
    hasTemp_.assign(compIdx.size(), 0);

    for (const auto& kv : nodeIdx_) {
        if (!compIdx.count(kv.first)) {
            ++lastOrphans_;
            if (lastOrphans_ <= detail::kWarnCap)
                std::cerr << "[ThermalParser] 温度表多出的实例 '" << kv.first
                          << "', 该条目废弃\n";
        }
    }
    for (const auto& kv : compIdx) {
        auto it = nodeIdx_.find(kv.first);
        if (it != nodeIdx_.end()) {
            nodeOfComp_[kv.second] = it->second;
            hasTemp_[kv.second] = 1;
        } else {
            ++lastMissing_;
            if (lastMissing_ <= detail::kWarnCap)
                std::cerr << "[ThermalParser] 版图实例 '" << kv.first
                          << "' 无温度, 用默认 " << defaultTemp_ << "\n";
        }
    }
    if (lastOrphans_ > detail::kWarnCap)
        std::cerr << "[ThermalParser] ... 及另外 " << lastOrphans_ - detail::kWarnCap
                  << " 个多出实例\n";
    if (lastMissing_ > detail::kWarnCap)
        std::cerr << "[ThermalParser] ... 及另外 " << lastMissing_ - detail::kWarnCap
                  << " 个缺失实例\n";
    return lastOrphans_ == 0 && lastMissing_ == 0;
}

int ThermalParser::locateGrid(double xUmu, double yUmu, int die) const {
    if (gridXs_.empty() || gridYs_.empty()) return -1;
    int xi = detail::nearestIndex(gridXs_, xUmu);
    int yi = detail::nearestIndex(gridYs_, yUmu);
    auto it = gridAt_.find(detail::packKey(die, xi, yi));
    if (it != gridAt_.end()) return it->second;
    // 不规则网格兜底: 同 die 线性找最近(常规完整网格不会走到这里)
    double bestD2 = std::numeric_limits<double>::max();
    int best = -1;
    for (int i = 0; i < static_cast<int>(gridNodes_.size()); ++i) {
        const GridNode& n = gridNodes_[i];
        if (n.die != die || !std::isfinite(n.temp)) continue;
        double dx = n.x - xUmu, dy = n.y - yUmu;
        double d2 = dx * dx + dy * dy;
        if (d2 < bestD2) {
            bestD2 = d2;
            best = i;
        }
    }
    return best;
}

bool ThermalParser::queryGrid(double xUmu, double yUmu, int tier,
                              double& out) const {
    int idx = locateGrid(xUmu, yUmu, tier);
    if (idx < 0) {
        std::cerr << "[ThermalParser] queryGrid: tier " << tier
                  << " 无可用温度节点\n";
        return false;
    }
    out = gridNodes_[idx].temp;
    return true;
}

bool ThermalParser::reconcile(const std::vector<CompLoc>& comps, int dieX1Dbu,
                              int dieY1Dbu, int dbuPerMicron) {
    lastOrphans_ = 0;
    lastMissing_ = 0;
    nodeOfComp_.assign(comps.size(), -1);
    hasTemp_.assign(comps.size(), 0);
    if (dbuPerMicron <= 0) {
        std::cerr << "[ThermalParser] reconcile: dbuPerMicron 非法\n";
        ++lastMissing_;
        return false;
    }
    double dbu = static_cast<double>(dbuPerMicron);
    for (size_t i = 0; i < comps.size(); ++i) {
        // tier 判断: 实例名后缀 _bottom/_top, 其余按 bottom 处理
        int tier = 0;
        const std::string& n = comps[i].name;
        if (n.size() >= 4 && n.compare(n.size() - 4, 4, "_top") == 0) tier = 1;
        double xUmu = (comps[i].x - dieX1Dbu) / dbu;
        double yUmu = (comps[i].y - dieY1Dbu) / dbu;
        int idx = locateGrid(xUmu, yUmu, tier);
        if (idx >= 0) {
            nodeOfComp_[i] = idx;
            hasTemp_[i] = 1;
        } else {
            ++lastMissing_;
            if (lastMissing_ <= detail::kWarnCap)
                std::cerr << "[ThermalParser] 实例 '" << comps[i].name
                          << "' 找不到同 die 温度探针, 用默认 " << defaultTemp_
                          << "\n";
        }
    }
    if (lastMissing_ > detail::kWarnCap)
        std::cerr << "[ThermalParser] ... 及另外 " << lastMissing_ - detail::kWarnCap
                  << " 个缺失实例\n";
    return lastMissing_ == 0;
}

double ThermalParser::query(const std::string& inst, int t) const {
    auto it = nodeIdx_.find(inst);
    if (it == nodeIdx_.end()) {
        std::cerr << "[ThermalParser] query: 无 '" << inst
                  << "' 的温度, 用默认 " << defaultTemp_ << "\n";
        return defaultTemp_;
    }
    const std::vector<double>& temp = nodes_[it->second].temp;
    if (t < 0 || t >= static_cast<int>(temp.size())) {
        std::cerr << "[ThermalParser] query: '" << inst << "' 时间步 " << t
                  << " 越界, 用默认 " << defaultTemp_ << "\n";
        return defaultTemp_;
    }
    return temp[t];
}

}  // namespace parser
}  // namespace thdic
