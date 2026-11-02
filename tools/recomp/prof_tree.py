"""Print the profiler's call tree from profile_edges.txt (written next to profile.txt by profile builds).
usage: prof_tree.py <profile_edges.txt> [root address] [min incl %]"""
import sys

edges = {}
for line in open(sys.argv[1]):
    if line.startswith('#'):
        continue
    pct, a, b = line.split()
    edges.setdefault(a, []).append((float(pct), b))
callees = {b for kids in edges.values() for _, b in kids}
root = sys.argv[2] if len(sys.argv) > 2 else next(a for a in edges if a not in callees)
cut = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0


def show(a, pct, depth, path):
    print('%s%5.1f%% %s' % ('  ' * depth, pct, a))
    if a in path:  # recursion
        return
    for p, b in sorted(edges.get(a, []), reverse=True):
        if p >= cut:
            show(b, p, depth + 1, path | {a})


show(root, 100.0, 0, frozenset())
