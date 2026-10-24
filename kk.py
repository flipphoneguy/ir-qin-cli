"""Encode IR signals for devices saved by the Duoqin Remote app (Kookong SDK).

The app caches each device as a serialized Java IrData object. Plain devices (TV, amplifier...) store finished pulses per key. ACs store protocol tags, and the app turns (power, mode, temp, wind, swing) into bytes with its native libkksdk.so. We run that same library through host/kkhost, then do what the app's Java side does (NormalCodeHelper): run the remote's Lua script over the bytes and turn them into pulses.
"""
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(HERE, 'host', 'kkhost')
LIB = os.path.join(HERE, 'host', 'libkksdk.so')
CERT = os.path.join(HERE, 'host', 'cert.der')

# values as ACStateV2 uses them
POWER_ON, POWER_OFF = 0, 1
MODES = ['cool', 'heat', 'auto', 'fan', 'dry']
WINDS = ['auto', 'low', 'mid', 'high']
# functionId: which button was pressed; a few remotes encode it in the frame
FUNC = {'power': 1, 'mode': 2, 'temp_up': 3, 'temp_down': 4, 'wind': 5, 'swing': 6, 'fix': 7}

# runs the remote's Lua script once per stdin line "bytes;power,mode,temp,wind,ud,func", the way luajava does in the app
LUA_RUNNER = r'''
local f = assert(io.open(arg[1])); local script = assert(load(f:read('a'))); f:close()
local function ints(s) local t = {} for v in s:gmatch('-?%d+') do t[#t + 1] = tonumber(v) end return t end
for line in io.lines() do
  local b, s = line:match('^([^;]*);(.*)$')
  bytes = ints(b)
  local st = ints(s)
  power, mode, temperature, windSpeed, udWindMode, functionId = st[1], st[2], st[3], st[4], st[5], st[6]
  exts = nil
  local ok, err = pcall(script)
  if not ok then io.stderr:write(err, '\n') end
  local keys = {}
  for k in pairs(bytes) do keys[#keys + 1] = k end
  table.sort(keys)
  local out = {}
  for _, k in ipairs(keys) do out[#out + 1] = string.format('%d', math.floor(bytes[k])) end
  print(table.concat(out, ','))
end
'''


def load(path) -> dict:
    """One cached IrData file as a dict: fre, rid, type (1 plain, 2 AC), exts {tag: str}, keys [...]."""
    # javaobj takes ~0.4s to import, so only pay for it when reading the app's files
    from javaobj.v2.beans import JavaArray, JavaInstance, JavaString
    from javaobj.v2.main import load as java_load

    def conv(o):
        if isinstance(o, JavaString):
            return str(o)
        if isinstance(o, JavaArray):
            return [conv(x) for x in o]
        if not isinstance(o, JavaInstance):
            return o
        name = o.classdesc.name or ''
        if name == 'java.util.HashMap':
            items = [conv(x) for x in o.annotations[o.classdesc][1:]]
            return {int(items[i]): items[i + 1] for i in range(0, len(items), 2)}
        if name == 'java.util.ArrayList':
            return [conv(x) for x in o.annotations[o.classdesc][1:]]
        fields = {}
        for values in o.field_data.values():
            for f, v in values.items():
                fields[f.name] = conv(v)
        return fields['value'] if name.startswith('java.lang.') else fields

    with open(path, 'rb') as f:
        data = conv(java_load(f))
    assert isinstance(data, dict), f'{path}: not an IrData file'
    return data


def _ints(s):
    """'4&645,20700' -> [4, 645, 20700]"""
    return [int(x) for x in s.replace('&', ',').split(',') if x.strip()]


def _parts(exts, tag):
    return [p.strip() for p in exts.get(tag, '').split('|') if p.strip()]


