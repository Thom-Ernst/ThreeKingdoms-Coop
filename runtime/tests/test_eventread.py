"""Compile and run the production #56 module/helper offline. Needs an MSVC x64 environment."""
from pathlib import Path
import json
import subprocess
import sys
import argparse

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--release', action='store_true', help='Compile the production module with TW3K_RELEASE')
parser.add_argument('--game-exe', type=Path,
                    default=None)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
state = json.loads((root / 'runtime/tests/fixtures/eventread-october8.json').read_text())
assert len(state['accepted_copies']) == 20
assert [c['mine_read'] for c in state['accepted_copies']] == [True] + [False]*19
lines = []
for f in state['human_registry']:
    lines.append(f"put(registryData+{f['index']*8},uintptr_t({f['faction']}));")
for i, c in enumerate(state['accepted_copies']):
    msg = int(c['address'], 16)
    identity = c['identity_word'] | c['event_index'] << 32
    lines += [f"put(acceptedData+{i*16},uintptr_t({msg}ull));",
              f"put(acceptedData+{i*16+8},uintptr_t({int(c['control'],16)}ull));",
              f"put({msg+0x88}ull,uint64_t({identity}ull));"]
    data = 0x292C73EA020 if c['read_by'] else 0
    lines += [f"put({msg+0x90}ull,eventread::Vector{{{len(c['read_by'])},{len(c['read_by'])},{data}ull}});"]
    for j, faction in enumerate(c['read_by']):
        lines.append(f"put({data+j*8}ull,uintptr_t({faction}));")
    assert msg == int(c['control'],16) + 16
(out / 'eventread_snapshot.inc').write_text('\n'.join(lines))
exe = out / 'eventread_harness.exe'
subprocess.run(['cl', '/nologo', '/EHa', '/std:c++17', '/utf-8',
                *(['/DTW3K_RELEASE'] if args.release else []),
                '/I'+str(root/'runtime/src'), '/I'+str(out),
                str(root/'runtime/tests/eventread_harness.cpp'),
                '/Fe:'+str(exe), '/Fo:'+str(out/'eventread_harness.obj')], check=True)
subprocess.run([str(exe)], check=True)

# Optional external binary verification; the portable production tests above need no game/dump.
if args.game_exe is not None and args.game_exe.exists():
    import pefile
    import capstone
    pe = pefile.PE(str(args.game_exe))
    entry = pe.get_data(0x2F72090, 14)
    assert entry.hex() == '4c8bdc55564157498daba8fdffff'
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    cs.detail = True
    insns = list(cs.disasm(entry, 0x142F72090))
    assert len(insns) == 5 and sum(i.size for i in insns) == 14
    assert all(op.type != capstone.x86.X86_OP_MEM or op.mem.base != capstone.x86.X86_REG_RIP
               for i in insns for op in i.operands)
    assert pe.get_data(0x2F72203, 18).hex() == 'e8086456fe84db740984c074334084ff752e'
    assert pe.get_data(0x2F721E2, 5).hex() == 'e8598756fe'
    print('PASS: executable whole-instruction RIP-free entry and untouched native RequiresResponse clause')
else:
    print('SKIP: external executable byte verification (provide --game-exe to verify a local binary)')
main = (root/'runtime/src/main.cpp').read_text(encoding='utf-8-sig')
probes = (root/'runtime/src/probes.cpp').read_text(encoding='utf-8-sig')
assert main.index('installEventReadHook()') < main.index('installNextAutoOpenHook()')
assert main.count('removeEventReadHook();') == 2
assert 'detourInstall(g_autoOpenDetour' not in probes
assert 'setNextAutoOpenObserver(&nextAutoOpenHook)' in probes
assert 'g_origNextAutoOpen' not in probes
print('PASS: production owner before observer, panic/detach cleanup, diagnostics registration only')
