#!/usr/bin/env python3
"""test_sertest.py - tools/sertest.py against SERTEST in its PC simulator (build/sertest/sim, make sertest-sim):
twice, with a line waking sys_sleep() and without (the sim's SIM_WAKE_ON_LINE); every item must be PASS / MEASURED /
INFO / SKIP, and UNKNOWN 1 must come out as the sim was set. Then --manual with scripted answers (no cable and no real
key in the sim: T3 and K11r may fail there).   python3 test/test_sertest.py"""
import os, re, subprocess, sys, tempfile, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
bad = 0


def run(env, manual=False):
    out = tempfile.mkdtemp()
    sim = subprocess.Popen([os.path.join(ROOT, 'build', 'sertest', 'sim')], cwd=out, env=dict(os.environ, **env),
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    tty = sim.stdout.readline().strip()
    args = [sys.executable, os.path.join(ROOT, 'tools', 'sertest.py'), tty, '--out', out] + (['--manual'] if manual else [])
    r = subprocess.run(args, input='\n\ny\n', text=True, capture_output=True, timeout=600)
    sim.kill()
    return r.stdout


def check(name, cond):
    global bad
    bad += not cond
    print('  %s  %s' % ('ok  ' if cond else 'FAIL', name))


for wake in ('0', '1'):
    t = time.time()
    log = run({'SIM_WAKE_ON_LINE': wake})
    fails = re.findall(r'\[FAIL\s*\] (\S+)', log)
    check('SIM_WAKE_ON_LINE=%s: no FAIL (%s) %.0f s' % (wake, ' '.join(fails), time.time() - t), not fails)
    check('  UNKNOWN 1 as the sim', ('a line WAKES' if wake == '1' else 'a line does NOT wake') in log)
    check('  the summary has 57 items', len(re.findall(r'^(\w+)\s.*(PASS|FAIL|MEASURED|INFO|SKIP)\s', log, re.M)) >= 57)
log = run({}, manual=True)
fails = re.findall(r'\[FAIL\s*\] (\S+)', log)
check('--manual: only T3, K11r (no cable, no real key in the sim) may fail: %s' % ' '.join(fails), set(fails) <= {'T3', 'K11r'})
check('  K11r and T5m ran', 'K11r' in log and 'T5m' in log)
print('%d failed' % bad if bad else 'ALL PASSED')
sys.exit(1 if bad else 0)