class Format:
    """How bytes become pulses (NormalCodeHelper.getFormatParam and getWaveCode)."""

    def __init__(self, exts):
        self.lead = _ints(exts.get(300, ''))
        self.zero = _ints(exts.get(301, ''))
        self.one = _ints(exts.get(302, ''))
        self.delays = {v[0]: v[1:] for v in map(_ints, _parts(exts, 303))}
        self.lsb_first = exts.get(306, '').strip() == '1'
        self.trailer_one = exts.get(307, '').strip() != '1'
        self.repeat = int(exts.get(1508, '1'))
        self.bit_nums = dict((int(k), int(v)) for k, v in (p.split('&') for p in _parts(exts, 1509)))
        self.script = exts.get(1518, '').strip() or None
        # fixed pulses for some (function, value) pairs, used instead of encoding
        self.wave_codes = {}
        for p in _parts(exts, 1510):
            fid, values, codes = p.split('&', 2)
            for v in values.split(','):
                self.wave_codes[f'{fid.strip()}&{v}'] = _ints(codes)
        if 1511 in exts and not self.script:
            raise NotImplementedError('remote uses a BeanShell script (tag 1511)')

    def wave(self, frame):
        out = list(self.lead)
        for i, b in enumerate(frame):
            n = self.bit_nums.get(i, 8)
            if i == len(frame) - 1 and n == 8:
                n = self.bit_nums.get(-1, 8)
            bits = [(b >> k) & 1 for k in range(n)]
            for bit in bits if self.lsb_first else reversed(bits):
                out += self.one if bit else self.zero
            out += self.delays.get(i, [])
        if self.trailer_one and self.one:
            out.append(self.one[0])
        out += self.delays.get(-1, [])
        if len(out) % 2:
            out.append(1000)
        return out * self.repeat


class Mode:
    """What one AC mode allows (ACModelV2.initMoel, from tags 1501-1505: 'T' temp rules, 'S' wind rules)."""

    def __init__(self, mode, spec):
        self.mode = mode
        temps, winds = list(range(16, 31)), [0, 1, 2, 3]
        self.temp_ok = self.wind_ok = True
        parts = spec.split('|') if spec else []
        rule = next((p.split('&') for p in parts if 'T' in p), None)
        if rule:
            temps = [t for t in temps if t not in (_ints(rule[1]) if len(rule) > 1 else temps)]
            self.temp_ok = bool(temps)
        rule = next((p.split('&') for p in parts if 'S' in p), None)
        if rule:
            winds = [w for w in winds if w not in (_ints(rule[1]) if len(rule) > 1 else winds)]
            self.wind_ok = bool(winds)
        self.temps = temps if self.temp_ok else []
        self.winds = winds if self.wind_ok else []
        default = [26, 20, 24, 24, 23][mode]
        self.default_temp = (default if default in self.temps else self.temps[0]) if self.temp_ok else -1
        self.default_wind = self.winds[0] if self.wind_ok else -1


class Limits:
    """What one AC supports (ACStateV2 constructor)."""

    def __init__(self, exts):
        self.modes = {}
        for tag in range(1501, 1506):
            spec = exts.get(tag, '').strip()
            if 'NA' not in spec:
                self.modes[tag - 1501] = Mode(tag - 1501, spec)
        # vane (initUDDirect): 0 is swinging, other values are fixed positions
        vals = _ints(exts.get(1506, ''))
        if not vals:
            self.swing, self.positions = True, [1]
        elif vals[0] == 0:
            self.swing, self.positions = True, vals[1:]
        else:
            self.swing, self.positions = False, vals

    def uds(self):
        """Valid swing values; -1 when the remote only knows 'swinging'."""
        if not self.positions:
            return [-1]
        return ([0] if self.swing else []) + self.positions


