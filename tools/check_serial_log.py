#!/usr/bin/env python3
"""
check_serial_log.py - 보드가 찍은 시리얼 줄이 리듬게임(나비야 정원 v1.0.5)과 맞는지 검사한다.

  python3 check_serial_log.py 터미널기록.txt [기록2.txt ...]
  python3 check_serial_log.py 터미널기록.txt --json      (결과를 JSON으로)

- 게임의 parseLine()과 같은 규칙으로 줄을 나누고, 맞지 않는 줄은 이유를 붙여 보여 준다.
- 나비야 채보(관악기 54 / 하프 32 / 드럼 63음)와 대조해서 판마다 악기별 P/G/B/M,
  음 틀림, 헛음, 쏠림(평균 오차), 흔들림(오차 표준편차)을 표로 낸다.
- 터미널이 붙이는 앞머리("[COM3] ", "13:20:01.123 -> ")는 떼고 읽는다.
- 여러 파일(보드마다 따로 받은 기록)을 주면 한 판으로 합친다.
"""
import json
import math
import re
import sys

# ---------------------------------------------------------------- 게임 설정 (index.html CONFIG와 같게)
JUDGE_MS = {"default": {"P": 50, "G": 120, "B": 250}, "W": {"P": 100, "G": 200, "B": 400}}
INSTS = ["W", "H", "D"]
INST_KO = {"W": "관악기", "H": "하프", "D": "드럼"}
VALID_PITCH = {"W": ["DO", "RE", "MI", "FA", "SOL"], "H": ["DO", "RE", "MI", "FA", "SOL"], "D": ["KICK", "SNARE"]}
BARS = [
    ("솔1 미1 미2", "C"), ("파1 레1 레2", "G7"), ("도1 레1 미1 파1", "C"), ("솔1 솔1 솔2", "C"),
    ("솔1 미1 미1 미1", "C"), ("파1 레1 레2", "G7"), ("도1 미1 솔1 솔1", "C"), ("미1 미1 미2", "C"),
    ("레1 레1 레1 레1", "G7"), ("레1 미1 파2", "G7"), ("미1 미1 미1 미1", "C"), ("미1 파1 솔2", "C"),
    ("솔1 미1 미2", "C"), ("파1 레1 레2", "G7"), ("도1 미1 솔1 솔1", "C"), ("미1 미1 미2", "C"),
]
DRUM_BAR, DRUM_LAST_BAR = "쿵1 짝1 쿵1 짝1", "쿵1 짝1 쿵2"
CHORDS = {"C": ["도", "미"], "G7": ["솔", "레"]}
K2E = {"도": "DO", "레": "RE", "미": "MI", "파": "FA", "솔": "SOL", "쿵": "KICK", "짝": "SNARE"}
TOTAL_BEATS = len(BARS) * 4


def build_chart():
    notes = []
    for bi, (melody, chord) in enumerate(BARS):
        b0, s = bi * 4, 0
        for tok in melody.split():
            ln = int(tok[1:]); notes.append({"inst": "W", "beat": b0 + s, "len": ln, "pitch": K2E[tok[0]]}); s += ln
        for i, p in enumerate(CHORDS[chord]):
            notes.append({"inst": "H", "beat": b0 + i * 2, "len": 2, "pitch": K2E[p]})
        ds = 0
        for tok in (DRUM_LAST_BAR if bi == len(BARS) - 1 else DRUM_BAR).split():
            ln = int(tok[1:]); notes.append({"inst": "D", "beat": b0 + ds, "len": ln, "pitch": K2E[tok[0]]}); ds += ln
    notes.sort(key=lambda n: (n["beat"], INSTS.index(n["inst"])))
    return notes


# ---------------------------------------------------------------- 줄 파싱 (index.html parseLine과 같은 규칙)
PREFIX = re.compile(r"^(\[[^\]]*\]\s*)?(\d{1,2}:\d{2}:\d{2}(\.\d+)?\s*(->)?\s*)?")
INT = re.compile(r"^[+-]?\d+$")


