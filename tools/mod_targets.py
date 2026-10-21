"""The game functions mod routines may hook: upstream's tools/prepare_mod_targets.py, with the port's own sources
scanned as well, so a function the port replaces (the health setter, for invincibility) is left out like
upstream's own replacements are.

mod_targets.py --upstream <checkout> --mapping <x86_func_mapping.cpp> --output <header> <source folder>...
"""
import argparse
import importlib.util
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--upstream', required=True, type=Path)
    parser.add_argument('--mapping', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('roots', nargs='+', type=Path)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('prepare_mod_targets', args.upstream / 'tools/prepare_mod_targets.py')
    upstream = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(upstream)
    addresses = upstream.targets(args.mapping, args.upstream / 'config/game.toml', args.roots)
    text = '#pragma once\n#include <array>\n#include <cstdint>\nnamespace shaolin::mods {\n'
    text += f'inline constexpr std::array<uint32_t,{len(addresses)}> RoutineTargets{{\n'
    text += ''.join(f'0x{address:08X}u,\n' for address in addresses) + '};\n}\n'
    if not args.output.exists() or args.output.read_text(encoding='utf-8') != text:
        args.output.write_text(text, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
