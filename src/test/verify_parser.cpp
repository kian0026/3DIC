// verify_parser.cpp —— 解析器体检报告（独立编译，不进主程序）
// 用法: ./verify_parser cases/sample001/sample001
// 编译: g++ -std=c++17 -O2 -Wall -Wextra -I include
//       src/test/verify_parser.cpp src/parser/*.cpp -o verify_parser.exe
// 注: Windows 控制台若中文乱码，编译时加 -fexec-charset=GBK

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "parser/LefParser.h"
#include "parser/DefParser.h"
#include "parser/Tokenizer.h"

using namespace thdic::parser;

// ------------------------------------------------------------
// 区块0: 报告工具 —— 所有检查共用，这就是"直观感受"的来源
// ------------------------------------------------------------
static int gPass = 0, gFail = 0;
static std::vector<std::string> gFailed;

static void check(bool ok, const std::string& desc,
                  const std::string& detail = "") {
    if (ok) {
        ++gPass;
        std::cout << "  [PASS] " << desc;
    } else {
        ++gFail;
        gFailed.push_back(desc);
        std::cout << "  [FAIL] " << desc;
    }
    if (!detail.empty()) std::cout << "  (" << detail << ")";
    std::cout << "\n";
}

static void section(const std::string& title) {
    std::cout << "\n================ " << title << " ================\n";
}

static std::string rectStr(const Rect& r) {
    std::ostringstream os;
    os << "(" << r.xl << "," << r.yb << ")-(" << r.xr << "," << r.yt << ")";
    return os.str();
}

static const char* orientName(uint8_t o) {
    static const char* kNames[] = {"N", "S", "W", "E", "FN", "FS", "FW", "FE"};
    return o < 8 ? kNames[o] : "?";
}

static std::string connStr(const DefParser& def, const Conn& c) {
    if (c.isIOPin) return "[IO]" + def.ioPins()[c.ownerIdx].name;
    return def.comps()[c.ownerIdx].name + "." + c.pinName;
}

// tier判断: metalN<=10 -> tier0(bottom), else tier1(top)
static int tierOf(const std::string& layer) {
    if (layer.rfind("metal", 0) != 0) return -1;
    int n = 0;
    for (size_t i = 5; i < layer.size(); ++i) {
        if (layer[i] < '0' || layer[i] > '9') return -1;
        n = n * 10 + (layer[i] - '0');
    }
    return n <= 10 ? 0 : 1;
}

// 朝向变换参考实现（DesignDatabase::build 将来就用这套公式）
// N:(x,y)  S:(W-x,H-y)  W:(H-y,x)  E:(y,W-x)
// F前缀 = 先上下翻(y->H-y)再旋转; FS 等价于左右翻(x->W-x)
static Rect transformPinRect(int W, int H, uint8_t orient, int px, int py,
                             const Rect& r) {
    Rect t;
    switch (orient) {
        case ORIENT_N:  t = Rect{r.xl, r.yb, r.xr, r.yt}; break;
        case ORIENT_S:  t = Rect{W - r.xr, H - r.yt, W - r.xl, H - r.yb}; break;
        case ORIENT_W:  t = Rect{H - r.yt, r.xl, H - r.yb, r.xr}; break;
        case ORIENT_E:  t = Rect{r.yb, W - r.xr, r.yt, W - r.xl}; break;
        case ORIENT_FN: t = Rect{r.xl, H - r.yt, r.xr, H - r.yb}; break;
        case ORIENT_FS: t = Rect{W - r.xr, r.yb, W - r.xl, r.yt}; break;
        case ORIENT_FW: t = Rect{r.yb, r.xl, r.yt, r.xr}; break;
        case ORIENT_FE: t = Rect{H - r.yt, W - r.xr, H - r.yb, W - r.xl}; break;
        default: return r;
    }
    return Rect{px + t.xl, py + t.yb, px + t.xr, py + t.yt};
}

static bool readAll(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream os;
    os << in.rdbuf();
    out = os.str();
    return true;
}

// 变异测试用：内容写临时文件 -> 解析 -> 删临时文件
static bool parseDefFromString(const std::string& content, DefParser& def,
                               const LefParser& lef) {
    const char* tmp = ".verify_tmp.def";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << content;
    }
    bool ok = def.parse(tmp, lef.macros(), lef.layers());
    std::remove(tmp);
    return ok;
}

