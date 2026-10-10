import tse_framework_pybind_def as def_pybind
import tse_framework_pybind_lef as lef_pybind
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path
import re

@dataclass(frozen=True)
class DieArea:
    x1: float
    y1: float
    x2: float
    y2: float

    @property
    def width(self) -> float:
        return self.x2 - self.x1

    @property
    def height(self) -> float:
        return self.y2 - self.y1

GuideRect = tuple[int, int, int, int, str]

def rect_contains_point(rect : GuideRect, point) -> bool:
        x, y = point
        return (rect[0] <= x <= rect[2] or rect[2] <= x <= rect[0]) and \
               (rect[1] <= y <= rect[3] or rect[3] <= y <= rect[1])

def rectangles_connect(rect1 : GuideRect, rect2 : GuideRect, tolerance=1e-6):
    """
    判断两个矩形是否相交（包括边重合、部分重叠、完全包含）。
    只要它们在 X 和 Y 方向上的投影有重叠（考虑容差），就判定为连接。
    """
    # 提取矩形坐标，确保 x1 <= x2, y1 <= y2
    x1_1, y1_1, x2_1, y2_1 = rect1[0], rect1[1], rect1[2], rect1[3]
    x1_2, y1_2, x2_2, y2_2 = rect2[0], rect2[1], rect2[2], rect2[3]

    # 归一化：确保 x1 <= x2, y1 <= y2
    if x1_1 > x2_1: x1_1, x2_1 = x2_1, x1_1
    if y1_1 > y2_1: y1_1, y2_1 = y2_1, y1_1
    if x1_2 > x2_2: x1_2, x2_2 = x2_2, x1_2
    if y1_2 > y2_2: y1_2, y2_2 = y2_2, y1_2

    # 检查 X 方向是否有重叠（考虑容差）
    x_overlap = (x1_1 <= x2_2 + tolerance) and (x1_2 <= x2_1 + tolerance)
    # 检查 Y 方向是否有重叠（考虑容差）
    y_overlap = (y1_1 <= y2_2 + tolerance) and (y1_2 <= y2_1 + tolerance)

    return x_overlap and y_overlap

def build_adjacency_list(rectangles : list[GuideRect]):
    """
    构建矩形邻接表，用于快速查找邻居。
    时间复杂度O(n^2)，可优化，但为清晰起见保持简洁。
    """
    adjacency = {i: [] for i in range(len(rectangles))}
    for i in range(len(rectangles)):
        for j in range(len(rectangles)):
            if i == j:
                continue

            if rectangles_connect(rectangles[i], rectangles[j]):
                adjacency[i].append(j)
                adjacency[j].append(i)

    return adjacency


def dfs_find_path(adjacency, visited, current, target_index, path):
    """
    深度优先搜索，递归查找从current到target_index的路径。
    """
    if current == target_index:
        path.append(current)
        return True

    visited.add(current)
    for neighbor in adjacency[current]:
        if neighbor not in visited:
            # 回溯的关键：先尝试，如果成功则返回，否则继续尝试其他邻居
            if dfs_find_path(adjacency, visited, neighbor, target_index, path):
                path.append(current)
                return True
    # 如果没有找到，取消当前节点的路径记录，回溯到上一层
    return False

def is_net_connected_with_dfs(rectangles : list[GuideRect], pin1_coord : list, pin2_coord : list, adjacency):
    if not rectangles or not pin1_coord or not pin2_coord:
        return False

    start_indices = [i for i, r in enumerate(rectangles) if rect_contains_point(r, pin1_coord)]
    target_indices = [i for i, r in enumerate(rectangles) if rect_contains_point(r, pin2_coord)]

    if not start_indices or not target_indices:
        return False

    for start in start_indices:
        for target in target_indices:
            visited = set()
            path = []
            if dfs_find_path(adjacency, visited, start, target, path):
                return True

    return False

