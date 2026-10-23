#!/usr/bin/env python3
"""Resolve phandles in a decompiled live device tree (dtc -I dtb -O dts).

  dts-resolve.py LIVE.dts NODE_REGEX [PROP ...]

Prints the node's named properties, and for every phandle-valued property
(clocks, resets, *-supply, *-gpio, power-domains, ...) the referenced node's
path and its label-ish properties (regulator-name, qcom,resource-name,
compatible), so facts can be cited from the real board DT, not guessed.
"""
import re
import sys


def parse(path):
    nodes, stack = {}, []
    for line in open(path, encoding='utf-8', errors='replace'):
        s = line.strip()
        m = re.match(r'^(?:[\w,.-]+:\s*)?([\w,.@+-]+|/)\s*\{$', s)
        if m:
            stack.append(m.group(1))
            nodes.setdefault('/'.join(stack), {})
            continue
        if s == '};':
            stack.pop()
            continue
        m = re.match(r'^([#\w,.+-]+)\s*=\s*(.*);$', s)
        if m and stack:
            nodes['/'.join(stack)][m.group(1)] = m.group(2)
    by_phandle = {}
    for path, props in nodes.items():
        if 'phandle' in props:
            by_phandle[int(props['phandle'].strip('<>'), 0)] = path
    return nodes, by_phandle


def cells(v):
    m = re.match(r'^<(.*)>$', v.strip())
    return [int(x, 0) for x in m.group(1).split()] if m else None


def describe(nodes, path):
    p = nodes.get(path, {})
    keys = ('regulator-name', 'qcom,resource-name', 'compatible', 'label')
    return path + ''.join(f'  {k}={p[k]}' for k in keys if k in p)


def main():
    dts, node_re = sys.argv[1], re.compile(sys.argv[2])
    want = sys.argv[3:]
    nodes, by_ph = parse(dts)
    for path, props in nodes.items():
        if not node_re.search(path):
            continue
        print(f'== {path}')
        for k, v in props.items():
            if want and k not in want:
                continue
            print(f'  {k} = {v[:200]}')
            c = cells(v)
            if c and (k.endswith(('-supply', '-gpio', '-gpios')) or k in ('clocks', 'resets', 'power-domains')):
                i = 0
                while i < len(c):
                    ref = by_ph.get(c[i])
                    if ref:
                        args = c[i + 1:i + 3] if k.endswith(('-gpio', '-gpios')) else c[i + 1:i + 2]
                        if k.endswith('-supply'):
                            args = []
                        print(f'      -> {describe(nodes, ref)}  args={args}')
                        i += 1 + len(args)
                    else:
                        i += 1


if __name__ == '__main__':
    main()
