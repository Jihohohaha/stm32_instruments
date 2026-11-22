# stm32_instruments — 센서 악기 합주

경희대학교 임베디드시스템설계 2차 팀 프로젝트. STM32 세 대가 관악기·하프·드럼이 되고, Arduino Uno가 지휘자로 박자를 준다. 곡은 「나비야」 16마디.
채점 핵심은 **DMA와 레지스터 직접 조작, DMA를 쓴 이점을 숫자로 보여주기**다. 같은 곡을 DMA 켜고/끄고(폴링) 연주해서 놓친 샘플, CPU 사용률(DWT), 박자 오차 흔들림을 비교한다.

```
                  박자 펄스 (Uno D8, 10ms, 분압 → 각 보드 D4=PB5)
   Uno 지휘자 ─────────────┬─────────────┬─────────────┐
   I²C 슬레이브 0x08       ▼             ▼             ▼
   (A4 SDA / A5 SCL)   관악기 F103    하프 F103     드럼 F103
        ▲              마이크+조이    레이저+CdS    피에조 2패드
        │                  │ W,ON,…      │ H,ON,…      │ D,ON,…
        └──────────────────┴─────────────┴─────────────┘
              I²C (각 보드 D14=PB9 SDA, D15=PB8 SCL, 마스터)
        │
        │ USB 한 줄: BEAT,n + 세 보드 줄 + #EVAL(Uno 채점)
        ▼
   PC 리듬게임 「나비야 정원」 (Web Serial, 포트 1개. 판정은 각 보드)
```

연결 구조(11/22 확정): USB 허브 없이 **Uno만 노트북에** 꽂는다. 각 STM32는 USB 충전기·보조배터리로 전원만 넣는다. 핀맵과 줄 형식은 [`docs/serial-protocol.md`](docs/serial-protocol.md)에 있다.

## 폴더

| 폴더 | 내용 | 담당 | 상태 |
| --- | --- | --- | --- |
| `wind/` | 관악기 STM32 (CubeIDE 프로젝트) | 박지호 + 공동 담당 | v1 올림, v2(DMA ON/OFF·박자) 올릴 예정 |
| `harp/` | 하프 STM32 | 하프 담당 | 원본 `Src/` + 합주 패치(센서 DMA 추가, I²C·박자) — 보드 미검증 |
| `drum/` | 드럼 STM32 | 드럼 담당 | 프로젝트 전체 + 합주 패치(폴링 경로 추가, I²C·박자) — 보드 미검증 |
| `uno/` | Uno 지휘자 | 박지호 | conductor v1 = v0 + I²C 수신·USB 전달·#EVAL (실물 미검증) |
| `game/nabiya-garden/` | PC 리듬게임 (HTML + Canvas 2D) | 박지호 | v1.0.5 |
| `common/` | 세 보드 공통 연결 모듈 `ensemble_link` (B1 DMA 전환, 박자, 게임 줄, CPU 사용률, I²C) | 박지호 | 각 보드 프로젝트에 같은 파일 복사 |
| `tools/` | `serial_monitor.py`(Uno 포트 보기·저장·보드별 모드), `check_serial_log.py`(기록 검사) | 박지호 | |
| `docs/` | 시리얼 형식, 팀원 코드 확인 목록 | | |

## 먼저 읽을 것

- 보드끼리, 보드와 게임이 주고받는 줄: [`docs/serial-protocol.md`](docs/serial-protocol.md)
- 코드를 올린 뒤 확인하는 것(형식 / DMA ON·OFF / 아두이노): [`docs/review-checklist.md`](docs/review-checklist.md)

## 팀원: 코드 올리는 법

1. 이 레포의 협업자 초대를 수락한다.
2. 자기 악기 폴더(`harp/`, `drum/`)에 CubeIDE 프로젝트 폴더를 통째로 넣는다. `Debug/`는 자동으로 빠진다.
3. 커밋 메시지에 무엇을 바꿨는지 한 줄. 예) `drum: DMA OFF 모드 추가`

GitHub 웹에서 폴더째 끌어다 놓아도 된다(Add file → Upload files).

## 게임 실행

```
cd game/nabiya-garden
python3 -m http.server 8000
```

크롬/엣지에서 `http://localhost:8000`. 키보드 A·S·D로 연습, T 자동 연주, R 무작위 데모, L 로그 재생. 자세한 건 `game/nabiya-garden/README.txt`.

## 보드·공통 사항

- 보드: NUCLEO-F103RB. F1은 GPIO가 CRL·CRH, ADC는 ADC1만 DMA(DMA1 채널1).
- 소리: 악기마다 자기 보드 PWM + PAM8403 + 스피커. 공통 소리 드라이버는 없다.
- 연결 구조(허브로 보드마다 PC 직결 / Uno가 I²C로 모아서)는 미정. 게임은 둘 다 받는다.

## 기여 표기

보고서 기준과 같다. 사람이 쓴 코드는 "구현", AI(Claude)가 낸 것은 "설명·검토·제안"으로 적고, 커밋 기록으로도 남긴다. 게임 그림 일부는 Higgsfield로 생성했다(`game/nabiya-garden/assets/`).