// ------------------------------------------------------------
// 区块1: Tokenizer 检查 —— 用内嵌字符串，不依赖外部文件
// ------------------------------------------------------------
static void verifyTokenizer() {
    section("区块1 Tokenizer");

    // 1.1 基本切分
    {
        std::istringstream in("LAYER metal1 ;\n  RECT 0.5 -70 ( foo )\n");
        Tokenizer tk(in);
        struct Expect { TokType type; const char* text; };
        const Expect kExp[] = {
            {TokType::Ident, "LAYER"}, {TokType::Ident, "metal1"},
            {TokType::Char, ";"},      {TokType::Ident, "RECT"},
            {TokType::Number, "0.5"},  {TokType::Ident, "-70"},
            {TokType::Char, "("},      {TokType::Ident, "foo"},
            {TokType::Char, ")"},
        };
        std::vector<std::string> got;
        Token t;
        while (tk.next(t) && t.type != TokType::End) got.push_back(t.text);
        bool ok = got.size() == sizeof(kExp) / sizeof(kExp[0]);
        if (ok) {
            std::istringstream in2("LAYER metal1 ;\n  RECT 0.5 -70 ( foo )\n");
            Tokenizer tk2(in2);
            Token u;
            for (size_t i = 0; i < got.size() && ok; ++i) {
                tk2.next(u);
                if (u.type != kExp[i].type || u.text != kExp[i].text) ok = false;
            }
        }
        std::cout << "  原文: LAYER metal1 ; RECT 0.5 -70 ( foo )\n  token: ";
        for (const auto& s : got) std::cout << "\"" << s << "\" ";
        std::cout << "\n";
        std::ostringstream det;
        det << "切出 " << got.size() << " 个token";
        check(ok, "1.1 基本切分(数字/负数/括号)", det.str());
    }

    // 1.2 注释吞噬
    {
        std::istringstream in("# copyright 2010 Nangate\nA # inline note\nB\n");
        Tokenizer tk(in);
        Token a, b, end;
        tk.next(a);
        tk.next(b);
        bool eof = !tk.next(end) || end.type == TokType::End;
        std::ostringstream det;
        det << "读到 \"" << a.text << "\" \"" << b.text << "\"";
        check(a.text == "A" && b.text == "B" && eof,
              "1.2 '#'注释不进token", det.str());
    }

    // 1.3 pushBack还原
    {
        std::istringstream in("A B C");
        Tokenizer tk(in);
        Token t1, t2, r1, r2, r3;
        tk.next(t1);
        tk.next(t2);
        tk.pushBack(t2);  // 回退 B
        tk.next(r1);      // B
        tk.next(r2);      // C
        bool eof = !tk.next(r3) || r3.type == TokType::End;
        check(t1.text == "A" && t2.text == "B" && r1.text == "B" &&
                  r2.text == "C" && eof,
              "1.3 pushBack后序列 A B B C End");
    }

    // 1.4 skipToSemicolon
    {
        std::istringstream in("GARBAGE junk ; NEXT ;");
        Tokenizer tk(in);
        Token bad, after;
        tk.next(bad);
        tk.skipToSemicolon();
        bool has = tk.next(after);
        std::ostringstream det;
        det << "起点 \"" << bad.text << "\" -> 跳过后 \""
            << (has ? after.text : "<EOF>") << "\"";
        check(has && after.text == "NEXT", "1.4 skipToSemicolon错误恢复",
              det.str());
    }
}

