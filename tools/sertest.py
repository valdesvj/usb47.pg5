#!/usr/bin/env python3
"""sertest.py - test every item of SwissMicros' "DMCP5 Serial Console API" data sheet (DMCP5 3.58, IFC 3.18) on
the DM42n, with the calculator program SERTEST.pg5 (make sertest), and write a log to send to SwissMicros.

  1. On the calculator: load build/sertest/SERTEST.pg5 (DMCP5 3.58), run it. It switches the console on itself.
  2. On the PC (Linux):  python3 tools/sertest.py /dev/ttyACM0            (about 3 minutes, no input needed)
                         python3 tools/sertest.py /dev/ttyACM0 --manual   (+ a real key, the cable, DMCP's menu)
  3. Send sertest_<date>.log (and sertest_<date>_wire.txt: every line on the wire, with times).
  To end SERTEST: the test does it at the end (QUIT); Ctrl+C here sends QUIT too; python3 tools/sertest.py --quit
  ends it any time; on the calculator EXIT or F6. No reset: the calculator goes off, ON gives DMCP's menu.

Every item gets PASS (as the sheet says), FAIL (not as the sheet says: the detail says what came instead), MEASURED
(a value or behaviour the sheet leaves open: the two UNKNOWNs) or INFO, and the columns NAV47 / CHESS47 say which of
those programs use the same item (yes: used in Victor's tests on the DM42n; code: in the code but not exercised).
Python 3 only, no packages (the port is opened raw, as tools/dm42view.py does).
"""
import argparse
import datetime
import os
import re
import select
import statistics
import sys
import termios
import time

W, H = 400, 240
INVALID = -(1 << 31)                                   # SER_IOCTL_INVALID (1 << 31) as a C int
K1, K2, K5 = 0x1D, 0x1E, 0x19                          # DMCP key codes of 1, 2, 5


