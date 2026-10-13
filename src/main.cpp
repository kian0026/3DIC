#include "parser/DefParser.h"
#include "parser/DesignDatabase.h"
#include "parser/LefParser.h"
#include "parser/ThermalParser.h"

#include <iostream>
#include <vector>
#include <string>

int main(int argc, char** argv) {
    std::string casePath = argc > 1 ? argv[1] : "cases/sample001/sample001";

    thdic::parser::LefParser lef;  // 第 1 步：LEF 建 MACRO/LAYER/VIA 表
    if (!lef.parse(casePath + ".input.lef")) return 1;

    thdic::parser::DefParser def;  // 第 2 步：DEF 查表解析
    if (!def.parse(casePath + ".input.def", lef.macros(), lef.layers())) return 1;

    thdic::parser::ThermalParser thermal;  // 第 3 步：温度表(零依赖)
    if (!thermal.parse(casePath + ".thermal.input.json")) return 1;

    // 第 4 步：与 DEF 对账(独立于解析的一步)
    bool thermalOk = false;
    if (thermal.format() == thdic::parser::ThermalFormat::Series) {
        thermalOk = thermal.reconcile(def.compIdx());
    } else {
        std::vector<thdic::parser::CompLoc> locs;
        locs.reserve(def.comps().size());
        for (const auto& c : def.comps()) locs.push_back({c.name, c.x, c.y});
        thermalOk = thermal.reconcile(locs, def.dieArea().xl, def.dieArea().yb,
                                      def.dbu());
    }

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
    std::cout << "thermal  : format="
              << (thermal.format() == thdic::parser::ThermalFormat::Grid
                      ? "GRID"
                      : "SERIES")
              << " unit=" << thermal.unit()
              << " default=" << thermal.defaultTemp() << "\n";
    if (thermal.format() == thdic::parser::ThermalFormat::Grid) {
        std::cout << "           grid=" << thermal.gridCols() << "x"
                  << thermal.gridRows() << "x" << thermal.dieCount()
                  << " probes=" << thermal.gridNodes().size()
                  << " ambient=" << thermal.ambientTemp() << "\n";
    } else {
        std::cout << "           nodes=" << thermal.nodes().size()
                  << " steps=" << thermal.numTimeSteps() << "\n";
    }
    std::cout << "reconcile: " << (thermalOk ? "OK" : "WITH WARNINGS")
              << " (missing=" << thermal.lastMissing()
              << ", orphans=" << thermal.lastOrphans() << ")\n";

    thdic::parser::DesignDatabase db;  // 第 5 步：网格 + 端点 + 温度
    if (!db.build(lef, def, thermal)) {
        std::cout << "designDB : 自检未通过, 不进入布线\n";
        return 1;
    }
    const thdic::parser::DesignStats& st = db.stats();
    std::cout << "-- 端点表 -- nets=" << st.nets
              << " endpoints=" << st.endpoints
              << " (IO=" << st.ioEndpoints << " inst=" << st.instEndpoints
              << " 越界入余量=" << st.outOfDieEndpoints
              << ") avg=" << (st.nets ? static_cast<double>(st.endpoints) /
                                            st.nets
                                      : 0.0)
              << "\n";
    std::cout << "-- 容量表 -- grid=" << st.cols << "x" << st.rows << "x"
              << st.layers << " cells=" << st.cells
              << " blocked=" << st.blockedCells << " cap min/avg/max="
              << st.capMin << "/" << st.capAvg << "/" << st.capMax << "\n";
    std::cout << "-- 代价表 -- unitMove=" << st.unitMoveDbu
              << " dbu via=" << st.viaCostDbu << " tsv=" << st.tsvCostDbu
              << "\n";
    std::cout << "-- 温度表 -- probes=" << st.probes << " min/avg/max="
              << st.tMin << "/" << st.tAvg << "/" << st.tMax
              << " default=" << st.tDefault << " thermalGrid="
              << st.thermalCols << "x" << st.thermalRows << "\n";
    // 第 6 步（布线）后续接入。
    return 0;
}
