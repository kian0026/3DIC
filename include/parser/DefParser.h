#pragma once

#include "parser/LefParser.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace thdic {
namespace parser {

class Tokenizer;

enum IoDir : uint8_t { IO_INPUT = 0, IO_OUTPUT = 1, IO_INOUT = 2 };

struct Row {
    std::string name, site;
    int x = 0, y = 0;  // DBU
    uint8_t orient = ORIENT_N;
    int num = 0, stepX = 0;  // DO 数量与 STEP 间距
};

struct TrackSet {
    std::vector<int> xs;  // X 方向坐标表
    std::vector<int> ys;  // Y 方向坐标表
};

struct Comp {
    std::string name;
    std::string macro;  // 对应 LEF MACRO 名
    int x = 0, y = 0;
    uint8_t orient = ORIENT_N;
};

struct IOPin {
    std::string name;
    std::string net;
    uint8_t dir = IO_INPUT;
    std::string layer;
    Rect absRect;  // 已按放置点换算的绝对矩形
};

struct Conn {
    bool isIOPin = false;  // true = 来自 PINS 块
    int ownerIdx = -1;     // comps_ 或 ioPins_ 下标
    std::string pinName;
};

struct Net {
    std::string name;
    std::vector<Conn> conns;
};

class DefParser {
public:
    // 依赖注入：解析 DEF 时查 LEF 的 MACRO / LAYER 表。
    bool parse(const std::string& path,
               const std::unordered_map<std::string, Macro>& macros,
               const std::unordered_map<std::string, Layer>& layers);

    const std::string& designName() const { return designName_; }
    int dbu() const { return dbu_; }
    const Rect& dieArea() const { return dieArea_; }
    const std::vector<Row>& rows() const { return rows_; }
    const std::unordered_map<std::string, TrackSet>& tracks() const { return tracks_; }
    const std::vector<int>& gcellX() const { return gcellX_; }
    const std::vector<int>& gcellY() const { return gcellY_; }
    const std::vector<Comp>& comps() const { return comps_; }
    const std::unordered_map<std::string, int>& compIdx() const { return compIdx_; }
    const std::vector<IOPin>& ioPins() const { return ioPins_; }
    const std::unordered_map<std::string, int>& ioPinIdx() const { return ioPinIdx_; }
    const std::vector<Net>& nets() const { return nets_; }
    const std::unordered_map<std::string, int>& netIdx() const { return netIdx_; }

private:
    void parseDieArea(Tokenizer& tk);
    void parseRow(Tokenizer& tk);
    void parseTracks(Tokenizer& tk);
    void parseGCellGrid(Tokenizer& tk);
    void parseComponents(Tokenizer& tk, long long declared);
    void parsePins(Tokenizer& tk, long long declared);
    void parseNets(Tokenizer& tk, long long declared);
    void parseCompEntry(Tokenizer& tk);
    void parsePinEntry(Tokenizer& tk);
    void parseNetEntry(Tokenizer& tk);

    const std::unordered_map<std::string, Macro>* macros_ = nullptr;
    const std::unordered_map<std::string, Layer>* layers_ = nullptr;

    std::string designName_;
    int dbu_ = 2000;
    Rect dieArea_;
    std::vector<Row> rows_;
    std::unordered_map<std::string, TrackSet> tracks_;
    std::vector<int> gcellX_, gcellY_;
    std::vector<Comp> comps_;
    std::unordered_map<std::string, int> compIdx_;
    std::vector<IOPin> ioPins_;
    std::unordered_map<std::string, int> ioPinIdx_;
    std::vector<Net> nets_;
    std::unordered_map<std::string, int> netIdx_;
};

}  // namespace parser
}  // namespace thdic
