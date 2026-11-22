#!/usr/bin/env python3
"""
serial_monitor.py - 노트북에 꽂힌 시리얼 포트를 전부 한꺼번에 열어 한 화면에 보여 준다.

  ~/.venvs/serial/bin/python tools/serial_monitor.py                  (꽂힌 포트 전부: Uno + STM32들)
  ~/.venvs/serial/bin/python tools/serial_monitor.py --send START     (Uno에 START → 게임 없이 한 판)
  ~/.venvs/serial/bin/python tools/serial_monitor.py /dev/cu.usbmodem2101 /dev/cu.usbmodem2103   (포트 직접)

- 줄마다 어느 보드인지 붙인다: [UNO] [W]관악기 [H]하프 [D]드럼. 처음엔 포트 끝번호로 보이다가,
  그 보드가 보내는 줄(#conductor·BEAT → UNO, #S,W / W,ON → W ...)로 알아낸다.
- 저장: logs/<시각>_all.txt (모든 포트, 표시 붙음) / logs/<시각>_uno.txt (Uno 포트만 = 게임이 받는 것과 같음)
  → 검사는 _uno.txt 로: tools/check_serial_log.py logs/<시각>_uno.txt
- [모드]: 보드가 1초마다 보내는 #S 줄로 세 보드의 DMA 모드를 모아 바뀔 때마다 보여 준다.
  "(USB로만, Uno엔 안 옴)"이 붙으면 그 보드의 I2C가 Uno까지 안 가는 것.
- 게임(크롬)과 같은 포트를 동시에 열 수 없다. 게임 전에 Ctrl+C로 닫는다.
"""
import datetime
import glob
import os
import re
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    print("pyserial이 없습니다:  python3 -m venv ~/.venvs/serial && ~/.venvs/serial/bin/pip install pyserial")
    sys.exit(1)


def list_ports():
    return sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*") +
                  glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))


def guess_tag(line):
    """이 줄을 보낸 보드를 알아낸다. 모르면 None."""
    if line.startswith(("#conductor", "BEAT,", "COUNT,", "END", "#EVAL", "#UNO", "#START", "#start")):
        return "UNO"
    m = re.match(r"^#S,([WHD]),", line) or re.match(r"^#?([WHD]),(ON|OFF),", line)
    if m:
        return m.group(1)
    if line.startswith("#=== wind"):
        return "W"
    if line.startswith(("#DRUM", "#ADC=", "#HIT ")):
        return "D"
    if line.startswith("#S2(A1)"):
        return "H"
    return None


class Port:
    def __init__(self, path):
        self.path = path
        self.short = path.rsplit("modem", 1)[-1] if "modem" in path else os.path.basename(path)
        self.tag = None
        self.buf = b""
        self.ser = serial.Serial(path, 115200, timeout=0)

    def label(self):
        return f"[{self.tag}]" if self.tag else f"[{self.short}]"


def main(argv):
    send = None
    if "--send" in argv:
        i = argv.index("--send")
        send = argv[i + 1] if i + 1 < len(argv) and not argv[i + 1].startswith("/") else "START"
    paths = [a for a in argv if a.startswith("/dev/")] or list_ports()
    if not paths:
        print("[포트 없음] USB 케이블(데이터용인지)·보드 전원을 확인하세요.")
        sys.exit(1)

    ports = []
    for p in paths:
        try:
            ports.append(Port(p))
        except serial.SerialException as e:
            print(f"[열기 실패] {p}: {e.strerror or e}  (screen·시리얼 모니터·게임 탭이 잡고 있으면 닫기)")
    if not ports:
        sys.exit(1)

    os.makedirs("logs", exist_ok=True)
    stamp = datetime.datetime.now().strftime("%m%d_%H%M%S")
    all_path, uno_path = f"logs/{stamp}_all.txt", f"logs/{stamp}_uno.txt"
    print(f"[열림] {', '.join(p.path for p in ports)}")
    print(f"[저장] {all_path} (전부) / {uno_path} (Uno 포트만, 검사용)   Ctrl+C로 끝")

    modes = {}          # (inst, via) -> mode
    shown = None
    sent = False
    t_open = time.time()
    with open(all_path, "w", encoding="utf-8") as fall, open(uno_path, "w", encoding="utf-8") as funo:
        try:
            while True:
                got = False
                for p in list(ports):
                    try:
                        data = p.ser.read(512)
                    except serial.SerialException:
                        print(f"{p.label()} [포트가 끊김] {p.path}")
                        ports.remove(p)
                        continue
                    if not data:
                        continue
                    got = True
                    p.buf += data
                    while b"\n" in p.buf:
                        raw, p.buf = p.buf.split(b"\n", 1)
                        line = raw.decode("utf-8", "replace").strip()
                        if not line:
                            continue
                        g = guess_tag(line)
                        if g == "UNO" and p.tag != "UNO":
                            p.tag = "UNO"
                        elif g in ("W", "H", "D") and p.tag is None:
                            p.tag = g
                        print(f"{p.label():7s}{line}")
                        fall.write(f"{p.label()} {line}\n"); fall.flush()
                        if p.tag == "UNO":
                            funo.write(line + "\n"); funo.flush()
                        m = re.match(r"^#S,([WHD]),(DMA_ON|DMA_OFF)", line)
                        if m:
                            via = "UNO" if p.tag == "UNO" else "USB"
                            modes[(m.group(1), via)] = m.group(2)
                            now_shown = tuple(sorted(modes.items()))
                            if now_shown != shown:
                                shown = now_shown
                                parts, vals = [], set()
                                for inst in "WHD":
                                    u, s = modes.get((inst, "UNO")), modes.get((inst, "USB"))
                                    v = u or s
                                    if v:
                                        vals.add(v)
                                    parts.append(f"{inst}={v or '?'}" + ("" if u else (" (USB로만, Uno엔 안 옴)" if s else "")))
                                warn = "   ← 다름! B1로 맞추세요" if len(vals) > 1 else ""
                                print(f"[모드]  {'  '.join(parts)}{warn}")
                # Uno를 찾으면 START 한 번 (Uno는 포트를 열면 리셋 → 배너가 온 뒤)
                if send and not sent:
                    uno = next((p for p in ports if p.tag == "UNO"), None)
                    if uno is not None and time.time() - t_open > 2.5:
                        uno.ser.write((send + "\n").encode())
                        print(f"[보냄→UNO] {send}")
                        sent = True
                    elif uno is None and time.time() - t_open > 6:
                        print("[주의] Uno를 못 찾았습니다(#conductor 줄 없음). START를 못 보냈어요.")
                        sent = True
                if not got:
                    time.sleep(0.01)
        except KeyboardInterrupt:
            pass
    for p in ports:
        p.ser.close()
    print(f"\n[끝] {all_path}\n검사: ~/.venvs/serial/bin/python tools/check_serial_log.py {uno_path}")


if __name__ == "__main__":
    main(sys.argv[1:])
