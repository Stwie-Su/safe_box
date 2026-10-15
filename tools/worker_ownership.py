#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""worker_post 的 arg 所有权约定检查。

约定（worker.c 实现）：
  - done == NULL（Fire-and-Forget）：框架在 poll 时 free(arg) + free(job)；
  - done != NULL：回调**必须**自己释放 arg，框架只释放 job 节点。
因此：done 回调里没有 free(arg) => 内存泄漏；arg 是栈/静态地址却 free => 崩溃。

用法: worker_ownership.py <app_dir>
输出: 每个调用点的 [文件:行] fn / arg / done / 回调是否释放 / arg 是否堆分配
"""
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "/home/book/桌面/lv_port_linux/app"

SRC = {}
for dp, _, fs in os.walk(ROOT):
    if "/build" in dp:
        continue
    for f in fs:
        if f.endswith(".c"):
            p = os.path.join(dp, f)
            SRC[p] = open(p, encoding="utf-8", errors="replace").read()

ALL = "\n".join(SRC.values())

CALL = re.compile(
    r"worker_post\s*\(\s*(\w+)\s*,\s*([A-Za-z_]\w*|NULL)\s*,\s*([A-Za-z_]\w*|NULL)\s*\)")


def func_body(name):
    """提取函数定义体（从 '<name>(' 起，按大括号配对）。"""
    m = re.search(r"[^\n]*\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", ALL)
    if not m:
        return None
    i = ALL.index("{", m.start())
    depth = 0
    for j in range(i, len(ALL)):
        if ALL[j] == "{":
            depth += 1
        elif ALL[j] == "}":
            depth -= 1
            if depth == 0:
                return ALL[i:j + 1]
    return None


def alloc_kind(path, arg):
    """在调用点所在文件里，判断 arg 是怎么来的。"""
    s = SRC[path]
    m = re.search(r"\b" + re.escape(arg) + r"\s*=\s*(calloc|malloc|realloc)\s*\(", s)
    if m:
        return "HEAP"
    if re.search(r"\b" + re.escape(arg) + r"\s*=\s*&\w+", s):
        return "ADDR_OF(可能栈)"
    return "UNKNOWN"


rows = []
for path, s in SRC.items():
    if "core/support/worker.c" in path:
        continue
    for m in CALL.finditer(s):
        fn, arg, done = m.group(1), m.group(2), m.group(3)
        line = s[: m.start()].count("\n") + 1
        if done == "NULL":
            verdict = "OK(框架释放)"
        else:
            body = func_body(done)
            if body is None:
                verdict = "??回调未找到"
            elif "free(" in body:
                verdict = "OK(回调释放)"
            else:
                verdict = "⚠回调未释放arg"
        kind = "NULL" if arg == "NULL" else alloc_kind(path, arg)
        if kind == "ADDR_OF(可能栈)" and done != "NULL":
            verdict += " ⚠栈地址被回调free"
        rows.append((os.path.relpath(path, ROOT), line, fn, arg, done, kind, verdict))

rows.sort(key=lambda r: (r[6].startswith("OK"), r[0]))
print("%-34s %5s %-22s %-10s %-20s %-14s %s" % ("文件", "行", "fn", "arg", "done", "arg来源", "判定"))
for r in rows:
    print("%-34s %5d %-22s %-10s %-20s %-14s %s" % r)
bad = [r for r in rows if not r[6].startswith("OK")]
print("\nTOTAL=%d  可疑=%d" % (len(rows), len(bad)))

