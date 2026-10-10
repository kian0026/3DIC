#include "parser/DefParser.h"

#include "parser/Tokenizer.h"

#include <cmath>
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

int intOf(const Token& t) {
    double v;
    return tokenToNumber(t, v) ? static_cast<int>(std::llround(v)) : 0;
}

int orientCode(const std::string& s) {
    if (s == "N") return ORIENT_N;
    if (s == "S") return ORIENT_S;
    if (s == "W") return ORIENT_W;
    if (s == "E") return ORIENT_E;
    if (s == "FN") return ORIENT_FN;
    if (s == "FS") return ORIENT_FS;
    if (s == "FW") return ORIENT_FW;
    if (s == "FE") return ORIENT_FE;
    return -1;
}

int ioDirCode(const std::string& s) {
    if (s == "INPUT") return IO_INPUT;
    if (s == "OUTPUT") return IO_OUTPUT;
    if (s == "INOUT") return IO_INOUT;
    return -1;
}

// "( x y )"，整数 DBU。
bool readDefPoint(Tokenizer& tk, int& x, int& y) {
    Token t;
    if (!tk.next(t)) return false;
    if (!(t.type == TokType::Char && t.text == "(")) {
        tk.pushBack(t);
        return false;
    }
    tk.next(t);
    x = intOf(t);
    tk.next(t);
    y = intOf(t);
    tk.next(t);  // ')'
    return t.type == TokType::Char && t.text == ")";
}

// "( x1 y1 ) ( x2 y2 )"，左括号已由调用方消费。
Rect readDefRect(Tokenizer& tk) {
    Token t;
    tk.next(t);
    int x1 = intOf(t);
    tk.next(t);
    int y1 = intOf(t);
    tk.next(t);  // ')'
    tk.next(t);  // '('
    tk.next(t);
    int x2 = intOf(t);
    tk.next(t);
    int y2 = intOf(t);
    tk.next(t);  // ')'
    return Rect{x1, y1, x2, y2};
}

long long readCount(Tokenizer& tk) {
    Token t;
    if (!tk.next(t)) return -1;
    double v;
    return tokenToNumber(t, v) ? static_cast<long long>(std::llround(v)) : -1;
}

// 跳过整个小节，直到 END <名字>。
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

bool DefParser::parse(const std::string& path,
                      const std::unordered_map<std::string, Macro>& macros,
                      const std::unordered_map<std::string, Layer>& layers) {
    macros_ = &macros;
    layers_ = &layers;
    rows_.reserve(78);

    std::ifstream in(path);
    if (!in) {
        std::cerr << "[DefParser] cannot open " << path << "\n";
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
        if (kw == "VERSION" || kw == "DIVIDERCHAR" || kw == "BUSBITCHARS" ||
            kw == "NAMESCASESENSITIVE") {
            tk.skipToSemicolon();
        } else if (kw == "DESIGN") {
            Token name;
            tk.next(name);
            designName_ = name.text;
            tk.skipToSemicolon();
        } else if (kw == "UNITS") {
            // DEF 单行 "UNITS DISTANCE MICRONS 2000 ;"，无 END。
            Token u;
            while (tk.next(u)) {
                if (u.type == TokType::Ident && u.text == "MICRONS") {
                    double v;
                    if (nextNumber(tk, v)) dbu_ = static_cast<int>(std::llround(v));
                } else if (u.type == TokType::Char && u.text == ";") {
                    break;
                } else if (u.type == TokType::Ident && u.text == "END") {
                    Token name;
                    tk.next(name);  // 兼容 LEF 风格 "END UNITS"
                    break;
                }
            }
        } else if (kw == "DIEAREA") {
            parseDieArea(tk);
        } else if (kw == "ROW") {
            parseRow(tk);
        } else if (kw == "TRACKS") {
            parseTracks(tk);
        } else if (kw == "GCELLGRID") {
            parseGCellGrid(tk);
        } else if (kw == "VIAS") {
            // 样例中为空段；如有 DEF via 定义暂不展开。
            long long declared = readCount(tk);
            long long actual = 0;
            Token u;
            while (tk.next(u)) {
                if (u.type == TokType::Ident && u.text == "END") {
                    Token name;
                    tk.next(name);
                    break;
                }
                if (u.type == TokType::Ident && u.text == "-") ++actual;
            }
            if (declared > 0 && declared != actual)
                std::cerr << "[DefParser] VIAS declared " << declared
                          << ", skipped " << actual << "\n";
        } else if (kw == "COMPONENTS") {
            long long declared = readCount(tk);
            comps_.reserve(declared > 0 ? static_cast<size_t>(declared) : 0);
            parseComponents(tk, declared);
        } else if (kw == "PINS") {
            long long declared = readCount(tk);
            ioPins_.reserve(declared > 0 ? static_cast<size_t>(declared) : 0);
            parsePins(tk, declared);
        } else if (kw == "NETS") {
            long long declared = readCount(tk);
            nets_.reserve(declared > 0 ? static_cast<size_t>(declared) : 0);
            parseNets(tk, declared);
        } else if (kw == "SPECIALNETS" || kw == "BLOCKAGES" ||
                   kw == "PROPERTYDEFINITIONS") {
            skipSection(tk);
        } else if (kw == "END") {
            Token name;
            tk.next(name);
            if (name.text == "DESIGN") break;
        } else {
            std::cerr << "[DefParser] unknown keyword '" << kw << "', skipped\n";
            tk.skipToSemicolon();
        }
    }
    return true;
}

