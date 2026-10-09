"""Synthetic regression tests; no game, captures, network or private tooling."""
from pathlib import Path
import subprocess
import sys

runtime = Path(__file__).resolve().parents[1]
out = runtime / 'tests/output'
out.mkdir(parents=True, exist_ok=True)
for name in ('test_ui', 'test_lobby_panels', 'test_savelobby_watch'):
    subprocess.run(['cl', '/nologo', '/EHa', '/std:c++17', '/utf-8',
                    'tests/' + name + '.cpp', '/Fe:tests/output/' + name + '.exe',
                    '/Fo:tests/output/' + name + '.obj', '/link', 'user32.lib'],
                   cwd=runtime, check=True)
    args = [str(out / (name + '.exe'))]
    if name == 'test_ui':
        args.append(str(out))
    subprocess.run(args, cwd=runtime, check=True)
for name in ('test_audit.py', 'test_audit_lifecycle.py'):
    subprocess.run([sys.executable, str(runtime / 'tests' / name), str(out)],
                   cwd=runtime, check=True)
for mode in ([], ['--release']):
    subprocess.run([sys.executable, str(runtime / 'tests/test_eventread.py'),
                    str(out / ('eventread-release' if mode else 'eventread-debug')), *mode],
                   cwd=runtime, check=True)
print('ok all offline runtime regressions, including event-read containment in both modes')
