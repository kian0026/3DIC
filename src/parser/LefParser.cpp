#include "parser/LefParser.h"

#include "parser/Tokenizer.h"

#include <cctype>
#include <fstream>
#include <iostream>

namespace thdic {
namespace parser {
namespace {

bool nextNumber(Tokenizer& tk, double& v) {
    Token t;
    if (!tk.next(t)) return false;
    if (!tokenToNumber(t, v)) {
        tk.pushBack(t);
        return false;
    }
    return true;
}

// "RECT x1 y1 x2 y2 ;"，单位微米，含负数。
bool readLefRect(Tokenizer& tk, double dbu, Rect& out) {
    double v[4] = {0, 0, 0, 0};
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        Token t;
        if (!tk.next(t) || !tokenToNumber(t, v[i])) {
            std::cerr << "[LefParser] bad RECT coordinate\n";
            ok = false;
        }
    }
    out.xl = static_cast<int>(std::llround(v[0] * dbu));
    out.yb = static_cast<int>(std::llround(v[1] * dbu));
    out.xr = static_cast<int>(std::llround(v[2] * dbu));
    out.yt = static_cast<int>(std::llround(v[3] * dbu));
    tk.skipToSemicolon();
    return ok;
}

// "metalN" -> N；其他层名返回 0。
int metalIndex(const std::string& layer) {
    if (layer.rfind("metal", 0) != 0) return 0;
    int n = 0;
    for (size_t i = 5; i < layer.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(layer[i]))) return 0;
        n = n * 10 + (layer[i] - '0');
    }
    return n;
}

// 跳过整个小节（VIARULE/SITE/SPACING 等），直到 END <名字>。
void skipSection(Tokenizer& tk) {
    Token t;
    while (tk.next(t)) {
        if (t.type == TokType::Ident && t.text == "END") {
            Token name;
            tk.next(name);
            return;
        }
    }
}

}  // namespace

bool LefParser::parse(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "[LefParser] cannot open " << path << "\n";
        return false;
    }
    Tokenizer tk(in);
    Token t;
    while (tk.next(t)) {
        if (t.type != TokType::Ident) {
            tk.skipToSemicolon();
            continue;
        }
        const std::string& kw = t.text;
        if (kw == "VERSION" || kw == "BUSBITCHARS" || kw == "DIVIDERCHAR" ||
            kw == "NAMESCASESENSITIVE") {
            tk.skipToSemicolon();
        } else if (kw == "UNITS") {
            parseUnits(tk);
        } else if (kw == "MANUFACTURINGGRID") {
            double v;
            if (nextNumber(tk, v)) mfgGrid_ = toDbu(v);
            tk.skipToSemicolon();
        } else if (kw == "LAYER") {
            Token name;
            tk.next(name);
            parseLayer(tk, name.text);
        } else if (kw == "VIA") {
            Token name;
            tk.next(name);
            parseVia(tk, name.text);
        } else if (kw == "VIARULE" || kw == "SITE" || kw == "SPACING" ||
                   kw == "PROPERTYDEFINITIONS") {
            skipSection(tk);
        } else if (kw == "MACRO") {
            Token name;
            tk.next(name);
            parseMacro(tk, name.text);
        } else if (kw == "END") {
            Token name;
            tk.next(name);
            if (name.text == "LIBRARY") break;
        } else {
            std::cerr << "[LefParser] unknown keyword '" << kw << "', skipped\n";
            tk.skipToSemicolon();
        }
    }
    return true;
}

void LefParser::parseUnits(Tokenizer& tk) {
    Token t;
    while (tk.next(t)) {
        if (t.type == TokType::Ident && t.text == "MICRONS") {
            double v;
            if (nextNumber(tk, v)) dbu_ = static_cast<int>(std::llround(v));
        } else if (t.type == TokType::Ident && t.text == "END") {
            Token name;
            tk.next(name);
            return;
        }
    }
}

void LefParser::parseLayer(Tokenizer& tk, const std::string& name) {
    Layer layer;
    layer.name = name;
    Token t;
    while (tk.next(t)) {
        if (t.type == TokType::Char) continue;  // ';'
        if (t.type != TokType::Ident) {
            tk.skipToSemicolon();
            continue;
        }
        const std::string& kw = t.text;
        if (kw == "END") {
            Token endName;
            tk.next(endName);
            break;
        }
        if (kw == "TYPE") {
            Token val;
            tk.next(val);
            layer.type = val.text;
            tk.skipToSemicolon();
        } else if (kw == "DIRECTION") {
            Token val;
            tk.next(val);
            layer.dir =
                val.text == "HORIZONTAL" ? 'H' : val.text == "VERTICAL" ? 'V' : 0;
            tk.skipToSemicolon();
        } else if (kw == "WIDTH") {
            double v;
            if (nextNumber(tk, v)) layer.width = toDbu(v);
            tk.skipToSemicolon();
        } else if (kw == "PITCH") {
            double v;
            if (nextNumber(tk, v)) layer.pitch = toDbu(v);
            tk.skipToSemicolon();
        } else if (kw == "SPACING") {
            double v;
            if (nextNumber(tk, v)) layer.spacing = toDbu(v);
            tk.skipToSemicolon();
        } else if (kw == "RESISTANCE") {
            // 普通层为 "RESISTANCE RPERSQ <值>"，tsv/hb 为直接数值。
            double v;
            if (nextNumber(tk, v)) layer.resistance = v;
            tk.skipToSemicolon();
        } else {
            tk.skipToSemicolon();  // SPACINGTABLE/OFFSET/THICKNESS...
        }
    }
    layers_[name] = std::move(layer);
}