// ------------------------------------------------------------
// 区块2: 总量对账 —— 用文件自带数量做免费答案
// ------------------------------------------------------------
static void verifyCounts(const DefParser& def, const LefParser& lef) {
    section("区块2 总量对账");

    check(def.designName() == "die_bottom_min", "design名", def.designName());
    check(def.dbu() == 2000 && lef.dbu() == 2000, "dbu==2000",
          "def=" + std::to_string(def.dbu()) + " lef=" +
              std::to_string(lef.dbu()));
    check(lef.manufacturingGrid() == 10, "mfgGrid==10 (0.005um)",
          std::to_string(lef.manufacturingGrid()));
    {
        const Rect& d = def.dieArea();
        check(d.xl == 0 && d.yb == 0 && d.xr == 240920 && d.yt == 240800,
              "dieArea==(0,0,240920,240800)", rectStr(d));
    }
    check(def.rows().size() == 78, "rows==78", std::to_string(def.rows().size()));
    check(def.comps().size() == 218, "comps==218",
          std::to_string(def.comps().size()));
    check(def.ioPins().size() == 12, "ioPins==12",
          std::to_string(def.ioPins().size()));
    check(def.nets().size() == 203, "nets==203",
          std::to_string(def.nets().size()));
    check(def.gcellX().size() == 42 && def.gcellY().size() == 42,
          "gcellX/Y线数==42",
          std::to_string(def.gcellX().size()) + "/" +
              std::to_string(def.gcellY().size()));
    check(def.tracks().at("metal1").xs.size() == 860 &&
              def.tracks().at("metal1").ys.size() == 860,
          "metal1 track X/Y==860",
          std::to_string(def.tracks().at("metal1").xs.size()) + "/" +
              std::to_string(def.tracks().at("metal1").ys.size()));
    check(lef.macros().size() == 8, "macros==8种",
          std::to_string(lef.macros().size()));
    {
        std::ostringstream det;
        for (const auto& n : lef.crossDieVias()) det << n << " ";
        check(lef.crossDieVias().size() == 2, "crossDieVias==2条", det.str());
    }
    check(lef.viaAdj().size() == 19, "viaAdj金属对==19",
          std::to_string(lef.viaAdj().size()));

    {
        bool allMetal = true;
        std::string missing;
        for (int i = 1; i <= 20; ++i) {
            std::string n = "metal" + std::to_string(i);
            if (!lef.layers().count(n)) {
                allMetal = false;
                missing += n + " ";
            }
        }
        check(allMetal, "layers含metal1..20",
              missing.empty() ? "20/20" : "缺:" + missing);
    }
    {
        bool ok = lef.layers().count("via1") && lef.layers().count("via2") &&
                  lef.layers().count("via3") && lef.layers().count("via4");
        check(ok, "layers含via1..via4");
    }
    check(lef.layers().count("tsv_layer") && lef.layers().count("hb_layer"),
          "layers含tsv_layer+hb_layer");
    check(def.compIdx().size() == def.comps().size(), "comp实例名无重复",
          std::to_string(def.compIdx().size()));

    std::cout << "\n  汇总表:\n";
    std::cout << "  | rows | comps | ioPins | nets | macros | viaDefs | crossDie |\n";
    std::cout << "  | " << std::setw(4) << def.rows().size()
              << " | " << std::setw(5) << def.comps().size()
              << " | " << std::setw(6) << def.ioPins().size()
              << " | " << std::setw(4) << def.nets().size()
              << " | " << std::setw(6) << lef.macros().size()
              << " | " << std::setw(7) << lef.viaDefs().size()
              << " | " << std::setw(8) << lef.crossDieVias().size()
              << " |\n";
}