class AC:
    def __init__(self, data, workdir):
        """data: load() result (exts keys may be strings after a JSON round trip)."""
        self.rid = data['rid']
        self.freq = data['fre']
        self.exts = {int(k): v for k, v in data['exts'].items()}
        self.fmt = Format(self.exts)
        self.limits = Limits(self.exts)
        self.workdir = workdir
        self._prepared = False

    def _prepare(self):
        """Write the files kkhost and lua read; only needed when actually encoding."""
        if self._prepared:
            return
        os.makedirs(self.workdir, exist_ok=True)
        base = os.path.join(self.workdir, str(self.rid))
        self._tags = base + '.tags'
        with open(self._tags, 'w') as f:
            f.writelines(f'{k}|{v.strip()}\n' for k, v in self.exts.items() if 1000 < k < 1501)
        if self.fmt.script:
            self._script = base + '.lua'
            with open(self._script, 'w') as f:
                f.write(self.fmt.script)
            self._runner = os.path.join(self.workdir, 'runner.lua')
            with open(self._runner, 'w') as f:
                f.write(LUA_RUNNER)
        self._prepared = True

    def frames(self, states):
        """Final bytes for each (power, mode, temp, wind, ud, func): native encoder, then the Lua script."""
        self._prepare()
        lines = ''.join(' '.join(map(str, st)) + '\n' for st in states)
        res = subprocess.run([HOST, LIB, str(self.rid), self._tags], input=lines, capture_output=True, text=True,
                             env=dict(os.environ, KKHOST_CERT=CERT))
        if res.returncode:
            raise RuntimeError(f'kkhost failed for remote {self.rid}: {res.stderr.strip()}')
        # the app only ever sends the first frame
        frames = [bytes.fromhex(line.split(' ')[0]) for line in res.stdout.splitlines()]
        if not self.fmt.script:
            return frames
        lines = ''.join(f"{','.join(map(str, fr))};{','.join(map(str, st))}\n" for fr, st in zip(frames, states))
        res = subprocess.run(['lua5.4', self._runner, self._script], input=lines, capture_output=True, text=True)
        if res.returncode or res.stderr:
            raise RuntimeError(f'lua script failed for remote {self.rid}: {res.stderr.strip()}')
        return [bytes(int(x) & 0xFF for x in line.split(',') if x) for line in res.stdout.splitlines()]

    def wave(self, state, frame):
        power, mode, temp, wind, ud, func = state
        if self.fmt.wave_codes:
            cur = {1: power, 2: mode, 3: temp, 4: temp, 5: wind, 6: ud, 7: ud}.get(func, -1)
            fixed = self.fmt.wave_codes.get(f'{func}&{cur}') or self.fmt.wave_codes.get(f'{func}&')
            if fixed:
                return fixed
        return self.fmt.wave(frame)

    def pattern(self, state, table=None):
        """Pulses for one (power, mode, temp, wind, ud, func), from a build_table() result when given."""
        if table:
            key = ' '.join(map(str, state if table['per_func'] else state[:5]))
            if key in table['frames']:
                return self.wave(state, bytes.fromhex(table['frames'][key]))
        return self.wave(state, self.frames([state])[0])

    def states(self):
        """Every (power, mode, temp, wind, ud) the app can produce for this AC."""
        return [(p, m, t, w, u)
                for m, lim in self.limits.modes.items()
                for t in lim.temps or [-1]
                for w in lim.winds or [-1]
                for u in self.limits.uds()
                for p in (POWER_ON, POWER_OFF)]

    def build_table(self):
        """Final bytes for every state, keyed 'p m t w u', plus ' f' if this remote encodes the pressed button."""
        states = self.states()
        sample = states[::max(1, len(states) // 300)]
        base = self.frames([s + (-1,) for s in sample])
        per_func = any(self.frames([s + (f,) for s in sample]) != base for f in FUNC.values())
        keyed = [s + (f,) for s in states for f in [-1, *FUNC.values()]] if per_func else [s + (-1,) for s in states]
        frames = self.frames(keyed)
        return {'per_func': per_func,
                'frames': {' '.join(map(str, st if per_func else st[:5])): fr.hex() for st, fr in zip(keyed, frames)}}


class Plain:
    """Device whose keys each store finished pulses."""

    def __init__(self, data):
        self.freq = data['fre']
        self.keys = {k['fkey']: k for k in data['keys']}

    def pattern(self, key):
        pulse = self.keys[key]['pulse']
        if not pulse:
            raise NotImplementedError(f'key {key} has no stored pulses')
        return [int(x) for x in pulse.split(',')]
