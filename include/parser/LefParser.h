#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace thdic {
namespace parser {

class Tokenizer;

// 方案中 include/common/types.h 的共享类型并入此处（不新增文件）。
struct Rect {
    int xl = 0, yb = 0, xr = 0, yt = 0;
};

// 8 种朝向编码；样例中出现 N/FS，其余保留完整实现。
enum Orient : uint8_t {
    ORIENT_N = 0,
    ORIENT_S,
    ORIENT_W,
    ORIENT_E,
    ORIENT_FN,
    ORIENT_FS,
    ORIENT_FW,
    ORIENT_FE,
};

struct Layer {
    std::string name;
    std::string type;  // "ROUTING" / "CUT" / ...
    char dir = 0;      // 'H' / 'V'，CUT 层为 0
    int width = 0, pitch = 0, spacing = 0;  // DBU
    double resistance = 0.0;                // 欧姆；tsv/hb 层使用
};

struct ViaDef {
    std::string name;
    std::vector<std::pair<std::string, Rect>> shapes;  // {层名, 矩形}
};

struct Port {
    std::string layer;
    Rect r;
};

struct MacroPin {
    std::string name;
    std::string use;  // SIGNAL / POWER / GROUND
    std::vector<Port> ports;
};

struct Macro {
    std::string name;
    int w = 0, h = 0;  // DBU
    std::vector<MacroPin> pins;
    std::vector<Port> obs;
    bool isTsvKoz = false;  // 名含 TSV/KOZ
};

class LefParser {
public:
    bool parse(const std::string& path);

    int dbu() const { return dbu_; }
    int manufacturingGrid() const { return mfgGrid_; }
    const std::unordered_map<std::string, Layer>& layers() const { return layers_; }
    const std::unordered_map<std::string, ViaDef>& viaDefs() const { return viaDefs_; }
    const std::vector<std::pair<int, int>>& viaAdj() const { return viaAdj_; }
    const std::vector<std::string>& crossDieVias() const { return crossDieVias_; }
    const std::unordered_map<std::string, Macro>& macros() const { return macros_; }

private:
    void parseUnits(Tokenizer& tk);
    void parseLayer(Tokenizer& tk, const std::string& name);
    void parseVia(Tokenizer& tk, const std::string& name);
    void parseMacro(Tokenizer& tk, const std::string& name);
    void parseMacroPin(Tokenizer& tk, const std::string& name, Macro& macro);
    void parsePortList(Tokenizer& tk, std::vector<Port>& out);

    int toDbu(double microns) const {
        return static_cast<int>(std::llround(microns * dbu_));
    }

    int dbu_ = 2000;
    int mfgGrid_ = 10;
    std::unordered_map<std::string, Layer> layers_;
    std::unordered_map<std::string, ViaDef> viaDefs_;
    std::vector<std::pair<int, int>> viaAdj_;  // {下层金属编号, 上层金属编号}
    std::vector<std::string> crossDieVias_;    // metal10 <-> metal11 的 via
    std::unordered_map<std::string, Macro> macros_;
};

}  // namespace parser
}  // namespace thdic