// ------------------------------------------------------------
// 区块3: 内容抽查 —— 每类容器抽首条+特殊条，打印"文件原文 vs 程序读到的"
// ------------------------------------------------------------
static void verifyContents(const DefParser& def, const LefParser& lef) {
    section("区块3 内容抽查(原文 vs 程序)");

    // 3.1 MACRO抽查
    {
        const Macro& m = lef.macros().at("BUF_X1_bottom");
        std::cout << "  原文: MACRO BUF_X1_bottom / SIZE 0.57 BY 1.4 ;\n";
        std::cout << "  程序: w=" << m.w << " h=" << m.h << " DBU, pins="
                  << m.pins.size() << "\n";
        check(m.w == 1140 && m.h == 2800, "BUF_X1_bottom SIZE 0.57x1.4um",
              std::to_string(m.w) + "x" + std::to_string(m.h));
        check(m.pins.size() == 4, "BUF_X1_bottom 4个pin(A/Z/VDD/VSS)",
              std::to_string(m.pins.size()));

        const MacroPin& pa = m.pins[0];  // A
        std::cout << "  原文: PIN A / LAYER metal1 / RECT 0.06 0.525 0.19 0.70\n";
        std::cout << "  程序: " << pa.name << " " << pa.ports[0].layer
                  << " " << rectStr(pa.ports[0].r) << "\n";
        check(pa.name == "A" && pa.ports[0].layer == "metal1" &&
                  pa.ports[0].r.xl == 120 && pa.ports[0].r.yb == 1050 &&
                  pa.ports[0].r.xr == 380 && pa.ports[0].r.yt == 1400,
              "pin A metal1 (120,1050)-(380,1400)", rectStr(pa.ports[0].r));
        check(m.pins[2].use == "POWER" && m.pins[2].ports.size() == 2,
              "VDD use=POWER 且2个RECT",
              m.pins[2].name + " rects=" +
                  std::to_string(m.pins[2].ports.size()));

        const Macro& ts = lef.macros().at("TSV_BOTTOM_M10_5X5_KOZ1");
        std::cout << "  原文: MACRO TSV_BOTTOM_M10_5X5_KOZ1 / SIZE 7 BY 7\n";
        std::cout << "  程序: w=" << ts.w << " h=" << ts.h
                  << " isTsvKoz=" << (ts.isTsvKoz ? "true" : "false")
                  << " pin=" << ts.pins[0].name << " "
                  << ts.pins[0].ports[0].layer << " "
                  << rectStr(ts.pins[0].ports[0].r) << "\n";
        check(ts.isTsvKoz && ts.w == 14000 && ts.h == 14000,
              "TSV_..._KOZ1 isTsvKoz=true, 8种macro之一");
        check(ts.pins[0].ports[0].layer == "metal10" &&
                  ts.pins[0].ports[0].r.xl == 2000 &&
                  ts.pins[0].ports[0].r.xr == 12000,
              "TSV_PORT metal10 (2000,2000)-(12000,12000)",
              rectStr(ts.pins[0].ports[0].r));
    }

    // 3.2 LAYER抽查
    {
        const Layer& m1 = lef.layers().at("metal1");
        std::cout << "  原文: LAYER metal1 / DIRECTION HORIZONTAL / WIDTH 0.07"
                  << " / PITCH 0.14 / SPACING 0.065\n";
        std::cout << "  程序: dir=" << m1.dir << " width=" << m1.width
                  << " pitch=" << m1.pitch << " spacing=" << m1.spacing << "\n";
        check(m1.dir == 'H' && m1.width == 140 && m1.pitch == 280 &&
                  m1.spacing == 130,
              "metal1 dir=H width=140 pitch=280 spacing=130");

        const Layer& tsv = lef.layers().at("tsv_layer");
        const Layer& hb = lef.layers().at("hb_layer");
        std::cout << "  原文: tsv_layer WIDTH 5.0 RESISTANCE 0.002 / "
                  << "hb_layer WIDTH 0.5 RESISTANCE 0.01\n";
        std::cout << "  程序: tsv width=" << tsv.width << " res="
                  << tsv.resistance << " / hb width=" << hb.width
                  << " res=" << hb.resistance << "\n";
        check(tsv.width == 10000 && tsv.resistance == 0.002,
              "tsv_layer width=10000 res=0.002");
        check(hb.width == 1000 && hb.resistance == 0.01,
              "hb_layer width=1000 res=0.01");
    }

    // 3.3 VIA抽查
    {
        const ViaDef& v = lef.viaDefs().at("via1_0");
        std::cout << "  原文: VIA via1_0 / via1(-0.035..0.035) / "
                  << "metal1(-0.07..0.07) / metal2(-0.07..0.07)\n";
        std::cout << "  程序: " << v.shapes.size() << "层: ";
        for (const auto& s : v.shapes)
            std::cout << s.first << rectStr(s.second) << " ";
        std::cout << "\n";
        bool ok = v.shapes.size() == 3 &&
                  v.shapes[0].first == "via1" &&
                  v.shapes[0].second.xl == -70 &&
                  v.shapes[1].first == "metal1" &&
                  v.shapes[1].second.xl == -140 &&
                  v.shapes[2].first == "metal2" &&
                  v.shapes[2].second.xr == 140;
        check(ok, "via1_0 三层矩形DBU换算");

        const ViaDef& hb = lef.viaDefs().at("hb_layer_0");
        bool has10 = false, has11 = false;
        for (const auto& s : hb.shapes) {
            if (s.first == "metal10") has10 = true;
            if (s.first == "metal11") has11 = true;
        }
        std::cout << "  程序: hb_layer_0 " << hb.shapes.size() << "层, "
                  << "metal10=" << has10 << " metal11=" << has11 << "\n";
        check(has10 && has11 && hb.shapes.size() == 3,
              "hb_layer_0 含metal10+metal11 [跨die]");
    }

    // 3.4 COMPONENTS抽查
    {
        const Comp& c0 = def.comps()[0];
        std::cout << "  原文: - RESOURCE_SLOT_0001_bottom RESOURCE_SLOT_bottom "
                  << "+ FIXED ( 111590 60340 ) N\n";
        std::cout << "  程序: comps[0] " << c0.name << " / " << c0.macro
                  << " / (" << c0.x << "," << c0.y << ") / "
                  << orientName(c0.orient) << "\n";
        check(c0.name == "RESOURCE_SLOT_0001_bottom" &&
                  c0.macro == "RESOURCE_SLOT_bottom" && c0.x == 111590 &&
                  c0.y == 60340 && c0.orient == ORIENT_N,
              "comps[0] 首条完整对账");

        const Comp& fs = def.comps()[def.compIdx().at("u_buf_l0_00_bottom")];
        std::cout << "  程序: u_buf_l0_00_bottom (" << fs.x << "," << fs.y
                  << ") " << orientName(fs.orient) << "\n";
        check(fs.x == 53200 && fs.y == 204400 && fs.orient == ORIENT_FS,
              "PLACED FS条目: u_buf_l0_00_bottom (53200,204400) FS");

        const Comp& fx = def.comps()[def.compIdx().at("u_macro0_bottom")];
        std::cout << "  程序: u_macro0_bottom " << fx.macro << " ("
                  << fx.x << "," << fx.y << ") " << orientName(fx.orient)
                  << "\n";
        check(fx.macro == "DEMO_MACRO_bottom" && fx.x == 21660 &&
                  fx.y == 193200 && fx.orient == ORIENT_N,
              "FIXED条目: u_macro0_bottom (21660,193200) N");
    }

    // 3.5 PINS抽查
    {
        const IOPin& mon0 = def.ioPins()[def.ioPinIdx().at("mon0")];
        std::cout << "  原文: - mon0 + NET mon0 + DIRECTION OUTPUT + LAYER "
                  << "metal2 (-70 -70)(70 70) + PLACED ( 119510 240730 ) N\n";
        std::cout << "  程序: net=" << mon0.net << " layer=" << mon0.layer
                  << " absRect=" << rectStr(mon0.absRect) << "\n";
        check(mon0.net == "mon0" && mon0.layer == "metal2" &&
                  mon0.dir == IO_OUTPUT &&
                  mon0.absRect.xl == 119510 - 70 &&
                  mon0.absRect.yb == 240730 - 70 &&
                  mon0.absRect.xr == 119510 + 70 &&
                  mon0.absRect.yt == 240730 + 70,
              "mon0 absRect=(119440,240660)-(119580,240800)",
              rectStr(mon0.absRect));

        const IOPin& vb = def.ioPins()[def.ioPinIdx().at("vlink0_bottom")];
        std::cout << "  原文: - vlink0_bottom + LAYER metal10 (-5000..5000) "
                  << "+ FIXED ( 51390 67340 ) N\n";
        std::cout << "  程序: layer=" << vb.layer
                  << " absRect=" << rectStr(vb.absRect) << "\n";
        check(vb.layer == "metal10" && vb.absRect.xl == 51390 - 5000 &&
                  vb.absRect.yb == 67340 - 5000 &&
                  vb.absRect.xr == 51390 + 5000 &&
                  vb.absRect.yt == 67340 + 5000,
              "vlink0_bottom absRect=(46390,62340)-(56390,72340)",
              rectStr(vb.absRect));
    }

    // 3.6 NETS抽查
    {
        const Net& n0 = def.nets()[def.netIdx().at("BOTTOM__l0_b0")];
        std::cout << "  原文: - BOTTOM__l0_b0 ( u_buf_l0_01_bottom A ) "
                  << "( u_buf_l0_00_bottom Z ) + USE SIGNAL\n";
        std::cout << "  程序: " << n0.conns.size() << "连接: ";
        for (const auto& c : n0.conns) std::cout << connStr(def, c) << " ";
        std::cout << "\n";
        check(n0.conns.size() == 2 &&
                  connStr(def, n0.conns[0]) == "u_buf_l0_01_bottom.A" &&
                  connStr(def, n0.conns[1]) == "u_buf_l0_00_bottom.Z",
              "BOTTOM__l0_b0 2连接全对");

        const Net& v = def.nets()[def.netIdx().at("vlink0")];
        std::cout << "  原文: - vlink0 ( PIN vlink0_bottom ) "
                  << "( u_macro0_bottom A ) ( u_macro0_top Z ) "
                  << "( PIN vlink0_top )\n";
        std::cout << "  程序: " << v.conns.size() << "连接: ";
        for (const auto& c : v.conns) std::cout << connStr(def, c) << " ";
        std::cout << "\n";
        int ioCnt = 0;
        for (const auto& c : v.conns) ioCnt += c.isIOPin ? 1 : 0;
        check(v.conns.size() == 4 && ioCnt == 2 &&
                  connStr(def, v.conns[0]) == "[IO]vlink0_bottom" &&
                  connStr(def, v.conns[3]) == "[IO]vlink0_top",
              "vlink0 4连接, 2个IO pin");
    }
}