void DefParser::parseDieArea(Tokenizer& tk) {
    int x1, y1, x2, y2;
    readDefPoint(tk, x1, y1);
    readDefPoint(tk, x2, y2);
    dieArea_ = Rect{x1, y1, x2, y2};
    tk.skipToSemicolon();
}

void DefParser::parseRow(Tokenizer& tk) {
    Row row;
    Token t;
    if (!tk.next(t)) return;
    row.name = t.text;
    tk.next(t);
    row.site = t.text;
    tk.next(t);
    row.x = intOf(t);
    tk.next(t);
    row.y = intOf(t);
    tk.next(t);
    int orient = orientCode(t.text);
    if (orient < 0) {
        std::cerr << "[DefParser] row '" << row.name << "': bad orient '"
                  << t.text << "'\n";
    } else {
        row.orient = static_cast<uint8_t>(orient);
    }
    while (tk.next(t)) {
        if (t.type == TokType::Char && t.text == ";") break;
        if (t.type == TokType::Ident && t.text == "DO") {
            double v;
            if (nextNumber(tk, v)) row.num = static_cast<int>(std::llround(v));
        } else if (t.type == TokType::Ident && t.text == "STEP") {
            double v;
            if (nextNumber(tk, v)) row.stepX = static_cast<int>(std::llround(v));
            nextNumber(tk, v);  // stepY，本结构不保存
        }
        // "BY <numY>" 等其他 token 忽略。
    }
    rows_.push_back(std::move(row));
}

void DefParser::parseTracks(Tokenizer& tk) {
    Token t;
    if (!tk.next(t)) return;
    const bool isX = t.text == "X";
    int start = 0;
    long long count = 0;
    int step = 0;
    std::string layer;
    tk.next(t);
    start = intOf(t);
    tk.next(t);  // DO
    tk.next(t);
    count = static_cast<long long>(intOf(t));
    tk.next(t);  // STEP
    tk.next(t);
    step = intOf(t);
    tk.next(t);  // LAYER
    tk.next(t);
    layer = t.text;
    tk.skipToSemicolon();

    TrackSet& ts = tracks_[layer];
    std::vector<int>& coords = isX ? ts.xs : ts.ys;
    coords.reserve(coords.size() + static_cast<size_t>(count));
    for (long long i = 0; i < count; ++i)
        coords.push_back(start + static_cast<int>(i * step));
}

