"""Checks the encoder against real data from the app. Run: python tests/verify.py [app-cache-dir]"""
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import kk  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
work = tempfile.mkdtemp(prefix='ir-verify-', dir=os.environ.get('TMPDIR'))
failed = False


def check(ok, what):
    global failed
    failed |= not ok
    print(('ok    ' if ok else 'FAIL  ') + what)


# sample-ac is one AC remote as the app caches it; sample-captures.txt has patterns the app sent with it (logged from its IR.transmit), and each must be some state's output
ac = kk.AC(kk.load(os.path.join(HERE, 'sample-ac')), work)
table = ac.build_table()
by_pattern = {}
for key in table['frames']:
    st = tuple(map(int, key.split())) + (-1,)
    by_pattern.setdefault(tuple(ac.pattern(st, table)), st)
caps = [tuple(map(int, line.split(','))) for line in open(os.path.join(HERE, 'sample-captures.txt')) if line.strip()]
check(all(c in by_pattern for c in caps), f'sample: {sum(c in by_pattern for c in caps)}/{len(caps)} captured app patterns reproduced')

# table lookups must equal live encoding
sample = [tuple(map(int, k.split())) + (-1,) for k in list(table['frames'])[::97]]
check(all(ac.pattern(st, table) == ac.pattern(st) for st in sample), f'sample: table matches live encoding on {len(sample)} states')

# limits must match what the app itself computed (AC_STATE_<id> in its prefs), for every AC in the app cache
cache = sys.argv[1] if len(sys.argv) > 1 else None
if cache:
    import html
    import json
    prefs = open(os.path.join(cache, 'Remote.xml')).read()
    for m in re.finditer(r'name="AC_STATE_(\d+)">([^<]*)<', prefs):
        dev, state = m.group(1), json.loads(html.unescape(m.group(2)))
        path = next((os.path.join(cache, f) for f in os.listdir(cache) if f.startswith(dev + '-')), None)
        if not path:
            continue
        lim = kk.AC(kk.load(path), work).limits
        ok = [ml['modelType'] for ml in state['modelList']] == list(lim.modes)
        for ml in state['modelList']:
            mode = lim.modes[ml['modelType']]
            ok &= ml['tempCanControl'] == mode.temp_ok and ml['windSpeedCanControl'] == mode.wind_ok
            ok &= not mode.temp_ok or (ml['lowTmp'], ml['highTmp']) == (mode.temps[0], mode.temps[-1])
            ok &= [int(w) for w in ml['windSpeedList']] == (mode.winds or [0, 1, 2, 3])
        ok &= state['udWindDirectList'] == lim.positions
        check(ok, f'{os.path.basename(path)}: limits match the app')

sys.exit(failed)