void LefParser::parseVia(Tokenizer& tk, const std::string& name) {
    ViaDef via;
    via.name = name;
    std::string curLayer;
    Token t;
    while (tk.next(t)) {
        if (t.type == TokType::Char) continue;
        if (t.type != TokType::Ident) {
            tk.skipToSemicolon();
            continue;
        }
        const std::string& kw = t.text;
        if (kw == "END") {
            Token endName;
            tk.next(endName);
            break;
        }
        if (kw == "DEFAULT") continue;  // 修饰符，无分号
        if (kw == "LAYER") {
            Token layer;
            tk.next(layer);
            curLayer = layer.text;
        } else if (kw == "RECT") {
            Rect r;
            readLefRect(tk, static_cast<double>(dbu_), r);
            via.shapes.emplace_back(curLayer, r);
        } else {
            tk.skipToSemicolon();
        }
    }

    // 派生数据：金属相邻对 + 跨 die via。
    int lower = 0, upper = 0;
    bool hasM10 = false, hasM11 = false;
    for (const auto& shape : via.shapes) {
        int n = metalIndex(shape.first);
        if (n > 0) {
            if (lower == 0 || n < lower) lower = n;
            if (n > upper) upper = n;
        }
        if (shape.first == "metal10") hasM10 = true;
        if (shape.first == "metal11") hasM11 = true;
    }
    if (lower != upper) {
        std::pair<int, int> adj{lower, upper};
        bool dup = false;
        for (const auto& p : viaAdj_) {
            if (p == adj) {
                dup = true;
                break;
            }
        }
        if (!dup) viaAdj_.push_back(adj);
    }
    if (hasM10 && hasM11) crossDieVias_.push_back(name);
    viaDefs_[name] = std::move(via);
}

void LefParser::parseMacro(Tokenizer& tk, const std::string& name) {
    Macro macro;
    macro.name = name;
    macro.isTsvKoz =
        name.find("TSV") != std::string::npos || name.find("KOZ") != std::string::npos;
    Token t;
    while (tk.next(t)) {
        if (t.type == TokType::Char) continue;
        if (t.type != TokType::Ident) {
            tk.skipToSemicolon();
            continue;
        }
        const std::string& kw = t.text;
        if (kw == "END") {
            Token endName;
            tk.next(endName);
            break;
        }
        if (kw == "SIZE") {
            double w = 0, h = 0;
            nextNumber(tk, w);
            Token by;
            tk.next(by);  // BY
            nextNumber(tk, h);
            macro.w = toDbu(w);
            macro.h = toDbu(h);
            tk.skipToSemicolon();
        } else if (kw == "PIN") {
            Token pinName;
            tk.next(pinName);
            parseMacroPin(tk, pinName.text, macro);
        } else if (kw == "OBS") {
            parsePortList(tk, macro.obs);
        } else {
            tk.skipToSemicolon();  // CLASS/ORIGIN/FOREIGN/SYMMETRY/SITE...
        }
    }
    macros_[name] = std::move(macro);
}

void LefParser::parseMacroPin(Tokenizer& tk, const std::string& name, Macro& macro) {
    MacroPin pin;
    pin.name = name;
    Token t;
    while (tk.next(t)) {
        if (t.type == TokType::Char) continue;
        if (t.type != TokType::Ident) {
            tk.skipToSemicolon();
            continue;
        }
        const std::string& kw = t.text;
        if (kw == "END") {
            Token endName;
            tk.next(endName);  // pin 名
            break;
        }
        if (kw == "USE") {
            Token val;
            tk.next(val);
            pin.use = val.text;
            tk.skipToSemicolon();
        } else if (kw == "PORT") {
            parsePortList(tk, pin.ports);
        } else {
            tk.skipToSemicolon();  // DIRECTION/SHAPE...
        }
    }
    macro.pins.push_back(std::move(pin));
}

// PORT/OBS 公用：LAYER..; RECT..;（一个 LAYER 可带多个 RECT），END 无名字。
void LefParser::parsePortList(Tokenizer& tk, std::vector<Port>& out) {
    std::string curLayer;
    Token t;
    while (tk.next(t)) {
        if (t.type == TokType::Char) continue;
        if (t.type != TokType::Ident) {
            tk.skipToSemicolon();
            continue;
        }
        if (t.text == "END") return;
        if (t.text == "LAYER") {
            Token layer;
            tk.next(layer);
            curLayer = layer.text;
        } else if (t.text == "RECT") {
            Rect r;
            readLefRect(tk, static_cast<double>(dbu_), r);
            out.push_back({curLayer, r});
        } else {
            tk.skipToSemicolon();
        }
    }
}

}  // namespace parser
}  // namespace thdic