void DefParser::parseGCellGrid(Tokenizer& tk) {
    Token t;
    if (!tk.next(t)) return;
    const bool isX = t.text == "X";
    int start = 0;
    long long count = 0;
    int step = 0;
    tk.next(t);
    start = intOf(t);
    tk.next(t);  // DO
    tk.next(t);
    count = static_cast<long long>(intOf(t));
    tk.next(t);  // STEP
    tk.next(t);
    step = intOf(t);
    tk.skipToSemicolon();

    std::vector<int>& lines = isX ? gcellX_ : gcellY_;
    lines.reserve(lines.size() + static_cast<size_t>(count));
    for (long long i = 0; i < count; ++i)
        lines.push_back(start + static_cast<int>(i * step));
}

void DefParser::parseComponents(Tokenizer& tk, long long declared) {
    Token t;
    while (tk.next(t)) {
        if (t.type == TokType::Ident && t.text == "END") {
            Token name;
            tk.next(name);
            break;
        }
        if (t.type == TokType::Ident && t.text == "-") {
            parseCompEntry(tk);
        } else if (t.type == TokType::Char && t.text == ";") {
            continue;
        } else {
            tk.skipToSemicolon();
        }
    }
    if (declared >= 0 && declared != static_cast<long long>(comps_.size()))
        std::cerr << "[DefParser] COMPONENTS declared " << declared << ", parsed "
                  << comps_.size() << "\n";
}

void DefParser::parseCompEntry(Tokenizer& tk) {
    Comp comp;
    Token t;
    if (tk.next(t)) comp.name = t.text;
    if (tk.next(t)) comp.macro = t.text;
    while (tk.next(t)) {
        if (t.type == TokType::Char && t.text == ";") break;
        if (t.type == TokType::Ident && t.text == "+") {
            Token kw;
            tk.next(kw);
            const std::string& attr = kw.text;
            if (attr == "PLACED" || attr == "FIXED" || attr == "COVER" ||
                attr == "UNPLACED") {
                readDefPoint(tk, comp.x, comp.y);
                Token o;
                if (tk.next(o)) {
                    int oc = orientCode(o.text);
                    if (oc < 0) tk.pushBack(o);
                    else comp.orient = static_cast<uint8_t>(oc);
                }
            } else {
                tk.skipToSemicolon();  // SOURCE 等其他属性
                break;
            }
        }
    }
    if (compIdx_.count(comp.name))
        std::cerr << "[DefParser] duplicate component '" << comp.name << "'\n";
    compIdx_[comp.name] = static_cast<int>(comps_.size());
    comps_.push_back(std::move(comp));
}

void DefParser::parsePins(Tokenizer& tk, long long declared) {
    Token t;
    while (tk.next(t)) {
        if (t.type == TokType::Ident && t.text == "END") {
            Token name;
            tk.next(name);
            break;
        }
        if (t.type == TokType::Ident && t.text == "-") {
            parsePinEntry(tk);
        } else if (t.type == TokType::Char && t.text == ";") {
            continue;
        } else {
            tk.skipToSemicolon();
        }
    }
    if (declared >= 0 && declared != static_cast<long long>(ioPins_.size()))
        std::cerr << "[DefParser] PINS declared " << declared << ", parsed "
                  << ioPins_.size() << "\n";
}

