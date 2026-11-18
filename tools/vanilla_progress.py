#!/usr/bin/env python3
"""原版实现进度燃尽跟踪。

三个互不干扰的口径：

1. 服务端源码燃尽 —— ``vanilla/`` 下已被移除的 .java 文件 / 服务端相关基线。
   ``net/minecraft/client`` 与 ``net/minecraft/realms`` 是纯客户端代码，服务端
   永远不会实现它们，计入分母会让进度永远停在 15% 左右，故单列不计入。
2. 缺口清单燃尽 —— ``docs/vanilla-gaps.md`` 里 P0/P1/P2 的待修、已修、阻塞条目。
   这是"还差什么"的权威清单。
3. 分模块燃尽 —— 按顶层包给出 已实现/总数 与进度条。

基线由 ``tools/vanilla_baseline.txt`` 提供（git 历史里出现过、现已删除的文件
加上当前仍存在的文件，共 2185 条）。缺失时回退到用 git 现算。
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VANILLA_DIR = os.path.join(ROOT, "vanilla")
BASELINE_FILE = os.path.join(ROOT, "tools", "vanilla_baseline.txt")
GAPS_FILE = os.path.join(ROOT, "docs", "vanilla-gaps.md")

# 纯客户端包：不计入服务端进度
CLIENT_PREFIXES = (
    os.path.join("vanilla", "net", "minecraft", "client") + os.sep,
    os.path.join("vanilla", "net", "minecraft", "realms") + os.sep,
)

BAR_WIDTH = 24


def _rel(path: str) -> str:
    return os.path.relpath(path, ROOT).replace(os.sep, "/")


def load_baseline() -> list[str]:
    if os.path.exists(BASELINE_FILE):
        with open(BASELINE_FILE, encoding="utf-8") as handle:
            return [line.strip() for line in handle if line.strip()]
    # 回退：直接从 git 现算（需要完整历史）
    try:
        deleted = subprocess.run(
            ["git", "log", "--diff-filter=D", "--name-only", "--pretty=format:",
             "--", "vanilla/**.java"],
            cwd=ROOT, capture_output=True, text=True, check=True,
        ).stdout.splitlines()
        present = subprocess.run(
            ["git", "ls-files", "vanilla/**.java"],
            cwd=ROOT, capture_output=True, text=True, check=True,
        ).stdout.splitlines()
    except (OSError, subprocess.CalledProcessError):
        return []
    return sorted({p for p in deleted + present if p.endswith(".java")})


def scan_present() -> set[str]:
    found: set[str] = set()
    for dirpath, _, files in os.walk(VANILLA_DIR):
        for name in files:
            if name.endswith(".java"):
                found.add(_rel(os.path.join(dirpath, name)))
    return found


def top_package(path: str) -> str:
    parts = path.split("/")
    # vanilla/net/minecraft/<pkg>/...
    return parts[3] if len(parts) > 4 else "root"


def is_client(path: str) -> bool:
    return any(path.startswith(prefix.replace(os.sep, "/")) for prefix in CLIENT_PREFIXES)


def bar(ratio: float) -> str:
    filled = int(round(ratio * BAR_WIDTH))
    return "█" * filled + "░" * (BAR_WIDTH - filled)


def parse_gaps() -> dict:
    """解析 docs/vanilla-gaps.md 的状态表，返回按优先级分组的计数。"""
    result = {
        "p0": {"todo": 0, "done": 0, "blocked": 0},
        "p1": {"todo": 0, "done": 0, "blocked": 0},
        "p2": {"todo": 0, "done": 0, "blocked": 0},
        # “已修”表不区分优先级，只累计总数
        "done_total": 0,
    }
    if not os.path.exists(GAPS_FILE):
        return result

    section = None
    with open(GAPS_FILE, encoding="utf-8") as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            if line.startswith("## "):
                if "P0" in line:
                    section = "p0"
                elif "P1" in line:
                    section = "p1"
                elif "P2" in line:
                    section = "p2"
                elif "已修" in line:
                    section = "done_table"
                elif "整类未实现" in line or "修复顺序" in line:
                    section = None
                continue
            if section is None or not line.startswith("| "):
                continue
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if len(cells) < 2:
                continue
            first = cells[0]
            # 条目 ID 形如 PREFIX-12 或 PREFIX-12a
            if not (first and first[0].isalpha() and "-" in first):
                continue
            status = cells[-1]
            if section == "done_table":
                result["done_total"] += 1
            elif status.startswith("已修"):
                result[section]["done"] += 1
            elif "阻塞" in status or status.startswith("部分"):
                result[section]["blocked"] += 1
            elif status.startswith("待修"):
                result[section]["todo"] += 1
    return result


def collect() -> dict:
    baseline = load_baseline()
    present = scan_present()

    server_baseline = [p for p in baseline if not is_client(p)]
    server_present = {p for p in present if not is_client(p)}
    client_baseline = [p for p in baseline if is_client(p)]
    client_present = {p for p in present if is_client(p)}

    def progress(total: list[str], still: set[str]) -> dict:
        total_set = set(total)
        remaining = total_set & still
        done = len(total_set) - len(remaining)
        return {
            "total": len(total_set),
            "done": done,
            "remaining": len(remaining),
            "ratio": done / len(total_set) if total_set else 0.0,
        }

    packages: dict[str, dict] = defaultdict(lambda: {"total": 0, "remaining": 0})
    server_total_set = set(server_baseline)
    for path in server_total_set:
        key = top_package(path)
        packages[key]["total"] += 1
        if path in server_present:
            packages[key]["remaining"] += 1

    pkg_rows = []
    for name, stat in packages.items():
        done = stat["total"] - stat["remaining"]
        pkg_rows.append({
            "package": name,
            "total": stat["total"],
            "done": done,
            "remaining": stat["remaining"],
            "ratio": done / stat["total"] if stat["total"] else 0.0,
        })
    pkg_rows.sort(key=lambda row: (-row["remaining"], row["package"]))

    return {
        "server": progress(server_baseline, server_present),
        "client": progress(client_baseline, client_present),
        "packages": pkg_rows,
        "gaps": parse_gaps(),
        "baseline_total": len(baseline),
    }


def render(data: dict, top: int) -> None:
    server = data["server"]
    print("=" * 66)
    print("            CyaneServer 原版实现进度燃尽跟踪")
    print("=" * 66)

    print(f"服务端基线: {server['total']} 个原版服务端源文件"
          f"（全量 {data['baseline_total']}，客户端包已剔除）")
    print(f"  已实现:   {server['done']:>5} ({server['ratio'] * 100:5.1f}%) "
          f"{bar(server['ratio'])}")
    print(f"  待实现:   {server['remaining']:>5} ({(1 - server['ratio']) * 100:5.1f}%)")

    client = data["client"]
    print(f"客户端包: {client['total']} 个文件，服务端不实现，不计入分母")

    gaps = data["gaps"]
    todo = sum(gaps[k]["todo"] for k in ("p0", "p1", "p2"))
    done = gaps["done_total"] + sum(gaps[k]["done"] for k in ("p0", "p1", "p2"))
    blocked = sum(gaps[k]["blocked"] for k in ("p0", "p1", "p2"))
    total_gaps = todo + done + blocked
    print("-" * 66)
    print(f"缺口清单: {total_gaps} 条（已修 {done} / 待修 {todo} / 阻塞 {blocked}）")
    if total_gaps:
        print(f"  修复进度: {bar(done / total_gaps)} {done / total_gaps * 100:5.1f}%")
    for key, label in (("p0", "P0 数据丢失/可见破缺"),
                       ("p1", "P1 机制不一致    "),
                       ("p2", "P2 数值/文本差异  ")):
        stat = gaps[key]
        count = stat["todo"] + stat["done"] + stat["blocked"]
        extra = f"  阻塞 {stat['blocked']:>2}" if stat["blocked"] else ""
        print(f"  {label}  共 {count:>2}  待修 {stat['todo']:>2}{extra}")

    print("-" * 66)
    print(f"分模块燃尽（按待实现数排序，前 {top}）：")
    print(f"  {'包':<22}{'已完成/总数':>12}  进度")
    for row in data["packages"][:top]:
        print(f"  net.minecraft.{row['package']:<20}"
              f"{row['done']:>5}/{row['total']:<6}  {bar(row['ratio'])}")
    print("-" * 66)
    print("注：cyane 的生物/方块/物品是数据驱动实现（mob_types/blocks 表），")
    print("    entity/block/item 等包的按文件计数会显著低估实际覆盖度；")
    print("    服务端进度应以缺口清单为准，文件燃尽只反映结构对齐程度。")
    print("=" * 66)


def main() -> int:
    parser = argparse.ArgumentParser(description="原版实现进度燃尽跟踪")
    parser.add_argument("--json", action="store_true", help="输出机器可读 JSON")
    parser.add_argument("--top", type=int, default=12, help="分模块显示条数（默认 12）")
    parser.add_argument("--strict", action="store_true",
                        help="存在 P0 待修时以退出码 1 结束（供 CI 使用）")
    args = parser.parse_args()

    if not os.path.isdir(VANILLA_DIR):
        print(f"找不到 {VANILLA_DIR}", file=sys.stderr)
        return 1

    data = collect()
    if args.json:
        print(json.dumps(data, ensure_ascii=False, indent=2))
    else:
        render(data, args.top)

    if args.strict and data["gaps"]["p0"]["todo"] > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
