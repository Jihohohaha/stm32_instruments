# 하프 (STM32 #2)

레이저 모듈 4개와 CdS GL5528 4개로 줄 4개(도·레·미·솔)를 만든다. 손이 레이저를 가리면 줄을 친 것이다. 마디마다 화음을 1박과 3박에 친다.

| 항목 | 값 |
| --- | --- |
| 보드 | NUCLEO-F103RB, 64MHz (HSI/2 × 16), APB1 32MHz |
| 센서 | CdS 4개 → PA0(A0)·PA1(A1)·PA4(A2)·PB0(A3) = ADC1 채널 0·1·4·8 → 도·레·미·솔 |
| 소리 | PA6(D12) TIM3_CH1 PWM. TIM4 업데이트(32kHz)마다 DMA1 채널7이 `audio_buf` 값을 CCR1에 넣는다. 반/전체 완료 인터럽트에서 끊긴 줄의 사인파를 더해 화음을 만든다 |
| 박자 입력 | PB5(D4), EXTI5 올라가는 순간. Uno D8 펄스를 1kΩ+2kΩ 분압으로 받는다. GND 공통 |
| DMA ON/OFF | B1(PC13). LD2 켜짐 = DMA OFF |
| PC 전송 | **I²C로 Uno에(11/22 구조 변경)**: I2C1 리맵 PB8(D15)=SCL, PB9(D14)=SDA → Uno A5/A4, 4.7kΩ 풀업(5V), PB8·PB9는 5V 내성(FT). Uno가 USB 한 줄로 노트북에 넘긴다. USART2(PA2·PA3, 115200)에도 같은 줄을 디버그용으로 찍는다. 게임 줄 형식은 `H,ON,<DO|RE|MI|SOL>,0,<박>,<±오차ms>` |

## 파일

| 파일 | 내용 |
| --- | --- |
| `Src/main.c` | **합주 패치 적용판(11/22).** 원본은 커밋 `66392e2`(yeoniix 업로드). 바뀐 줄은 `git diff 66392e2 -- harp/Src/main.c` |
| `Src/ensemble_link.c`, `Inc/ensemble_link.h` | 세 보드 공통 연결 모듈(`common/`과 같은 파일) |
| `Inc/ensemble_link_cfg.h` | 하프용 설정: `LINK_INST 'H'`, I²C 켬 |
| 나머지 `Src/*.c` | 원본 그대로 |

**하프 담당이 빌드하는 법:** 자기 CubeIDE 프로젝트에 `Src/main.c`, `Src/ensemble_link.c`는 `Core/Src`로, `Inc/` 두 파일은 `Core/Inc`로 복사한다. 그다음 빌드한다. 이 폴더에는 `Inc/main.h`, `.ioc`, `Drivers/`가 없어서 이 폴더만으로는 빌드할 수 없다. **프로젝트 전체를 올려 주면** 다음부터는 바로 빌드된다.

## 합주 패치 (11/22)

소리 코드는 한 글자도 바꾸지 않았다. `sound_test_init`부터 `DMA1_Channel7_IRQHandler`까지가 원본과 같다(스크립트로 확인함).

1. **광센서 DMA 읽기를 추가했다(DMA ON).**
   - ADC1을 scan 모드(채널 0→1→4→8)와 연속 변환으로 돌린다.
   - DMA1 채널1이 circular 모드로 `adc_dma_buf[8세트 × 4채널]`을 계속 채운다.
   - 전부 레지스터로 직접 설정한다(CCR·CPAR·CMAR·CNDTR, SQR1·SQR3, CR1 SCAN, CR2 CONT·DMA).
   - 우선순위는 소리 DMA(채널7, PL=높음)보다 낮은 PL=중간으로 둬서 소리를 방해하지 않는다.
2. **원래 폴링 읽기는 DMA OFF 모드로 그대로 쓴다.** `adc_read_avg()`가 채널마다 SWSTART → EOC 기다림을 8번씩 한다.
   - 두 모드 모두 "채널마다 최근 8번 평균"을 같은 메인 루프 주기로 쓴다. 다른 것은 "누가 ADC 값을 옮기나" 하나뿐이다.
3. **B1로 모드를 바꾼다.** `link_on_mode_change()`가 DMA 시작과 정지를 맡는다. 곡 도중(펄스가 2초 안에 온 때)에는 바뀌지 않는다.
4. **줄이 끊기는 순간 게임 줄을 보낸다.** `gate`가 0에서 1로 바뀔 때 `link_note_on("RE", 0)` 같은 줄이 나간다. 박 번호와 오차는 모듈이 Uno 펄스 기준으로 계산한다.
5. **CPU 사용률을 잰다.** 센서 읽기 부분만 `link_busy_begin/end`로 감싼다. 1초마다 `#S,H,DMA_ON,cpu=3.1,miss=0` 줄이 나온다.
6. **LD2 역할을 바꿨다.** 원래는 "하나라도 끊김" 표시였고, 지금은 DMA OFF 표시다.
7. **디버그 줄 `S2(A1) now=...` 앞에 `#`을 붙였다.** 게임은 이 줄을 무시한다.

## 확인 결과 (막을 것 / 사소한 것)

**막을 것**

- [x] 게임 줄이 없었다 → 패치 4번
- [x] 센서 DMA가 없어 ON/OFF 비교를 할 수 없었다 → 패치 1~3번
- [x] 박자 입력이 없었다 → 공통 모듈이 PB5(EXTI5)로 받는다. 하프의 `stm32f1xx_it.c`에 EXTI9_5 핸들러가 없어서 그대로 쓸 수 있다
- [ ] 프로젝트 전체(`Inc/`, `.ioc`, `Drivers/`)를 아직 올리지 않았다

**사소한 것 / 확인 필요**

- 메인 루프에 `HAL_Delay(5)`가 있어서 감지 시각의 해상도가 약 5ms다. 판정에는 충분하다(게임은 5ms 단위로 표시).
- CdS 반응 지연(수십 ms로 알려짐)을 아직 재지 않았다. 고정 지연이면 `ensemble_link_cfg.h`의 `LINK_LATENCY_MS`에 넣는다. 그러면 쏠림이 0 근처로 온다.
- 기준값 보정 1초 동안은 손을 대면 안 된다. 원본 그대로다.
- `.ioc`로 Generate Code를 하면 USER CODE 밖은 덮어쓴다. 패치는 USER CODE 안에만 넣었다.

## 시험한 것

- PC에서 ST 공식 CMSIS·HAL 헤더로 패치를 문법 검사했다. 경고와 오류 모두 0이다.
- 보드에서는 아직 안 돌려 봤다. 확인 순서:
  1. 굽고 시리얼에서 `MODE,DMA_ON`을 확인한다.
  2. 줄을 가리면 `H,ON,..`이 나오는지 본다. Uno가 없으면 박이 없어서 `#H,ON,RE,no beat`로 나온다.
  3. B1을 누르면 LD2가 켜지고 `MODE,DMA_OFF`가 나오는지 본다.
  4. `#S` 줄의 cpu 값을 두 모드에서 비교한다.
