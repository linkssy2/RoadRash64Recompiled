"""Exercise the actual startup boundary without creating graphics or guest threads."""
from pathlib import Path
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('runtime', type=Path)
parser.add_argument('course', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
runtime = args.runtime.read_text()
course = args.course.read_text()


def function(source, signature):
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2] + '\n'


loader = function(course, 'void load_pack(const std::filesystem::path &configured)')
marker = '    const auto rom = recomp::get_rom();'
assert loader.count(marker) == 1
# The ROM/geometry tail is simulated so failures after installing partial assets
# can be injected. Catalogue parsing and every production cleanup call stay real.
loader = loader[:loader.index(marker)] + '    fixture_install_assets();\n}\n'
parts = [function(course, signature) for signature in (
    '[[noreturn]] void fail(const std::string &message)',
    'void require(bool ok, const char *message)',
    'Bytes read(const std::filesystem::path &path, std::size_t maximum)',
    'void reset_assets()',
)]
parts += [loader, function(course, 'void initialize()')]
output = 'namespace rr64::experimental_course {\n' + '\n'.join(parts) + '}\n'
output += function(runtime, 'bool wait_for_game_started(uint8_t* rdram, recomp_context* context)')
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(output)
print('Extracted common startup, catalogue validation, session cleanup and retry paths.')