def get_position(connection : def_pybind.NetConnection, lef_definition : lef_pybind.LefDefinition, def_definition, dbu):
    if connection.instance == "PIN":
        pin = def_pybind.Pin()
        exists = def_definition.GetIoPin(connection.pin, pin)
        if not exists:
            raise ValueError(f"No pin definition: {connection.pin}")
        pin_port = pin.pin_ports[0]
        return [pin_port.x, pin_port.y]
    else:
        component = next((component for component in def_definition.getComponents() if component.instance_name == connection.instance), None)
        if component is None:
            raise ValueError(f"Not found component for instance: {connection.instance}")
    
        macro = lef_definition.GetMacroByName(component.macro_name)
        pin = macro.pins[connection.pin]
        port = pin.ports[0].geometries[0].rect[0]
        x = (port.xl + port.xh) / 2
        y = (port.yl + port.yh) / 2
    
        return [component.x + x * dbu, component.y + y * dbu]


def check_net_connection(guide_nets : dict[str, list[GuideRect]], def_definition : def_pybind.DefDefinition, 
                         lef_definition : lef_pybind.LefDefinition):
    nets_not_routed = 0
    net_name_not_routed = []
    pins_not_connected = 0
    pin_name_not_connected = []
    dbu = def_definition.GetDatabaseUnits()
    for net in def_definition.getNets():
        if net.name not in guide_nets.keys():
            net_name_not_routed.append(net.name)
            nets_not_routed += 1

        net_rectangles = guide_nets[net.name]
        adjacency = build_adjacency_list(net_rectangles)
        source_connection = net.connections[0]
        source_position = get_position(source_connection, lef_definition, def_definition, dbu)
        for i in range(1, len(net.connections)):
            target_connection = net.connections[i]
            target_position = get_position(target_connection, lef_definition, def_definition, dbu)
            if not is_net_connected_with_dfs(net_rectangles, source_position, target_position, adjacency):
                pins_not_connected += 1
                pin_name_not_connected.append(f'{source_connection.instance}.{source_connection.pin} <---> {target_connection.instance}.{target_connection.pin}')

    if nets_not_routed > 0 or pins_not_connected > 0:
        raise ValueError("nets not routed: " + ", ".join(net_name_not_routed) + "\n" + "pins not connected: " + ", ".join(pin_name_not_connected))


def check_crossing_border(guide_nets : dict[str, list[GuideRect]], def_definition : def_pybind.DefDefinition):
    cross_nets = 0
    cross_net_name = []
    die_area = def_definition.getDieArea()
    for net_name, rectangles in guide_nets.items():
        for rect in rectangles:
            if rect[0] < die_area.xl or rect[0] > die_area.xh or rect[1] < die_area.yl or rect[1] > die_area.yh \
               or rect[2] < die_area.xl or rect[2] > die_area.xh or rect[3] < die_area.yl or rect[3] > die_area.yh:
               cross_nets += 1
               cross_net_name.append(net_name)

    if cross_nets > 0:
        raise ValueError(f"crossing border nets: " + ", ".join(cross_net_name))


def parse_layer_num(layer_name: str) -> int:
    match = re.fullmatch(r"(?:Metal|M)(\d+)", layer_name, flags=re.IGNORECASE)
    return int(match.group(1)) if match else 0

def parse_guide(path: Path) -> dict[str, list[GuideRect]]:
    nets: dict[str, list[GuideRect]] = defaultdict(list)
    current: str | None = None
    for number, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        line = raw.strip()
        if not line or line == "(":
            continue
        if line == ")":
            current = None
            continue
        fields = line.split()
        if len(fields) == 5 and parse_layer_num(fields[4]):
            if current is None:
                raise ValueError(f"{path}:{number}: guide rectangle before net name")
            x1, y1, x2, y2 = map(int, fields[:4])
            nets[current].append(
                (min(x1, x2), min(y1, y2), max(x1, x2), max(y1, y2), fields[4])
            )
        else:
            current = line
    if not nets:
        raise ValueError(f"{path}: no guide nets")
    return dict(nets)


def check(guide_nets : dict[str, list[GuideRect]], def_definition : def_pybind.DefDefinition, 
        lef_definition : lef_pybind.LefDefinition):
    check_crossing_border(guide_nets, def_definition)
    check_net_connection(guide_nets, def_definition, lef_definition)


if __name__ == "__main__":
    try:
        def_definition = def_pybind.DefParser().Parse("cases/sample001/sample001.input.def")
        lef_definition = lef_pybind.LefParser().Parse(["cases/sample001/sample001.input.lef"])
        guide_nets = parse_guide(Path("cases/sample001/sample001.output.guide"))
        check(guide_nets, def_definition, lef_definition)
    except (OSError, ValueError) as error:
        print(str(error))
    