// ------------------------------------------------------------
// 区块4: 派生数据复算 —— pin绝对坐标，纸上先手算，程序对答案
// ------------------------------------------------------------
static void verifyDerived(const DefParser& def, const LefParser& lef) {
    section("区块4 派生数据复算");

    // 4.1 朝向变换8种逐一验证
    // 假macro 100x50, pin (10,10)-(20,30), 放在(1000,2000), 手算硬编码:
    {
        struct OrientCase { uint8_t orient; Rect expect; };
        const OrientCase kCases[8] = {
            {ORIENT_N,  Rect{1010, 2010, 1020, 2030}},
            {ORIENT_S,  Rect{1080, 2020, 1090, 2040}},
            {ORIENT_W,  Rect{1020, 2010, 1040, 2020}},
            {ORIENT_E,  Rect{1010, 2080, 1030, 2090}},
            {ORIENT_FN, Rect{1010, 2020, 1020, 2040}},
            {ORIENT_FS, Rect{1080, 2010, 1090, 2030}},
            {ORIENT_FW, Rect{1010, 2010, 1030, 2020}},
            {ORIENT_FE, Rect{1020, 2080, 1040, 2090}},
        };
        const Rect pin{10, 10, 20, 30};
        std::cout << "  朝向 | 期望矩形(手算)          | 程序矩形\n";
        for (const auto& oc : kCases) {
            Rect got = transformPinRect(100, 50, oc.orient, 1000, 2000, pin);
            std::cout << "  " << std::setw(4) << orientName(oc.orient)
                      << " | " << std::setw(23) << rectStr(oc.expect)
                      << " | " << rectStr(got) << "\n";
            std::ostringstream det;
            det << "期望" << rectStr(oc.expect) << " 实际" << rectStr(got);
            check(got.xl == oc.expect.xl && got.yb == oc.expect.yb &&
                      got.xr == oc.expect.xr && got.yt == oc.expect.yt,
                  std::string("4.1 朝向 ") + orientName(oc.orient), det.str());
        }
    }

    // 4.2 端到端pin坐标: BOTTOM__l0_b0 两个pin走完整链路
    {
        const Net& net = def.nets()[def.netIdx().at("BOTTOM__l0_b0")];
        const Rect kExp[2] = {
            Rect{52940, 208250, 53200, 208600},  // u_buf_l0_01_bottom A, N
            Rect{53320, 204780, 53500, 206880},  // u_buf_l0_00_bottom Z, FS
        };
        int i = 0;
        for (const auto& c : net.conns) {
            const Comp& comp = def.comps()[c.ownerIdx];
            const Macro& macro = lef.macros().at(comp.macro);
            const MacroPin* pin = nullptr;
            for (const auto& p : macro.pins)
                if (p.name == c.pinName) pin = &p;
            if (!pin || pin->ports.empty()) {
                check(false, "4.2 链路查不到pin", comp.name + "." + c.pinName);
                ++i;
                continue;
            }
            const Port& port = pin->ports[0];
            Rect abs = transformPinRect(macro.w, macro.h, comp.orient,
                                        comp.x, comp.y, port.r);
            std::cout << "  inst(" << comp.x << "," << comp.y << ","
                      << orientName(comp.orient) << ") x macro_pin"
                      << rectStr(port.r) << " -> 绝对" << rectStr(abs)
                      << " layer=" << port.layer
                      << " tier=" << tierOf(port.layer) << "\n";
            std::ostringstream det;
            det << "期望" << rectStr(kExp[i]) << " 实际" << rectStr(abs);
            check(abs.xl == kExp[i].xl && abs.yb == kExp[i].yb &&
                      abs.xr == kExp[i].xr && abs.yt == kExp[i].yt,
                  "4.2 端到端 " + comp.name + "." + c.pinName, det.str());
            ++i;
        }
        check(i == 2, "4.2 BOTTOM__l0_b0 两个pin都走完链路");
    }

    // 4.3 vlink0的IO pin坐标: 两个die的vlink应坐标相同(这就是TSV)
    {
        const Net& v = def.nets()[def.netIdx().at("vlink0")];
        const IOPin* bot = nullptr;
        const IOPin* top = nullptr;
        for (const auto& c : v.conns) {
            if (!c.isIOPin) continue;
            const IOPin& io = def.ioPins()[c.ownerIdx];
            if (io.name == "vlink0_bottom") bot = &io;
            if (io.name == "vlink0_top") top = &io;
        }
        check(bot && top, "4.3 vlink0 两个IO pin都在");
        if (bot && top) {
            std::cout << "  bottom(" << bot->layer << ") "
                      << rectStr(bot->absRect) << " / top(" << top->layer
                      << ") " << rectStr(top->absRect) << "\n";
            std::ostringstream det;
            det << rectStr(bot->absRect) << " vs " << rectStr(top->absRect);
            check(bot->absRect.xl == top->absRect.xl &&
                      bot->absRect.yb == top->absRect.yb &&
                      bot->absRect.xr == top->absRect.xr &&
                      bot->absRect.yt == top->absRect.yt,
                  "4.3 bottom/top坐标相同=跨die TSV落点", det.str());
            check(bot->absRect.xl == 46390 && bot->absRect.yt == 72340,
                  "4.3 手算核对 (46390,62340)-(56390,72340)",
                  rectStr(bot->absRect));
            check(bot->layer == "metal10" && top->layer == "metal11",
                  "4.3 层号 metal10/metal11", bot->layer + "/" + top->layer);
        }
    }
}

