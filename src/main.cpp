#include "parser/DefParser.h"
#include "parser/LefParser.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string casePath = argc > 1 ? argv[1] : "cases/sample001/sample001";

    thdic::parser::LefParser lef;  // 第 1 步：LEF 建 MACRO/LAYER/VIA 表
    if (!lef.parse(casePath + ".input.lef")) return 1;

    thdic::parser::DefParser def;  // 第 2 步：DEF 查表解析
    if (!def.parse(casePath + ".input.def", lef.macros(), lef.layers())) return 1;

    std::cout << "design   : " << def.designName() << "\n";
    std::cout << "dbu      : " << def.dbu() << "\n";
    std::cout << "dieArea  : (" << def.dieArea().xl << ", " << def.dieArea().yb
              << ") - (" << def.dieArea().xr << ", " << def.dieArea().yt << ")\n";
    std::cout << "rows     : " << def.rows().size() << "\n";
    std::cout << "comps    : " << def.comps().size() << "\n";
    std::cout << "ioPins   : " << def.ioPins().size() << "\n";
    std::cout << "nets     : " << def.nets().size() << "\n";
    std::cout << "tracks   : " << def.tracks().size() << " layers\n";
    std::cout << "gcell    : " << def.gcellX().size() << " x " << def.gcellY().size()
              << " lines\n";
    std::cout << "layers   : " << lef.layers().size() << "\n";
    std::cout << "viaDefs  : " << lef.viaDefs().size() << "\n";
    std::cout << "viaAdj   : " << lef.viaAdj().size() << " pairs\n";
    std::cout << "crossDie : " << lef.crossDieVias().size() << "\n";
    std::cout << "macros   : " << lef.macros().size() << "\n";
    // 第 3 步（DesignDatabase）/ 第 4 步（布线）后续接入。
    return 0;
}
