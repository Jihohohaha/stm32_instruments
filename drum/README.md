# 드럼 (STM32 #3)

피에조 패드 2개(킥·스네어)를 친 순간과 세기를 읽고, 플래시에 넣은 샘플로 소리를 낸다.

| 항목 | 값 |
| --- | --- |
| 보드 | NUCLEO-F103RB, 64MHz (HSI/2 × 16), APB1 32MHz. **HAL 없이 레지스터만** 쓴다(자체 SysTick 1ms) |
| 센서 | 피에조(1MΩ) 킥 PA0(A0), 스네어 PA1(A1) → ADC1 채널 0·1 |
| 센서 DMA | TIM3 TRGO 8kHz가 ADC1 scan(0→1)을 깨운다 → DMA1 채널1 circular `adc_buffer[2 × 16프레임 × 2]` → 반/전체 완료 인터럽트에서 `adc_process()`가 타격을 판정한다. 기준값(60)을 넘으면 2ms 동안 피크를 찾아 세기를 정한다 |
| 소리 | PB6(D10) TIM4_CH1 PWM(250kHz, 8번 반복 = 31.25kHz 샘플). TIM4 업데이트마다 DMA1 채널7이 `pwm_buffer` 값을 CCR1에 넣는다 → RC 필터 → PAM8403 |
| 박자 입력 | PB5(D4), EXTI5 올라가는 순간. Uno D8 펄스를 1kΩ+2kΩ 분압으로 받는다. GND 공통 |
| DMA ON/OFF | B1(PC13). LD2 켜짐 = DMA OFF. 고장(FAULT)이면 LD2가 깜빡인다 |
| PC 전송 | **I²C로 Uno에(11/22 구조 변경)**: I2C1 리맵 PB8(D15)=SCL, PB9(D14)=SDA → Uno A5/A4, 4.7kΩ 풀업(5V), PB8·PB9는 5V 내성(FT). Uno가 USB 한 줄로 노트북에 넘긴다. USART2(PA2·PA3, 115200)에도 같은 줄을 디버그용으로 찍는다. 게임 줄 형식은 `D,ON,<KICK|SNARE>,<세기>,<박>,<±오차ms>` |

## 파일

| 파일 | 내용 |
| --- | --- |
| `stm_drum/` | 드럼 CubeIDE 프로젝트(`proj_05`). 원본은 커밋 `ba673a3` |
| `stm_drum/Core/Src/main.c` | **합주 패치 적용판(11/22).** 바뀐 줄은 `git diff ba673a3 -- drum/stm_drum/Core/Src/main.c` |
| `stm_drum/Core/Src/ensemble_link.c`, `Core/Inc/ensemble_link.h` | 세 보드 공통 연결 모듈(`common/`과 같은 파일) |
| `stm_drum/Core/Inc/ensemble_link_cfg.h` | 드럼용 설정: `LINK_INST 'D'`, `LINK_LED_ENABLE 0`, `LINK_LATENCY_MS 2`, I²C 켬 |

**빌드:** CubeIDE에서 File → Import → Existing Projects into Workspace로 `drum/stm_drum`을 연다. 그다음 Build한다. `Core/Src`에 있는 파일은 저절로 같이 빌드된다.

**`.ioc`로 Generate Code를 하면 안 된다.** 주변장치를 모두 `main.c`가 레지스터로 설정한다(`DRUM_README.txt`). 그래서 `stm32f1xx_it.c`가 거의 비어 있다.

## 합주 패치 (11/22)

소리 코드는 한 글자도 바꾸지 않았다. `voice_start`, `audio_fill`, `audio_dma_init`, `DMA1_Channel7_IRQHandler`가 원본과 같다(스크립트로 확인함).

1. **폴링 경로를 추가했다(DMA OFF).** DMA가 하던 "ADC 값 옮기기"만 CPU가 직접 한다.
   - 메인 루프가 TIM3 넘침 플래그(8kHz)를 본다.
   - 채널 0·1을 하나씩 SWSTART → EOC 기다림으로 읽는다.
   - 같은 `adc_buffer` 자리에 쓰고, 반 버퍼가 차면 같은 `adc_process()`를 부른다.
   - 타이머 주기, 버퍼, 판정 함수는 두 모드가 같다.