// ------------------------------------------------------------
// 区块5: 鲁棒性 —— 变异文件测试，程序不许崩
// ------------------------------------------------------------
static void verifyRobustness(const std::string& casePath,
                             const LefParser& lef) {
    section("区块5 鲁棒性(变异DEF)");
    std::cout << "  (解析器输出的 warning 属于预期行为)\n";

    std::string raw;
    if (!readAll(casePath + ".input.def", raw)) {
        check(false, "区块5 读取原DEF失败", casePath + ".input.def");
        return;
    }

    // V1 插入未知关键字行
    {
        std::string s = raw;
        s.insert(s.find("COMPONENTS"), "BOGUS_KEYWORD foo bar ;\n");
        DefParser def;
        bool ok = parseDefFromString(s, def, lef);
        check(ok && def.comps().size() == 218 && def.rows().size() == 78,
              "V1 未知关键字容错, comps仍==218",
              "comps=" + std::to_string(def.comps().size()));
    }

    // V2 某实例朝向改成E
    {
        std::string s = raw;
        size_t pos = s.find("53200 204400 ) FS");
        if (pos != std::string::npos) s.replace(pos + 15, 2, "E");  // FS -> E
        DefParser def;
        parseDefFromString(s, def, lef);
        uint8_t o = def.comps()[def.compIdx().at("u_buf_l0_00_bottom")].orient;
        std::ostringstream det;
        det << "orient=" << orientName(o);
        check(o == ORIENT_E, "V2 朝向改E能识别", det.str());
    }

    // V3 某net加第5个引用
    {
        std::string s = raw;
        const std::string anchor = "( PIN vlink0_top ) + USE SIGNAL";
        size_t pos = s.find(anchor);
        if (pos != std::string::npos)
            s.replace(pos, anchor.size(),
                      "( PIN vlink0_top ) ( u_buf_l0_00_bottom A ) + USE SIGNAL");
        DefParser def;
        parseDefFromString(s, def, lef);
        size_t n = def.netIdx().count("vlink0")
                       ? def.nets()[def.netIdx().at("vlink0")].conns.size()
                       : 0;
        check(n == 5, "V3 vlink0加第5个引用 conns==5", std::to_string(n));
    }

    // V4 制造语法错(缺括号)
    {
        std::string s = raw;
        const std::string broken =
            "( u_buf_l0_01_bottom A )  ( u_buf_l0_00_bottom Z )";
        size_t pos = s.find(broken);
        if (pos != std::string::npos)
            s.replace(pos, broken.size(),
                      "( u_buf_l0_01_bottom A  ( u_buf_l0_00_bottom Z )");
        DefParser def;
        bool ok = parseDefFromString(s, def, lef);
        size_t b0 = def.netIdx().count("BOTTOM__l0_b0")
                        ? def.nets()[def.netIdx().at("BOTTOM__l0_b0")].conns.size()
                        : 0;
        size_t vl = def.netIdx().count("vlink0")
                        ? def.nets()[def.netIdx().at("vlink0")].conns.size()
                        : 0;
        std::ostringstream det;
        det << "nets=" << def.nets().size() << " 破坏网conns=" << b0
            << " vlink0conns=" << vl;
        check(ok && def.nets().size() == 203 && b0 == 1 && vl == 4,
              "V4 缺括号后跳过恢复, 后面net继续", det.str());
    }

    // 空文件
    {
        DefParser def;
        bool ok = parseDefFromString("", def, lef);
        check(ok && def.comps().empty() && def.rows().empty(),
              "空文件不segfault", "comps=0 rows=0");
    }

    // 只有头信息的截断文件
    {
        DefParser def;
        bool ok = parseDefFromString(
            "VERSION 5.8 ;\nDESIGN die_bottom_min ;\n"
            "UNITS DISTANCE MICRONS 2000 ;\nDIEAREA ( 0 0 ) ( 240920 240800 ) ;\n",
            def, lef);
        std::ostringstream det;
        det << "design=" << def.designName()
            << " dieArea=" << rectStr(def.dieArea());
        check(ok && def.designName() == "die_bottom_min" &&
                  def.dieArea().xr == 240920 && def.rows().empty(),
              "截断文件(只有头信息)安全", det.str());
    }
}