void DefParser::parsePinEntry(Tokenizer& tk) {
    IOPin pin;
    Token t;
    if (tk.next(t)) pin.name = t.text;
    int px = 0, py = 0;
    Rect rel;
    bool hasRect = false;
    std::string curLayer;
    while (tk.next(t)) {
        if (t.type == TokType::Char && t.text == ";") break;
        if (t.type == TokType::Ident && t.text == "+") {
            Token kw;
            tk.next(kw);
            const std::string& attr = kw.text;
            if (attr == "NET") {
                Token v;
                tk.next(v);
                pin.net = v.text;
            } else if (attr == "DIRECTION") {
                Token v;
                tk.next(v);
                int d = ioDirCode(v.text);
                if (d >= 0) pin.dir = static_cast<uint8_t>(d);
            } else if (attr == "USE") {
                Token v;
                tk.next(v);  // SIGNAL/POWER/GROUND，暂不保存
            } else if (attr == "PORT") {
                // 容器关键字，无载荷。
            } else if (attr == "LAYER") {
                Token ln;
                tk.next(ln);
                curLayer = ln.text;
                while (tk.next(t) && t.type == TokType::Char && t.text == "(") {
                    rel = readDefRect(tk);  // 相对矩形；多组时取最后一组
                    hasRect = true;
                }
                if (t.type != TokType::End) tk.pushBack(t);
            } else if (attr == "PLACED" || attr == "FIXED" || attr == "COVER" ||
                       attr == "UNPLACED") {
                readDefPoint(tk, px, py);
                Token o;
                if (tk.next(o) && orientCode(o.text) < 0) tk.pushBack(o);
            } else {
                tk.skipToSemicolon();
                break;
            }
        }
    }
    if (hasRect) {
        pin.layer = curLayer;
        pin.absRect = Rect{px + rel.xl, py + rel.yb, px + rel.xr, py + rel.yt};
    }
    ioPinIdx_[pin.name] = static_cast<int>(ioPins_.size());
    ioPins_.push_back(std::move(pin));
}

void DefParser::parseNets(Tokenizer& tk, long long declared) {
    Token t;
    while (tk.next(t)) {
        if (t.type == TokType::Ident && t.text == "END") {
            Token name;
            tk.next(name);
            break;
        }
        if (t.type == TokType::Ident && t.text == "-") {
            parseNetEntry(tk);
        } else if (t.type == TokType::Char && t.text == ";") {
            continue;
        } else {
            tk.skipToSemicolon();
        }
    }
    if (declared >= 0 && declared != static_cast<long long>(nets_.size()))
        std::cerr << "[DefParser] NETS declared " << declared << ", parsed "
                  << nets_.size() << "\n";
}

void DefParser::parseNetEntry(Tokenizer& tk) {
    Net net;
    Token t;
    if (tk.next(t)) net.name = t.text;
    while (tk.next(t)) {
        if (t.type == TokType::Char && t.text == ";") break;
        if (t.type == TokType::Char && t.text == "(") {
            Conn conn;
            Token a;
            if (!tk.next(a)) break;
            if (a.type == TokType::Ident && a.text == "PIN") {
                Token pin;
                tk.next(pin);
                auto it = ioPinIdx_.find(pin.text);
                if (it != ioPinIdx_.end()) {
                    conn = Conn{true, it->second, pin.text};
                } else {
                    std::cerr << "[DefParser] net '" << net.name
                              << "': unknown IO pin '" << pin.text << "', conn skipped\n";
                }
            } else {
                Token pinName;
                tk.next(pinName);
                auto it = compIdx_.find(a.text);
                if (it != compIdx_.end()) {
                    conn = Conn{false, it->second, pinName.text};
                } else {
                    std::cerr << "[DefParser] net '" << net.name
                              << "': unknown instance '" << a.text << "', conn skipped\n";
                }
            }
            Token close;
            while (tk.next(close) && !(close.type == TokType::Char && close.text == ")")) {
            }
            if (conn.ownerIdx >= 0) net.conns.push_back(std::move(conn));
        } else if (t.type == TokType::Ident && t.text == "+") {
            Token kw;
            tk.next(kw);
            if (kw.text == "USE") {
                Token v;
                tk.next(v);
            } else {
                tk.skipToSemicolon();  // 其余属性本阶段不关心
                break;
            }
        }
    }
    netIdx_[net.name] = static_cast<int>(nets_.size());
    nets_.push_back(std::move(net));
}

}  // namespace parser
}  // namespace thdic
