"""Use the actual enum control methods; substitute only its window/widget services."""
from pathlib import Path
import argparse
from extract_controller_ui_fixture import method


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    header = args.source.with_suffix('.h').read_text(encoding='utf-8')
    start, end = 'class ConfigOptionEnum :', 'class ConfigOptionNumber :'
    assert header.count(start) == header.count(end) == 1
    declarations = header[header.index(start):header.index(end)]
    signatures = ['ConfigOptionEnum::ConfigOptionEnum('] + [
        'void ConfigOptionEnum::' + name + '('
        for name in ('update_value', 'update_disabled', 'update_enum_details', 'update_enum_disabled')]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(declarations + '\n' + '\n\n'.join(method(source, name) for name in signatures), encoding='utf-8')


if __name__ == '__main__':
    main()