// ------------------------------------------------------------
// main: 顺序执行，末尾出总账
// ------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "用法: verify_parser cases/sample001/sample001\n";
        return 1;
    }
    std::string casePath = argv[1];  // 如 cases/sample001/sample001

    verifyTokenizer();  // 区块1: 不需要文件

    LefParser lef;
    if (!lef.parse(casePath + ".input.lef")) {
        std::cout << "[FAIL] LEF 打开/解析失败: " << casePath << ".input.lef\n";
        return 1;
    }
    DefParser def;
    if (!def.parse(casePath + ".input.def", lef.macros(), lef.layers())) {
        std::cout << "[FAIL] DEF 打开/解析失败: " << casePath << ".input.def\n";
        return 1;
    }

    verifyCounts(def, lef);           // 区块2: 总量
    verifyContents(def, lef);         // 区块3: 抽查
    verifyDerived(def, lef);          // 区块4: 复算(最重要)
    verifyRobustness(casePath, lef);  // 区块5: 变异

    // 总账
    std::cout << "\n==============================\n";
    std::cout << "  PASS: " << gPass << "   FAIL: " << gFail << "\n";
    if (!gFailed.empty()) {
        std::cout << "  失败项:\n";
        for (size_t i = 0; i < gFailed.size(); ++i)
            std::cout << "    #" << (i + 1) << " " << gFailed[i] << "\n";
    }
    std::cout << "==============================\n";
    return gFail == 0 ? 0 : 1;
}
