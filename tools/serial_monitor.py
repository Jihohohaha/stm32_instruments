#!/usr/bin/env python3
"""
serial_monitor.py - Uno 포트로 들어오는 줄을 보고, 파일로 저장하고, 세 보드의 DMA 모드를 한 줄로 보여 준다.

  pip3 install pyserial                    (처음 한 번)
  python3 tools/serial_monitor.py          (포트를 자동으로 찾음: /dev/cu.usbmodem*, /dev/cu.usbserial*)
  python3 tools/serial_monitor.py /dev/cu.usbmodem14101
  python3 tools/serial_monitor.py --send START     (Uno에 START를 보내고 시작. 게임 없이 한 판 돌릴 때)

- 받은 줄은 그대로 화면에 찍고 logs/<시각>.txt 에 저장한다 → tools/check_serial_log.py 로 바로 검사.
- 보드가 1초마다 보내는 #S 줄(#S,D,DMA_OFF,cpu=..,miss=..)로 보드별 현재 모드를 모아서 바뀔 때마다
  [모드] W=DMA_ON  H=DMA_ON  D=DMA_OFF  ← 다름!  처럼 알려 준다. 세 보드의 LD2가 같은지 여기서 확인한다.
- 게임(크롬)과 동시에 같은 포트를 열 수 없다. 게임을 하기 전에 Ctrl+C로 닫는다.
"""
import datetime
import glob
import os
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    print("pyserial이 없습니다: pip3 install pyserial")
    sys.exit(1)


def find_port():
    c = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    if not c:
        print("포트를 못 찾았습니다. Uno가 꽂혀 있는지, 인자로 포트를 직접 주세요.")
        sys.exit(1)
    if len(c) > 1:
        print("포트가 여러 개입니다. 첫 번째를 엽니다:", c, "(다르면 인자로 주세요)")
    else:
        print("포트:", c[0])
    return c[0]


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    send = None
    if "--send" in argv:
        i = argv.index("--send")
        send = argv[i + 1] if i + 1 < len(argv) else "START"
        args = [a for a in args if a != send]
    port = args[0] if args else find_port()
    os.makedirs("logs", exist_ok=True)
    path = os.path.join("logs", datetime.datetime.now().strftime("%m%d_%H%M%S") + ".txt")
    modes, last_shown = {}, None
    try:
        ser = serial.Serial(port, 115200, timeout=0.2)
    except serial.SerialException as e:
        now = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*"))
        print(f"[포트 열기 실패] {port}: {e.strerror or e}")
        print("지금 보이는 포트:", now if now else "없음 (USB 케이블·보드 전원 확인, 데이터 케이블인지 확인)")
        print("다른 프로그램(screen, 아두이노 시리얼 모니터, 게임 탭)이 잡고 있어도 안 열립니다.")
        sys.exit(1)
    with ser, open(path, "w", encoding="utf-8") as log:
        print(f"[열림] {port}  → 저장: {path}  (Ctrl+C로 끝)")
        time.sleep(2.0)                      # Uno는 포트를 열면 리셋된다
        if send:
            ser.write((send + "\n").encode())
            print(f"[보냄] {send}")
        buf = b""
        try:
            while True:
                buf += ser.read(256)
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").strip()
                    if not line:
                        continue
                    print(line)
                    log.write(line + "\n"); log.flush()
                    f = line.split(",")
                    if line.startswith("#S,") and len(f) >= 3:
                        modes[f[1]] = f[2]
                        shown = tuple(sorted(modes.items()))
                        if shown != last_shown:
                            last_shown = shown
                            same = len(set(modes.values())) == 1
                            text = "  ".join(f"{k}={v}" for k, v in sorted(modes.items()))
                            print(f"[모드] {text}" + ("" if same else "   ← 다름! B1로 맞추세요") +
                                  ("" if len(modes) == 3 else f"   (아직 {3 - len(modes)}대 소식 없음)"))
        except KeyboardInterrupt:
            pass
    print(f"\n[끝] 저장됨: {path}\n검사: python3 tools/check_serial_log.py {path}")


if __name__ == "__main__":
    main(sys.argv[1:])
