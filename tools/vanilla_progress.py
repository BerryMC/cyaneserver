#!/usr/bin/env python3
"""原版源码实现进度燃尽统计工具。"""
import os
import sys

VANILLA_DIR = os.path.join(os.path.dirname(__file__), "..", "vanilla", "net", "minecraft")
TOTAL_ORIGINAL = 2185

def main():
    if not os.path.exists(VANILLA_DIR):
        print(f"Directory {VANILLA_DIR} not found.")
        return 1

    remaining = 0
    package_counts = {}
    for root, _, files in os.walk(VANILLA_DIR):
        for f in files:
            if f.endswith(".java"):
                remaining += 1
                rel_pkg = os.path.relpath(root, VANILLA_DIR)
                pkg = rel_pkg.replace(os.sep, ".") if rel_pkg != "." else "root"
                top_pkg = pkg.split(".")[0]
                package_counts[top_pkg] = package_counts.get(top_pkg, 0) + 1

    completed = TOTAL_ORIGINAL - remaining
    pct = (completed / TOTAL_ORIGINAL) * 100.0

    print("========================================")
    print("      CyaneServer 原版进度燃尽跟踪      ")
    print("========================================")
    print(f"总计基准文件数: {TOTAL_ORIGINAL}")
    print(f"已完成/已移除:   {completed} ({pct:.1f}%)")
    print(f"剩余待实现:     {remaining} ({100.0 - pct:.1f}%)")
    print("----------------------------------------")
    print("按顶层包统计待实现文件数:")
    for pkg, count in sorted(package_counts.items(), key=lambda x: -x[1]):
        print(f"  net.minecraft.{pkg:<20} {count:>4} 文件")
    print("========================================")
    return 0

if __name__ == "__main__":
    sys.exit(main())