def parse_line(raw):
    line = raw.rstrip("\r\n").strip()
    if not line:
        return {"kind": "empty"}
    f = [x.strip() for x in line.split(",")]
    if f[0] == "BEAT":
        if len(f) == 2 and f[1].isdigit():
            return {"kind": "beat", "n": int(f[1])}
        return {"kind": "bad", "line": line, "why": "BEAT 뒤에는 0 이상 정수 하나"}
    if f[0] == "MODE":
        if len(f) == 2 and f[1] in ("DMA_ON", "DMA_OFF"):
            return {"kind": "mode", "label": f[1]}
        return {"kind": "bad", "line": line, "why": "MODE,DMA_ON 또는 MODE,DMA_OFF만"}
    if f[0] in ("COUNT", "END", "START", "STOP"):            # Uno 지휘자 줄 (게임 v1.0.5는 무시)
        return {"kind": "uno", "line": line}
    if f[0] in INSTS:
        if len(f) not in (6, 7):
            return {"kind": "bad", "line": line, "why": f"칸 수 {len(f)}개 (6개, 등급까지 7개)"}
        inst, ev, pitch, vel, beat, off = f[:6]
        grade = f[6] if len(f) == 7 else None
        why = None
        if ev not in ("ON", "OFF"): why = "이벤트는 ON/OFF"
        elif pitch not in VALID_PITCH[inst]: why = f"{inst}의 음은 {'/'.join(VALID_PITCH[inst])} (대문자)"
        elif not vel.isdigit() or int(vel) > 100: why = "세기는 0~100 정수"
        elif not beat.isdigit(): why = "박번호는 0 이상 정수 (-1 = 박자를 못 받음)" if beat.startswith("-") else "박번호는 0 이상 정수"
        elif not INT.match(off): why = "오차ms는 정수 (+12, -8)"
        elif grade is not None and not re.match(r"^[PGBMX]$", grade): why = "등급은 P/G/B/M/X 한 글자"
        if why:
            return {"kind": "bad", "line": line, "why": why}
        return {"kind": "event", "inst": inst, "ev": ev, "pitch": pitch, "vel": int(vel), "beat": int(beat),
                "offset": int(off), "grade": grade}
    if line.startswith("#"):
        return {"kind": "comment", "line": line}
    return {"kind": "bad", "line": line, "why": "모르는 줄 (게임은 무시)"}


def read_lines(paths):
    out = []
    for p in paths:
        with open(p, encoding="utf-8", errors="replace") as fh:
            for i, raw in enumerate(fh, 1):
                s = raw.rstrip("\r\n")
                m = parse_line(s)
                if m["kind"] == "bad":                        # 터미널 앞머리를 떼고 한 번 더
                    s2 = PREFIX.sub("", s.strip(), count=1)
                    if s2 != s.strip():
                        m2 = parse_line(s2)
                        if m2["kind"] != "bad": m = m2
                m["src"] = f"{p}:{i}"
                out.append(m)
    return out


# ---------------------------------------------------------------- 판 나누기 + 판정 (handleLine과 같은 규칙)
def split_runs(items):
    runs, cur, maxbeat = [], None, -1

    def new():
        return {"label": None, "beats": [], "events": [], "comments": [], "bad": []}

    for m in items:
        k = m["kind"]
        if k == "empty": continue
        if cur is None: cur = new()
        start_new = False
        if k == "beat" and m["n"] == 0 and (cur["beats"] or cur["events"]): start_new = True
        if k == "mode" and cur["events"] and maxbeat >= 8: start_new = True
        if k == "event" and m["ev"] == "ON" and m["beat"] < maxbeat - 8: start_new = True
        if start_new:
            runs.append(cur); cur = new(); maxbeat = -1
        if k == "beat": cur["beats"].append(m["n"]); maxbeat = max(maxbeat, m["n"])
        elif k == "mode": cur["label"] = m["label"]
        elif k == "event": cur["events"].append(m); maxbeat = max(maxbeat, m["beat"])
        elif k == "comment": cur["comments"].append(m)
        elif k == "bad": cur["bad"].append(m)
    if cur and (cur["beats"] or cur["events"] or cur["bad"]):
        runs.append(cur)
    return runs


