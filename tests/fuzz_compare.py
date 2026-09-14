#!/usr/bin/env python3
import sys

ABS_TOL = 1e-4
REL_TOL = 1e-4

def tol_for(d):
    return max(ABS_TOL, REL_TOL * abs(d))

def parse(path):
    scalar = []
    asm = []
    section = None
    rc_s = rc_a = None
    n_s = n_a = None
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("rc_s="):
                parts = dict(p.split("=") for p in line.split())
                rc_s = int(parts["rc_s"])
                rc_a = int(parts["rc_a"])
                continue
            if line.startswith("n_returned_s="):
                parts = dict(p.split("=") for p in line.split())
                n_s = int(parts["n_returned_s"])
                n_a = int(parts["n_returned_a"])
                continue
            if line == "scalar":
                section = "scalar"
                continue
            if line == "asm":
                section = "asm"
                continue
            if line.startswith("case "):
                continue
            if section == "scalar":
                idx_str, dist_str = line.split()
                scalar.append((int(idx_str), float(dist_str)))
            elif section == "asm":
                idx_str, dist_str = line.split()
                asm.append((int(idx_str), float(dist_str)))
    return rc_s, rc_a, n_s, n_a, scalar, asm

def main():
    if len(sys.argv) != 2:
        print("usage: fuzz_compare.py <case_file>")
        return 2

    rc_s, rc_a, n_s, n_a, scalar, asm = parse(sys.argv[1])

    if rc_s != rc_a:
        print(f"rc mismatch: rc_s={rc_s} rc_a={rc_a}")
        return 1
    if n_s != n_a:
        print(f"n_returned mismatch: {n_s} vs {n_a}")
        return 1
    if len(scalar) != len(asm):
        print(f"length mismatch: {len(scalar)} vs {len(asm)}")
        return 1

    set_s = set(i for i, _ in scalar)
    set_a = set(i for i, _ in asm)
    if set_s != set_a:
        only_s = sorted(set_s - set_a)
        only_a = sorted(set_a - set_s)
        print(f"index set mismatch: only_in_scalar={only_s} only_in_asm={only_a}")
        return 1

    map_s = dict(scalar)
    map_a = dict(asm)
    for idx in map_s:
        ds = map_s[idx]
        da = map_a[idx]
        tol = tol_for(ds)
        if abs(ds - da) > tol:
            print(f"index {idx}: distance mismatch scalar={ds:.9f} asm={da:.9f} diff={abs(ds-da):.3e} tol={tol:.3e}")
            return 1

    for pos, ((is_, ds), (ia, da)) in enumerate(zip(scalar, asm)):
        if is_ != ia:
            d_s_of_s = map_s[is_]
            d_s_of_a = map_s[ia]
            d_a_of_s = map_a[is_]
            d_a_of_a = map_a[ia]
            tol_pair_s = max(tol_for(d_s_of_s), tol_for(d_s_of_a))
            tol_pair_a = max(tol_for(d_a_of_s), tol_for(d_a_of_a))
            if abs(d_s_of_s - d_s_of_a) > tol_pair_s or abs(d_a_of_s - d_a_of_a) > tol_pair_a:
                print(f"pos {pos}: order mismatch not explained by tie: scalar=({is_},{ds:.9f}) asm=({ia},{da:.9f})")
                return 1

    return 0

if __name__ == "__main__":
    sys.exit(main())