2. **B1로 모드를 바꾼다.** `link_on_mode_change()`가 DMA1 채널1과 ADC 설정(SCAN·EXTSEL·DMA 비트)을 바꾼다. 곡 도중에는 바뀌지 않는다.
   - **원래 B1 테스트음(킥 → 스네어 → 동시)은 껐다**(`DRUM_B1_TEST 0`). 테스트음은 시리얼에서 `T`, 수동 타격은 `K`/`S`/`B`로 그대로 된다.
3. **타격이 확정되는 순간 게임 줄을 보낸다.** `hit_push()`에서 `link_note_on("KICK", 세기)`를 부른다.
   - 세기 0~100은 원래 gain과 같은 비율이다.
   - 2ms 피크 창만큼 늦게 확정되므로 `LINK_LATENCY_MS 2`로 오차에서 뺀다.
4. **CPU 사용률을 잰다.**
   - DMA ON: `DMA1_Channel1_IRQHandler` 본문(`adc_dma_isr_body`)을 감싼다.
   - DMA OFF: 변환 대기와 처리를 감싼다.
   - 폴링이 늦어 놓친 프레임은 `miss`로 센다. 1초마다 `#S,D,DMA_OFF,cpu=..,miss=..` 줄이 나온다.
5. **`link_millis()`는 이 코드의 `milliseconds`(자체 SysTick)를 쓴다.** HAL이 없어도 된다.
6. **LD2 역할을 바꿨다.** 원래는 칠 때 0.25초 켜졌고, 지금은 DMA OFF 표시다. 고장 깜빡임은 그대로다.
7. **진단 줄(`ADC=..`, `HIT KICK..`, 시작 안내, TEST..) 앞에 `#`을 붙였다.** 게임은 이 줄들을 무시한다.

## 확인 결과 (막을 것 / 사소한 것)

**막을 것**

- [x] 게임 줄이 없었다 → 패치 3번
- [x] 폴링 경로가 없어 ON/OFF 비교를 할 수 없었다 → 패치 1·2번
- [x] B1이 테스트음에 묶여 있었다 → 패치 2번 (팀 합의: 세 보드 모두 B1 = DMA 전환)
- [x] 박자 입력이 없었다 → 공통 모듈이 PB5(EXTI5)로 받는다. 드럼은 EXTI9_5 핸들러가 없어서 그대로 쓸 수 있다
- [x] 레포 `drum/stm_drum/`에 원본을 올렸다(커밋 ba673a3). 패치는 그 위에 적용했다

**사소한 것 / 확인 필요**

- DMA ON에서 원래 코드의 고장 검사(반/전체 플래그가 함께 서면 `FAULT_MISSED_ADC`)는 그대로 둔다.
- DMA OFF에서는 진단 줄(0.5초마다 약 100글자, 약 9ms)을 찍는 동안 프레임을 놓친다. 이것이 폴링의 약점이고, 그대로 `miss`에 잡힌다. 비교를 공정하게 하려면 두 모드 모두 `SERIAL_DIAGNOSTICS`를 같은 값으로 둔다(둘 다 1 또는 둘 다 0).
- 문턱값 `KICK_THRESHOLD`·`SNARE_THRESHOLD` 60은 실물 패드로 다시 맞춘다(원본 주석 그대로).
- 피에조 "모듈"의 보호 회로와 1N5817 다이오드가 겹치는지 확인 필요(시작 문서).

## 시험한 것

- PC에서 ST 공식 CMSIS 헤더로 패치를 문법 검사했다. 경고와 오류 모두 0이다(`drum_samples.h`는 가짜로 대신함).
- 보드에서는 아직 안 돌려 봤다. 확인 순서:
  1. 굽고 시리얼에서 `MODE,DMA_ON`과 `#DRUM PIEZO ENABLED`를 확인한다.
  2. 패드를 치면 소리가 나고 `#HIT KICK..`이 나오는지 본다. Uno가 없으면 `#D,ON,KICK,no beat`로 나온다.
  3. B1을 누르면 LD2가 켜지고 `MODE,DMA_OFF`가 나오는지, 패드를 치면 여전히 소리가 나는지 본다.
  4. `#S` 줄의 cpu·miss 값을 두 모드에서 비교한다.