def pixel(x, y):                                       # src/sertest/sertest.c sertest_pixel()
    if x < 384:
        return (x + 3 * y) % 50 < 9
    return (y >> ((x - 384) // 2)) & 1


class Line:
    __slots__ = ('t', 'ch', 'data', 'idb', 'cut')

    def __init__(self, t, ch, data, idb, cut=False):
        self.t, self.ch, self.data, self.idb = t, ch, bytes(data), idb   # idb: an id byte came right before it
        self.cut = cut                                                   # cut: no \n before the next id byte

    @property
    def text(self):
        return bytes(b for b in self.data if b < 0x80).decode('ascii', 'replace')


class Port:
    """the console as the data sheet's section 10: id bytes 0x10 + n, lines per channel, raw bytes kept"""

    def __init__(self, path, wire):
        self.path, self.wire, self.fd = path, wire, None
        self.lines, self.buf, self.ch, self.idb = [], {}, 0, False
        self.t0 = time.time()

    def open(self, wait=60):
        end = time.time() + wait
        while not os.path.exists(self.path):
            if time.time() > end:
                raise SystemExit('no %s: is SERTEST running and the cable in?' % self.path)
            time.sleep(0.3)
        self.fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY)
        t = termios.tcgetattr(self.fd)
        t[0] = 0; t[1] = 0; t[3] = 0
        t[2] |= termios.CLOCAL | termios.CREAD
        t[6][termios.VMIN] = 0; t[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, t)
        self.note('port opened (DTR up)')

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
        self.fd = None
        self.buf, self.ch = {}, 0
        self.note('port closed (DTR down)')

    def note(self, s):
        self.wire.write('%8.3f  ---  %s\n' % (time.time() - self.t0, s))

    def send(self, ch, data):
        if isinstance(data, str):
            data = data.encode('ascii')
        self.wire.write('%8.3f  TX%-2d %s\n' % (time.time() - self.t0, ch, show(data)))
        os.write(self.fd, bytes([0x10 + ch]) + data + b'\n')

    def pump(self, timeout=0.05):
        if self.fd is None:
            time.sleep(timeout)
            return
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return
        try:
            data = os.read(self.fd, 65536)
        except OSError:
            self.fd = None
            self.note('port gone')
            return
        if not data:
            time.sleep(timeout)
            return
        now = time.time()
        for b in data:
            if 0x10 <= b <= 0x1a:
                self.ch, self.idb = b - 0x10, True
                buf = self.buf.get(self.ch)
                if buf and buf[0]:                     # a write starts at a line: the last one lost its end
                    self.add(now, self.ch, buf[0], buf[1], cut=True)
                    buf[0] = bytearray()
                continue
            buf = self.buf.setdefault(self.ch, [bytearray(), self.idb])
            if not buf[0]:
                buf[1] = self.idb
            self.idb = False
            if b == 0x0a:
                self.add(now, self.ch, buf[0], buf[1])
                buf[0] = bytearray()
            else:
                buf[0].append(b)

    def add(self, now, ch, data, idb, cut=False):
        ln = Line(now - self.t0, ch, data, idb, cut)
        self.lines.append(ln)
        self.wire.write('%8.3f  RX%-2d %s%s\n' % (ln.t, ch, show(ln.data), '   [cut: no \\n before the next write]'
                                                  if cut else ''))

    def mark(self):
        return len(self.lines)

    def collect(self, secs):
        end = time.time() + secs
        while time.time() < end:
            self.pump(min(0.05, max(0.0, end - time.time())))

    def wait(self, pred, timeout=3.0, since=None):
        i = self.mark() if since is None else since
        end = time.time() + timeout
        while True:
            while i < len(self.lines):
                if pred(self.lines[i]):
                    return self.lines[i]
                i += 1
            if time.time() > end:
                return None
            self.pump(0.05)

    def reply(self, ch, prefix, timeout=3.0, since=None):
        return self.wait(lambda l: l.ch == ch and l.text.startswith(prefix), timeout, since)


def show(data):
    text = bytes(b for b in data if b < 0x80)
    nbin = sum(1 for b in data if b >= 0x80)
    s = repr(text)[2:-1]
    if len(s) > 100:
        s = s[:100] + '...(%d)' % len(text)
    return s + (' +%d binary' % nbin if nbin else '')


class Test:
    def __init__(self, port, log, manual):
        self.p, self.logf, self.manual = port, log, manual
        self.res = []
        self.calclog = ''
        self.boot = {}

    # ---- the log ----
    def log(self, s=''):
        print(s)
        self.logf.write(s + '\n')
        self.logf.flush()

    def rec(self, rid, item, tag, result, detail, apps=('-', '-')):
        self.res.append((rid, item, tag, result, detail, apps))
        self.log('  [%-8s] %-5s %s: %s' % (result, rid, item, detail))

    def cmd(self, text, ch=2, expect=None, timeout=3.0):
        since = self.p.mark()
        self.p.send(ch, text)
        if expect is None:
            expect = text.split()[0]
        ln = self.p.reply(ch, expect, timeout, since)
        return ln.text if ln else None

    def result(self, prefix):
        """SERTEST's last result again (RESULT): it may have been lost while the PC did not read"""
        since = self.p.mark()
        self.p.send(2, 'RESULT')
        ln = self.p.reply(2, prefix, 3, since)
        if ln:
            self.log('    (%s lost on the wire; RESULT gave it again)' % prefix)
        return ln

    def io(self, op, val='R'):
        r = self.cmd('IO 0x%02X %s' % (op, val), expect='IO 0x%02X' % op)
        m = re.search(r'-> (-?\d+)', r or '')
        return int(m.group(1)) if m else None

    # ---- the tests ----
    def start(self):
        self.log('== connect')
        self.p.open()
        for i in range(10):
            r = self.cmd('PING start%d' % i, expect='PONG start%d' % i, timeout=1.5)
            if r:
                break
        else:
            raise SystemExit('SERTEST does not answer on channel 2: is it running (DMCP5 3.58)?')
        self.rec('W1', 'channel ids 0x10+n, lines end in \\n', 'DOC', 'PASS', 'PING on channel 2 -> ' + r,
                 ('yes', 'yes'))
        since = self.p.mark()
        self.p.send(0, 'V')
        v = self.p.reply(0, 'V|', 2.0, since)
        if v:
            self.rec('K1', 'V: identity on channel 0', 'DOC', 'PASS' if '|HW SERTEST' in v.text and '|IFC' in v.text
                     else 'FAIL', v.text)
            m = re.search(r'IFC ([\d.]+)', v.text)
            self.rec('R1', 'a .pg5 with IFC 18 runs on DMCP5 3.58', 'DOC', 'PASS' if m else 'INFO',
                     'IFC %s in the V answer, SERTEST built with PLATFORM_IFC_VER 18' % (m.group(1) if m else '?'),
                     ('yes', 'yes'))
        else:
            self.rec('K1', 'V: identity on channel 0', 'DOC', 'FAIL', 'no V| answer in 2 s')
        self.report(quiet=True)
        b = re.search(r'BOOT read(.*)', self.calclog)
        if b:
            self.boot = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', b.group(1))}
            self.log('  the user\'s settings at start (SERTEST puts them back at QUIT): ' + b.group(1).strip())
        for k in ('BOOT invalid', 'BOOT LINE_READY', 'BOOT IS_ENABLED'):
            m = re.search(k + r'[^\n]*', self.calclog)
            if m:
                self.log('  calculator: ' + m.group(0))

    def ioctls(self):
        self.log('== usb_serial_ioctl (section 6)')
        names = {0x01: 'IS_ENABLED', 0x02: 'IS_ACTIVE', 0x03: 'IS_CONNECTED'}
        for op, n in names.items():
            r = self.io(op)
            self.rec('I%d' % op, 'SERIO_%s read' % n, 'DOC', 'PASS' if r == 1 else 'FAIL',
                     'read -> %s (1 expected: on, cable in, port open)' % r,
                     {1: ('yes', 'yes'), 2: ('yes', 'yes'), 3: ('yes', 'yes')}[op])
        r = self.io(0x02, 1)
        self.rec('I2w', 'write to a read-only switch (IS_ACTIVE 1)', 'NOT DOC', 'MEASURED', '-> %s%s' % (
            r, ' = SER_IOCTL_INVALID' if r == INVALID else ''))
        for op, n, apps in ((0x10, 'SEND_PRINTF', ('-', '-')), (0x11, 'SEND_LCD', ('yes', 'yes')),
                            (0x12, 'SEND_KEYS', ('-', '-')), (0x13, 'SEND_IR', ('-', '-'))):
            was = self.io(op)
            a, ra = self.io(op, 1), self.io(op)
            b, rb = self.io(op, 0), self.io(op)
            self.io(op, was if was in (0, 1) else 0)
            ok = ra == 1 and rb == 0
            self.rec('I%02X' % op, 'SERIO_%s read / write' % n, 'DOC', 'PASS' if ok else 'FAIL',
                     'was %s; set 1 -> %s, read %s; set 0 -> %s, read %s; put back' % (was, a, ra, b, rb), apps)
        r = self.io(0x7F)
        self.rec('I7F', 'an unknown op returns SER_IOCTL_INVALID', 'DOC', 'PASS' if r == INVALID else 'FAIL',
                 'op 0x7F read -> %s' % r)
        ready = [self.io(0x08, ch) for ch in range(0, 11)]
        length = [self.io(0x09, ch) for ch in (2, 3)]
        self.rec('I08', 'SERIO_LINE_READY / LINE_LENGTH with nothing waiting', 'DOC',
                 'PASS' if ready[2] == 0 and ready[3] == 0 and length == [0, 0] else 'FAIL',
                 'LINE_READY ch0..10 = %s; LINE_LENGTH ch2, ch3 = %s' % (ready, length), ('-', 'yes'))
        en = self.io(0x01, 1)
        self.rec('I01w', 'SERIO_IS_ENABLED write 1 (while on)', 'DOC', 'PASS' if en == 1 else 'FAIL', '-> %s' % en,
                 ('yes', 'yes'))

    def latency(self):
        self.log('== answers while polling (section 7: "within about 100 ms")')
        ts = []
        for i in range(10):
            t = time.time()
            if self.cmd('PING lat%d' % i, expect='PONG lat%d' % i, timeout=2):
                ts.append((time.time() - t) * 1000)
        if ts:
            self.rec('T2', 'round trip PC -> SERTEST (50 ms poll timer) -> PC', 'MEAS', 'PASS' if max(ts) < 300 else
                     'INFO', '%d/10, median %.0f ms, max %.0f ms' % (len(ts), statistics.median(ts), max(ts)),
                     ('-', 'yes'))
        else:
            self.rec('T2', 'round trip', 'MEAS', 'FAIL', 'no answer')

    def channels(self):
        self.log('== channels (section 5)')
        r = self.cmd('PING c3', ch=3, expect='PONG c3')
        self.rec('C3', 'channel 3 both ways', 'DOC', 'PASS' if r else 'FAIL', r or 'no answer on channel 3')
        since = self.p.mark()
        self.p.send(4, 'PING c4')
        self.p.collect(0.8)
        got = [l for l in self.p.lines[since:] if 'c4' in l.text]
        r4 = self.io(0x08, 4)
        self.rec('C4in', 'input on channel 4 is dropped', 'DOC', 'PASS' if not got else 'FAIL',
                 'PING on channel 4: %s; LINE_READY ch4 -> %s' % ('answered!' if got else 'no answer', r4))
        found = {}
        for n in range(0, 11):
            since = self.p.mark()
            self.cmd('CHAN %d T%d' % (n, n), expect='CHAN %d sent' % n)
            ln = self.p.wait(lambda l, n=n: l.text == 'T%d' % n, 0.5, since)
            found[n] = ln.ch if ln else None
        ok = all(found[n] == n for n in range(2, 11))
        self.rec('C2', 'program output on channels 2..10', 'DOC', 'PASS' if ok else 'FAIL',
                 ' '.join('%d:%s' % (n, 'ok' if found[n] == n else found[n]) for n in range(2, 11)), ('-', 'yes'))
        self.rec('C0', 'program output on channels 0 and 1 (not the program\'s: tried)', 'NOT DOC', 'INFO',
                 'ch0: %s, ch1: %s' % ('arrived' if found[0] == 0 else 'not seen',
                                       'arrived' if found[1] == 1 else 'not seen'))
        since = self.p.mark()
        r = self.cmd('PRINTF', expect='PRINTF was')
        pf = self.p.wait(lambda l: l.ch == 1 and 'PRINTF hello 42' in l.text, 0.5, since)
        self.rec('C1', 'SERIO_SEND_PRINTF: printf on channel 1', 'DOC', 'PASS' if pf else 'FAIL',
                 '%s; %s' % ('"PRINTF hello 42" on ch1' if pf else 'nothing on ch1', r))
        since = self.p.mark()
        self.p.send(2, 'MULTI')
        self.p.wait(lambda l: l.text == 'M3', 2, since)
        ms = [l for l in self.p.lines[since:] if l.text in ('M1', 'M2', 'M3')]
        ids = ''.join('1' if l.idb else '0' for l in ms)
        self.rec('W2', 'the id byte only at the start of each write', 'MEAS', 'PASS' if ids == '100' else 'FAIL',
                 'one write "M1\\nM2\\nM3\\n": id byte before M1 M2 M3 = %s' % ids, ('-', 'yes'))

    def reads(self):
        self.log('== usb_serial_read_line (section 6)')
        self.cmd('VERBOSE 1')
        since = self.p.mark()
        self.p.send(2, 'PING nul')
        rx = self.p.reply(2, 'RX ch=2', 2, since)
        self.p.reply(2, 'PONG nul', 2, since)
        m = dict(re.findall(r'(\w+)=(\S+)', rx.text)) if rx else {}
        self.rec('RL1', 'n > 0: the bytes, with the final \\n', 'DOC',
                 'PASS' if m.get('n') == '9' and m.get('last') == '0A' else 'FAIL',
                 '"PING nul\\n": %s' % (rx.text if rx else 'no RX report'), ('-', 'yes'))
        self.rec('RL5', 'DMCP may add a NUL after the line', 'DOC', 'INFO',
                 'buf[n] after the read = %s (buf filled with AA before)' % m.get('after'))
        self.rec('I09', 'SERIO_LINE_LENGTH of a waiting line', 'DOC',
                 'PASS' if m.get('len') == m.get('n') else 'INFO',
                 'LINE_LENGTH %s, read_line n %s, LINE_READY %s' % (m.get('len'), m.get('n'), m.get('ready')))
        self.cmd('BUF 16')
        since = self.p.mark()
        self.p.send(2, 'PING ' + 'y' * 40)
        rx = self.p.reply(2, 'RX ch=2', 2, since)
        lg = self.p.reply(2, 'RXLONG', 2, since)
        self.rec('RL3', 'a line longer than buflen: -1, it stays; buf = NULL discards it', 'DOC',
                 'PASS' if rx and ' n=-1' in rx.text and lg and 'ready_after=0' in lg.text else 'FAIL',
                 'buflen 16, a 46-byte line: %s | %s' % (rx.text if rx else '-', lg.text if lg else '-'),
                 ('-', 'code'))
        self.cmd('BUF 256')
        since = self.p.mark()
        self.cmd('HOLD 2500', expect='HOLD')
        for i in range(20):
            self.p.send(2, 'PING H%02d' % i)
            time.sleep(0.02)
        he = self.p.reply(2, 'HOLDEND', 5, since)
        self.p.collect(1.5)
        got = sorted({int(l.text[6:8]) for l in self.p.lines[since:] if l.text.startswith('PONG H')})
        self.rec('RL7', 'at most 16 lines wait per channel (USIN_MAX_READLINES)', 'DOC',
                 'PASS' if len(got) <= 16 and got else 'FAIL',
                 '20 lines while not reading: %s; answered %d: %s' % (he.text if he else 'no HOLDEND', len(got),
                                                                     ','.join('%d' % g for g in got)))
        since = self.p.mark()
        self.cmd('HOLD 1500', expect='HOLD')
        self.p.send(2, 'ECHO ' + 'z' * 295)
        time.sleep(0.05)
        self.p.send(2, 'PING after')
        self.p.reply(2, 'HOLDEND', 4, since)
        self.p.collect(1.0)
        rxs = [l.text for l in self.p.lines[since:] if l.text.startswith('RX ch=2')]
        ovf = [t for t in rxs if 'ovf=1' in t]
        after = any(l.text.startswith('PONG after') for l in self.p.lines[since:])
        self.rec('RL6', 'a line cut by an overflow: is_overflow set, it ends in 0x0f', 'DOC',
                 'PASS' if ovf and 'last=0F' in ovf[0] else 'FAIL',
                 'a 300-byte line into the 256-byte FIFO: %s; the next line %s' % (
                     ovf[0] if ovf else ('; '.join(rxs) or 'no RX report'), 'answered' if after else 'lost'),
                 ('-', 'code'))
        self.rec('W4', 'an overflowed line ends with 0x0f (wire format)', 'DOC', 'PASS' if ovf and 'last=0F' in ovf[0]
                 else 'FAIL', 'see RL6')
        self.cmd('VERBOSE 0')
        r = self.io(0x08, 2)
        self.rec('RL2', 'n = 0 / LINE_READY 0 when nothing waits', 'DOC', 'PASS' if r == 0 else 'FAIL',
                 'LINE_READY ch2 after the tests -> %s' % r, ('-', 'yes'))

    def writes(self):
        self.log('== usb_serial_write (section 6)')
        for n in (255, 300):
            since = self.p.mark()
            self.p.send(2, 'BIG %d' % n)
            self.p.wait(lambda l: l.text == 'BIGEND', 2, since)
            g = [l for l in self.p.lines[since:] if l.ch == 2 and l.data[:1] == b'G']
            got = len(g[0].data) if g else None
            if n == 255:
                self.rec('WR0', 'one write of 255 bytes arrives whole', 'MEAS', 'PASS' if got == 254 else 'FAIL',
                         'a 255-byte line: %s bytes + \\n' % got)
            else:
                self.rec('WR1', 'at most 255 bytes of one write are sent', 'MEAS', 'PASS' if got == 255 else 'FAIL',
                         'a 300-byte write: %s bytes arrived before the next write (255 expected)' % got)
        for size, gap, n in ((64, 0, 100), (128, 0, 100), (128, 0, 300), (200, 0, 100), (255, 0, 100),
                             (255, 33, 50)):
            since = self.p.mark()
            self.p.send(2, 'BURST %d %d %d' % (n, size, gap))
            end = self.p.reply(2, 'BURSTEND', 30, since)
            self.p.collect(0.3)
            ok, cut = set(), 0
            for l in self.p.lines[since:]:
                if l.ch == 2 and l.data[:1] == b'S' and l.data[1:7].isdigit():
                    if len(l.data) == size - 1 and not l.cut:
                        ok.add(int(l.data[1:7]))
                    else:
                        cut += 1
            lost = n - len(ok)
            rid = 'WR3' if size <= 128 else 'WR2'
            expect = (lost == 0) if size <= 128 else None
            self.rec(rid, '%d writes of %d bytes, gap %d ms' % (n, size, gap), 'MEAS',
                     ('PASS' if expect else 'FAIL') if expect is not None else 'MEASURED',
                     'whole %d, lost %d, cut %d; %s' % (len(ok), lost, cut, end.text if end else 'no BURSTEND'),
                     ('-', 'yes') if size <= 128 else ('-', '-'))

    def lcd(self):
        self.log('== LCD rows on channel 0 (section 8)')
        self.cmd('LCD 1')
        self.cmd('PATTERN')
        self.p.collect(0.8)
        since = self.p.mark()
        self.p.send(0, 'L')
        k77 = self.p.reply(2, 'KEY 77', 2, since)
        rel = self.p.wait(lambda l: l.ch == 2 and l.text.startswith('KEY 0'), 2, since)
        self.p.collect(1.5)
        rows = [l for l in self.p.lines[since:] if l.ch == 0 and l.text.startswith('L ')]
        self.rec('K6', 'L: key 77, then a release', 'MEAS', 'PASS' if k77 and rel else 'FAIL',
                 '%s; release %s' % (k77.text if k77 else 'no key 77', 'yes' if rel else 'no'), ('yes', 'yes'))
        img, bad, ys = {}, 0, set()
        for l in rows:
            binary = [b & 0x7F for b in l.data if b >= 0x80]
            yy = int(l.text[2:4], 16)
            bits = 0
            for j, v in enumerate(binary[:58]):
                bits |= v << (7 * j)
            row = bytes((bits >> (8 * i)) & 0xFF for i in range(50))
            y = H - 1 - yy
            ys.add(y)
            img[y] = [0 if (row[49 - x // 8] >> (7 - x % 8)) & 1 else 1 for x in range(W)]
            if len(binary) != 58:
                bad += 1
        wrong = [y for y in sorted(img) if img[y] != [int(pixel(x, y)) for x in range(W)]]
        self.rec('K7', 'after the redraw and lcd_forced_refresh() every non-blank row goes out', 'MEAS',
                 'PASS' if len(ys) == H else 'FAIL', '%d rows of 240 (all non-blank)' % len(ys), ('yes', 'yes'))
        self.rec('K10', 'row format: L <yy>, 58 bytes 7 bits each, row 239 at the top, mirrored, dark = 0', 'MEAS',
                 'PASS' if ys and not wrong and not bad else 'FAIL',
                 'the test pattern pixel for pixel: %d rows right, %d wrong%s, %d with a bad length' % (
                     len(img) - len(wrong), len(wrong), (' (rows %s...)' % wrong[:5]) if wrong else '', bad),
                 ('yes', 'yes'))
        self.rec('W3', 'a line: an ASCII part, then a binary part (top bit set)', 'DOC',
                 'PASS' if ys and not bad else 'FAIL', 'see K10')
        since = self.p.mark()
        self.cmd('PING same')                           # SERTEST redraws the same pattern
        self.p.collect(0.8)
        n = sum(1 for l in self.p.lines[since:] if l.ch == 0 and l.text.startswith('L '))
        self.rec('K9', 'a row goes out only when it changed', 'MEAS', 'PASS' if n == 0 else 'FAIL',
                 'the same picture refreshed again: %d rows sent' % n, ('yes', 'yes'))
        self.cmd('NO77 1')
        self.p.collect(0.3)
        since = self.p.mark()
        self.p.send(0, 'L')
        k = self.p.reply(2, 'KEY 77', 2, since)
        self.p.collect(1.0)
        n = sum(1 for l in self.p.lines[since:] if l.ch == 0 and l.text.startswith('L '))
        self.rec('K8', 'L without the program\'s redraw sends nothing', 'MEAS', 'PASS' if k and n == 0 else 'FAIL',
                 'key 77 %s, %d rows' % ('came' if k else 'missing', n))
        self.cmd('NO77 0')
        self.cmd('LCD 0')
        self.p.collect(0.3)
        since = self.p.mark()
        self.p.send(0, 'L')
        k = self.p.reply(2, 'KEY 77', 2, since)
        self.p.collect(1.0)
        n = sum(1 for l in self.p.lines[since:] if l.ch == 0 and l.text.startswith('L '))
        self.rec('K8b', 'L with SERIO_SEND_LCD 0', 'NOT DOC', 'MEASURED', 'key 77 %s, %d rows' % (
            'came' if k else 'did not come', n))
        lcd = self.boot.get('SEND_LCD', 0)
        self.cmd('LCD %d' % (lcd if lcd in (0, 1) else 0))
        self.cmd('SCREEN')

    def keys(self):
        self.log('== keys from the PC (section 8)')
        since = self.p.mark()
        self.p.send(0, 'KP%02X' % K1)
        d = self.p.reply(2, 'KEY %d ' % K1, 2, since)
        time.sleep(0.1)
        self.p.send(0, 'KR')
        u = self.p.reply(2, 'KEY 0 ', 2, since)
        self.rec('K2', 'KPhh holds a key: the program gets it as a real key', 'DOC', 'PASS' if d else 'FAIL',
                 d.text if d else 'no key 29', ('yes', 'yes'))
        self.rec('K4', 'KR releases it', 'DOC', 'PASS' if u else 'FAIL', u.text if u else 'no release', ('yes', 'yes'))
        since = self.p.mark()
        self.p.send(0, 'K %02X' % K2)
        d = self.p.reply(2, 'KEY %d ' % K2, 2, since)
        u = self.p.wait(lambda l: l.ch == 2 and l.text.startswith('KEY 0 '), 2, since)
        self.rec('K5', 'K hh: a short press, press then release', 'DOC', 'PASS' if d and u else 'FAIL',
                 '%s / %s' % (d.text if d else 'no key 30', u.text if u else 'no release'))
        since = self.p.mark()
        self.p.send(0, 'KP%02X%02X' % (K2, K1))
        time.sleep(0.15)
        self.p.collect(0.3)
        self.p.send(0, 'KR')
        self.p.collect(0.5)
        ks = [l.text for l in self.p.lines[since:] if l.ch == 2 and l.text.startswith('KEY')]
        self.rec('K3', 'KPhhhh: two keys held (k1 the low byte)', 'DOC', 'INFO' if ks else 'FAIL',
                 'KP%02X%02X then KR: %s' % (K2, K1, ' | '.join(ks) or 'nothing'), ('code', '-'))
        self.cmd('KEYS 1')
        since = self.p.mark()
        self.p.send(0, 'K %02X' % K1)
        self.p.collect(1.0)
        kr = [l.text for l in self.p.lines[since:] if l.ch == 0 and re.match(r'K [0-9a-fA-F]{2} ', l.text)]
        self.rec('K11', 'SERIO_SEND_KEYS: key reports K k1 k2 tttttt (injected key)', 'DOC',
                 'PASS' if kr else 'MEASURED', ' | '.join(kr) or 'no K report for an injected key')
        if self.manual:
            since = self.p.mark()
            input('\n  >>> press and release the 5 key on the calculator, then press Enter here ')
            self.p.collect(0.5)
            kr = [l.text for l in self.p.lines[since:] if l.ch == 0 and re.match(r'K [0-9a-fA-F]{2} ', l.text)]
            kk = [l.text for l in self.p.lines[since:] if l.ch == 2 and l.text.startswith('KEY')]
            self.rec('K11r', 'SERIO_SEND_KEYS: key reports for a real key', 'DOC', 'PASS' if kr else 'FAIL',
                     'reports: %s; the program got: %s' % (' | '.join(kr) or 'none', ' | '.join(kk) or 'none'))
        self.cmd('KEYS %d' % (self.boot.get('SEND_KEYS', 0) if self.boot.get('SEND_KEYS') in (0, 1) else 0))

    def wake(self):
        self.log('== does a line wake sys_sleep()? (section 7, UNKNOWN 1)')
        out = []
        for i in range(3):
            self.cmd('NOWAKE 4000', expect='NOWAKE 4000 armed')
            since = self.p.mark()
            time.sleep(1.0)
            t = time.time()
            self.p.send(2, 'PING W%d' % i)
            pong = self.p.reply(2, 'PONG W%d' % i, 6, since)
            lat = (time.time() - t) * 1000 if pong else None
            end = self.p.reply(2, 'NOWAKEEND', 6, since)
            out.append((lat, end.text if end else 'no NOWAKEEND'))
            self.log('    try %d: answer after %s ms; %s' % (i + 1, '%.0f' % lat if lat else '-', out[-1][1]))
        woke = [o for o in out if o[0] is not None and o[0] < 500 and 'woke=line' in o[1]]
        slept = [o for o in out if o[0] is not None and o[0] > 2000]
        verdict = ('a line WAKES sys_sleep(): answered in %s ms with no timer running' %
                   '/'.join('%.0f' % o[0] for o in woke) if len(woke) == 3 else
                   'a line does NOT wake sys_sleep(): answered only at the fallback timer (%s ms)' %
                   '/'.join('%.0f' % o[0] for o in slept) if len(slept) == 3 else
                   'mixed: ' + '; '.join('%s ms %s' % ('%.0f' % o[0] if o[0] else '-', o[1]) for o in out))
        self.rec('T1', 'a line from the host wakes sys_sleep()', 'UNKNOWN', 'MEASURED', verdict, ('-', 'code'))

    def new_host(self):
        self.log('== a new host: DTR and the lines waiting (section 7)')
        self.cmd('HOLD 4000', expect='HOLD')
        for i in range(3):
            self.p.send(2, 'PING STALE%d' % i)
        time.sleep(0.6)                                 # the lines are in SERTEST's FIFO (it logs LINE_READY)
        self.p.close()
        time.sleep(1.0)
        self.p.open()
        since = self.p.mark()
        hello = self.p.reply(2, 'HELLO', 3, since)
        self.p.reply(2, 'HOLDEND', 5, since)
        self.p.collect(1.0)
        got = [l.text for l in self.p.lines[since:] if l.text.startswith('PONG STALE')]
        self.report(quiet=True)
        part = self.calclog[self.calclog.rfind(' HOLD 4000'):]
        seen = re.findall(r'HOLDING ready2=(-?\d+) connected=(-?\d+)', part)
        st = re.findall(r'STATE active \S+ connected (\S+) ready2=(-?\d+)', part)
        before = max([int(r) for r, c in seen if c == '1'] or [0])
        self.rec('T4', 'lines sent before a reconnect are still in the FIFO', 'DOC',
                 'PASS' if len(got) == 3 else 'FAIL',
                 'LINE_READY while the port was open: %d; at the close / reopen: %s; answered after the reconnect: %d '
                 'of 3%s' % (before, ', '.join('%s ready2=%s' % x for x in st) or '-', len(got),
                             ' (the lines were there, then gone: DMCP emptied the FIFO)' if before and not got else ''),
                 ('-', 'yes'))

    def floods(self):
        self.log('== a host that does not read (section 6, UNKNOWN 2)')
        since = self.p.mark()
        self.p.send(2, 'FLOOD 8000 128')
        self.p.note('not reading for 12 s')
        time.sleep(12)
        self.p.collect(3)
        fl = [l for l in self.p.lines[since:] if l.ch == 2 and l.data[:1] == b'F' and l.data[1:7].isdigit()]
        seqs = sorted({int(l.data[1:7]) for l in fl if len(l.data) == 127 and not l.cut})
        cut = sum(1 for l in fl if l.cut or len(l.data) != 127)
        end = self.p.reply(2, 'FLOODEND', 3, since)
        if not end:                                    # lost while the PC did not read: SERTEST keeps it
            end = self.result('FLOODEND')
        alive = self.cmd('PING alive', expect='PONG alive')
        m = dict(re.findall(r'(\w+)=(\S+)', end.text)) if end else {}
        self.rec('WR4', 'the port open, the host not reading: writes', 'UNKNOWN', 'MEASURED',
                 'SERTEST wrote %s lines in 8 s, longest write call %s, calls > 20 ms %s; the PC then got %d whole '
                 'lines (seq %s..%s) and %d cut; afterwards %s' % (
                     m.get('calls', '?'), m.get('maxcall', '?'), m.get('slow20', '?'), len(seqs),
                     seqs[0] if seqs else '-', seqs[-1] if seqs else '-', cut,
                     'the console works' if alive else 'NO ANSWER'))
        since = self.p.mark()
        self.p.send(2, 'LCDFLOOD 5000')
        self.p.note('not reading for 8 s')
        time.sleep(8)
        self.p.collect(2)
        end = self.p.reply(2, 'LCDFLOODEND', 3, since)
        if not end:
            end = self.result('LCDFLOODEND')
        rows = sum(1 for l in self.p.lines[since:] if l.ch == 0 and l.text.startswith('L '))
        self.rec('WR4b', 'the host not reading: the LCD stream (NAV47\'s case)', 'UNKNOWN', 'MEASURED',
                 '%s; the PC then got %d rows' % (end.text if end else 'no LCDFLOODEND', rows), ('yes', '-'))
        self.p.send(2, 'FLOOD 5000 128')
        time.sleep(0.1)
        self.p.close()
        time.sleep(7)
        self.p.open()
        self.p.collect(0.5)
        r = self.cmd('RESULT', expect='FLOODEND', timeout=3)
        self.rec('WR5', 'writes with the port closed (DTR down)', 'NOT DOC', 'MEASURED', r or 'no FLOODEND')

    def cable(self):
        if not self.manual:
            return
        self.log('== no cable (section 7)')
        self.p.close()
        input('\n  >>> unplug the USB cable, wait 5 s, plug it back in, then press Enter here ')
        self.p.open(wait=60)
        self.p.collect(1.0)
        self.report(quiet=True)
        st = re.findall(r'STATE active (-?\d+)->(-?\d+)', self.calclog)
        off = any(b == '0' for a, b in st)
        self.rec('T3', 'SERIO_IS_ACTIVE 0 without the cable', 'DOC', 'PASS' if off else 'FAIL',
                 'SERTEST saw: %s' % ', '.join('%s->%s' % s for s in st), ('yes', 'yes'))

    def report(self, quiet=False):
        since = self.p.mark()
        self.p.send(2, 'REPORT')
        self.p.reply(2, 'REPORTEND', 30, since)
        txt = [l.text for l in self.p.lines[since:] if l.ch == 2 and re.match(r'\s*\d+ ', l.text)]
        self.calclog = '\n'.join(txt)
        if not quiet:
            self.log('== the calculator\'s own log (REPORT)')
            for t in txt:
                self.log('    ' + t)

    def finish(self):
        self.report()
        st = re.findall(r'STATE active (-?\d+)->(-?\d+) connected (-?\d+)->(-?\d+)', self.calclog)
        drops = sum(1 for s in st if s[2] == '1' and s[3] == '0')
        ups = sum(1 for s in st if s[2] == '0' and s[3] == '1')
        self.rec('W5', 'SERIO_IS_CONNECTED follows the host\'s DTR (open / close)', 'DOC',
                 'PASS' if drops >= 2 and ups >= 2 else 'FAIL',
                 'SERTEST saw connected 1->0 %d times, 0->1 %d times (the PC closed the port 2-3 times)' % (drops, ups),
                 ('yes', 'yes'))
        wakes = re.findall(r'^\s*\d+ WAKE (\S+)', self.calclog, re.M)
        if wakes:
            self.log('  the wakes during NOWAKE: ' + ', '.join('%s %d' % (w, wakes.count(w)) for w in sorted(set(wakes))))
        self.log('== QUIT: SERTEST puts the user\'s DMCP settings back (section 7)')
        since = self.p.mark()
        self.p.send(2, 'QUIT')
        q = self.p.reply(2, 'QUIT putting back', 3, since)
        self.p.collect(3)
        done = any(l.text.startswith('QUIT done') for l in self.p.lines[since:])
        gone = not os.path.exists(self.p.path)
        en = self.boot.get('IS_ENABLED')
        ok = (en == 0 and gone) or (en == 1 and done)
        self.rec('T5', 'leaving: the user\'s settings put back', 'DOC', 'PASS' if ok else 'INFO',
                 '%s; then: %s' % (q.text if q else 'no QUIT answer',
                                   'the port vanished (console off, as before)' if gone else
                                   'the port stayed' + (' and "QUIT done" arrived' if done else '')), ('yes', 'yes'))
        if self.manual:
            a = input('\n  >>> in DMCP\'s menu, is the serial console item as it was before SERTEST? [y/n] ')
            self.rec('T5m', 'DMCP\'s menu after leaving', 'DOC', 'PASS' if a.strip().lower().startswith('y')
                     else 'FAIL', 'Victor checked the menu: %s' % a.strip())
        self.rec('B', 'B: the bootloader', 'DOC', 'SKIP', 'not tested on purpose')

    def summary(self, started):
        self.log('')
        self.log('=' * 118)
        self.log('SUMMARY  (NAV47 / CHESS47: yes = used and working in Victor\'s tests on the DM42n, code = in the '
                 'code, not exercised)')
        self.log('%-5s %-62s %-8s %-9s %-5s %s' % ('item', 'what', 'sheet', 'result', 'NAV47', 'CHESS47'))
        for rid, item, tag, result, detail, apps in self.res:
            self.log('%-5s %-62s %-8s %-9s %-5s %s' % (rid, item[:62], tag, result, apps[0], apps[1]))
        counts = {}
        for r in self.res:
            counts[r[3]] = counts.get(r[3], 0) + 1
        self.log('')
        self.log('totals: ' + ', '.join('%s %d' % kv for kv in sorted(counts.items())) +
                 '   (%.0f s)' % (time.time() - started))
        self.log('UNKNOWN 1 (wake on input): ' + next((r[4] for r in self.res if r[0] == 'T1'), 'not run'))
        self.log('UNKNOWN 2 (host not reading): ' + next((r[4] for r in self.res if r[0] == 'WR4'), 'not run'))
        self.log('             LCD stream: ' + next((r[4] for r in self.res if r[0] == 'WR4b'), 'not run'))


def quit_only(path):
    """--quit: end SERTEST from the PC (QUIT: your DMCP settings back, /SERTEST.TXT saved, the program ends)."""
    with open(os.devnull, 'w') as null:
        p = Port(path, null)
        p.open(wait=5)
        since = p.mark()
        p.send(2, 'QUIT')
        q = p.reply(2, 'QUIT', 3, since)
        print(q.text if q else 'no answer: is SERTEST running? (EXIT or F6 on the calculator ends it too)')
        p.collect(1.0)


def main():
    ap = argparse.ArgumentParser(description='DMCP5 3.58 serial console: test every item of the data sheet')
    ap.add_argument('port', nargs='?', default='/dev/ttyACM0')
    ap.add_argument('--manual', action='store_true', help='also a real key, unplugging the cable, DMCP\'s menu')
    ap.add_argument('--out', default='.', help='where the logs go')
    ap.add_argument('--quit', action='store_true', help='only end SERTEST on the calculator (QUIT), no test')
    a = ap.parse_args()
    if a.quit:
        return quit_only(a.port)
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M')
    lp = os.path.join(a.out, 'sertest_%s.log' % stamp)
    wp = os.path.join(a.out, 'sertest_%s_wire.txt' % stamp)
    started = time.time()
    with open(lp, 'w') as log, open(wp, 'w') as wire:
        port = Port(a.port, wire)
        t = Test(port, log, a.manual)
        t.log('SERTEST - DMCP5 Serial Console API (preliminary data sheet of 2026-11-06), tested item by item')
        t.log('date %s, port %s, %s' % (datetime.datetime.now().isoformat(' ', 'seconds'), a.port,
                                        'with the manual steps' if a.manual else 'automatic steps only'))
        try:
            t.start()
            t.ioctls()
            t.latency()
            t.channels()
            t.reads()
            t.writes()
            t.lcd()
            t.keys()
            t.wake()
            t.new_host()
            t.floods()
            t.cable()
            t.finish()
        except (KeyboardInterrupt, OSError) as e:
            t.log('stopped: %r' % e)
            if port.fd is not None:                    # end SERTEST too: its settings back, no reset needed
                try:
                    port.send(2, 'QUIT')
                    port.collect(1.0)
                    t.log('sent QUIT: SERTEST put your DMCP settings back and ended')
                except OSError:
                    pass
        t.summary(started)
        t.log('')
        t.log('log: %s   every line on the wire: %s' % (lp, wp))


if __name__ == '__main__':
    main()