def judge_run(run):
    notes = [dict(n, grade=None, offset=None, wrong=False) for n in build_chart()]
    per = {i: {"P": 0, "G": 0, "B": 0, "M": 0, "wrong": 0, "stray": 0, "off": 0, "on": 0, "offsets": []} for i in INSTS}
    for m in sorted(run["events"], key=lambda e: e["beat"] + e["offset"] / 1000.0):
        s = per[m["inst"]]
        if m["ev"] == "OFF":
            s["off"] += 1; continue
        s["on"] += 1
        note = next((n for n in notes if n["inst"] == m["inst"] and n["beat"] == m["beat"]), None)
        w = JUDGE_MS.get(m["inst"], JUDGE_MS["default"]); a = abs(m["offset"])
        grade = m["grade"] or ("P" if a <= w["P"] else "G" if a <= w["G"] else "B" if a <= w["B"] else None)
        if note is None or note["grade"] is not None or grade is None or grade == "X":
            s["stray"] += 1; continue
        wrong = m["pitch"] != note["pitch"]
        note["grade"] = "B" if wrong else grade
        note["offset"] = m["offset"]; note["wrong"] = wrong
        if wrong: s["wrong"] += 1
        s["offsets"].append(m["offset"])
    for n in notes:
        if n["grade"] is None: n["grade"] = "M"
        per[n["inst"]][n["grade"]] += 1
    res = {}
    for i in INSTS:
        s = per[i]; o = s.pop("offsets")
        total = sum(1 for n in notes if n["inst"] == i)
        s["notes"] = total
        s["mean"] = round(sum(o) / len(o)) if o else None
        s["std"] = round(math.sqrt(sum((x - sum(o) / len(o)) ** 2 for x in o) / len(o))) if len(o) > 1 else None
        res[i] = s
    bt = run["beats"]
    gaps = [b for a, b in zip(bt, bt[1:]) if b != a + 1]
    return {"label": run["label"], "beats": len(bt), "beat_first": bt[0] if bt else None, "beat_last": bt[-1] if bt else None,
            "beat_jumps": gaps[:5], "per": res, "notes": notes,
            "bad": [{"src": b["src"], "line": b["line"], "why": b["why"]} for b in run["bad"]],
            "comments": len(run["comments"])}


def r5(x):
    return None if x is None else int(round(x / 5.0) * 5)


def report(runs_res):
    out = []
    for k, r in enumerate(runs_res, 1):
        out.append(f"\n## 판 {k}: {r['label'] or '모드 줄 없음 (게임 시작 화면에서 고른 이름)'}")
        if r["beats"]:
            jump = f", 건너뛴 곳 {r['beat_jumps']}" if r["beat_jumps"] else ""
            out.append(f"BEAT 줄 {r['beats']}개 ({r['beat_first']}~{r['beat_last']}{jump})")
        else:
            out.append("BEAT 줄 없음 → 이 기록만으로는 게임이 Uno 박자를 못 받는다 (Uno 포트를 같이 열면 됨)")
        out.append("")
        out.append("| 악기 | 노트 | ON 줄 | P | G | B | M | 음 틀림 | 헛음 | 쏠림 | 흔들림 | OFF 줄 |")
        out.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
        for i in INSTS:
            s = r["per"][i]
            m = "-" if s["mean"] is None else f"{r5(s['mean']):+d}ms"
            sd = "-" if s["std"] is None else f"{r5(s['std'])}ms"
            out.append(f"| {INST_KO[i]} | {s['notes']} | {s['on']} | {s['P']} | {s['G']} | {s['B']} | {s['M']} | {s['wrong']} | {s['stray']} | {m} | {sd} | {s['off']} |")
        if r["bad"]:
            out.append(f"\n게임이 버리는 줄 {len(r['bad'])}개 (앞 10개):")
            for b in r["bad"][:10]:
                out.append(f"- `{b['line'][:60]}` — {b['why']} ({b['src']})")
        if r["comments"]:
            out.append(f"\n`#` 줄 {r['comments']}개는 디버그용이라 게임이 무시한다 (정상).")
    return "\n".join(out)


def main(argv):
    paths = [a for a in argv if not a.startswith("--")]
    if not paths:
        print(__doc__); return 2
    runs = split_runs(read_lines(paths))
    res = [judge_run(r) for r in runs]
    if "--json" in argv:
        for r in res: r.pop("notes")
        print(json.dumps(res, ensure_ascii=False, indent=1))
    else:
        print(f"# 시리얼 기록 검사 ({', '.join(paths)}) — 판 {len(res)}개")
        print(report(res))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
