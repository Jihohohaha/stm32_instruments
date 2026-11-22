# 관악기 (STM32 #1, NUCLEO-F103RB)

마이크 입김 세기와 조이스틱 5방향으로 사인파 PWM 소리를 낸다. 조이스틱은 가운데 도 / 위 레 / 오른쪽 미 / 아래 파 / 왼쪽 솔이다.
주변장치는 레지스터로 직접 설정한다. HAL은 클럭과 `HAL_GetTick`에만 쓴다.

| 기능 | 자원 |
| --- | --- |
| 마이크 / 조이 X / 조이 Y | PA0 / PA1 / PA4 → ADC1 scan. TIM3 TRGO(8kHz)가 변환을 시작하고, DMA1 채널1이 circular로 옮긴다 |
| 조이 SW | PB0 풀업 입력 (누르면 한 옥타브 위) |
| 소리 | PB6 TIM4_CH1 PWM. DMA1 채널7이 사인파 표를 CCR1에 넣는다 → RC 1kΩ+100nF → PAM8403 |
| 박자 입력 | PB5(D4), EXTI5 올라가는 순간. Uno D8 펄스를 1kΩ+2kΩ 분압으로 받는다 |
| DMA ON/OFF | B1(PC13). LD2 켜짐 = DMA OFF |
| 게임 줄 | **I²C로 Uno에**: PB8(D15)=SCL, PB9(D14)=SDA → Uno A5/A4, 4.7kΩ 풀업(5V). USART2(PA2/PA3, 115200)에도 같은 줄을 디버그용으로 찍는다 |
| 클럭 | HSI → PLL **64MHz** (기획안의 72MHz 아님) |

## 버전

| 버전 | 내용 | 기여 |
| --- | --- | --- |
| v1 (커밋 `0528111`) | ADC 3채널 + DMA, 입김으로 소리 켜고 끄기, 조이스틱 5음, SW 옥타브, 0.1초 상태 로그 | 팀원(공동 담당) 구현. 실제 연결해서 소리와 음 변경 확인 |
| **합주 패치 (11/22, 지금)** | v1 + DMA OFF 폴링 경로 + B1 전환 + 박자 기준 게임 줄 + I²C (`common/ensemble_link`) | AI 제안, 박지호 검증 예정. **보드 미검증** |

## 합주 패치에서 바뀐 것

소리 코드는 그대로다. `sine_q15`, `sound_update`, `sound_init`가 v1과 같다.

- **`wind.c`**
  - **게임 줄**
    - `process_half()` 끝에서, 불기 시작하면 `W,ON,<음>,<세기>,..`, 멈추면 `W,OFF,..`를 보낸다.
    - 부는 중에 조이스틱 음이 바뀌어 24ms(반 버퍼 3번) 유지되면 OFF(이전 음)와 ON(새 음)을 보낸다.
    - 박 번호와 오차는 모듈이 Uno 펄스 기준으로 계산한다.
  - **DMA OFF 폴링 경로**
    - 메인 루프가 TIM3 넘침 플래그(8kHz)를 보고 채널 0·1·4를 하나씩 SWSTART → EOC 기다림으로 읽는다.
    - 같은 `adc_buf` 자리에 쓰고, 반 버퍼가 차면 같은 `process_half()`를 부른다. 타이머 주기와 처리 함수는 두 모드가 같다.
  - **`link_on_mode_change()`**: B1을 누르면 모듈이 부르는 함수다. DMA1 채널1과 ADC 설정(SCAN·EXTSEL·DMA 비트)을 바꾼다. 곡 도중에는 바뀌지 않는다.
  - **CPU 사용률**
    - DMA ON: DMA 인터럽트를 `link_busy_begin/end`로 감싼다.
    - DMA OFF: 변환 대기와 처리를 감싼다.
    - 폴링이 늦어 놓친 프레임은 miss로 센다.
    - 1초마다 `#S,W,DMA_ON,cpu=..,miss=..` 줄이 나온다.
  - **디버그 줄**: 시작 안내와 보정 결과 앞에 `#`을 붙였다. 0.1초 상태 줄은 `WIND_LOG_STATUS`(기본 0)로 끄고 켠다.
- **`wind.h`**: `WIND_LOG_STATUS` 옵션을 추가했다.
- **추가한 파일**
  - `Core/Src/ensemble_link.c`, `Core/Inc/ensemble_link.h`: `common/`과 같은 파일이다.
  - `Core/Inc/ensemble_link_cfg.h`: `LINK_INST 'W'`, `LINK_LATENCY_MS 4`(추정), I²C 켬.
- `main.c`는 그대로다. `wind_init`과 `wind_loop` 이름도 그대로다.

## 빌드

1. CubeIDE에서 File → Import → Existing Projects into Workspace를 연다.
2. `wind/wind_instrument`를 고른다. "Copy projects into workspace"는 체크를 뺀다.
3. Build한다.

## 확인 순서 (보드 하나, 노트북 USB 터미널 115200)

1. 켜고 2초 동안은 불지 말고 조이스틱도 건드리지 않는다. 그러면 `#cal done`과 `MODE,DMA_ON`이 나와야 한다.
2. 불기 시작하면 소리가 나야 한다.
   - Uno가 없으면 박을 못 받아서 `#W,ON,DO,no beat`가 나온다.
   - Uno D8 펄스가 연결돼 있으면 `W,ON,DO,..,<박>,±ms`가 나온다.
3. 불면서 조이스틱을 위로 밀면 `W,OFF,DO,..`와 `W,ON,RE,..`가 나와야 한다.
4. B1을 누르면 LD2가 켜지고 `MODE,DMA_OFF`가 나와야 한다. 소리와 음 바꾸기가 그대로 되는지 본다.
5. `#S` 줄의 cpu·miss 값을 두 모드에서 비교한다.

## 열린 것

- `.ioc`에는 FreeRTOS가 켜져 있다. **Generate Code를 하면 안 된다.** 다시 생성할 일이 생기면 CubeMX에서 FreeRTOS부터 끈다.
- `LINK_LATENCY_MS 4`는 추정값이다. 실물로 재서 쏠림이 0 근처가 되게 맞춘다.
- 자기 스피커와 옆 악기(특히 드럼) 소리를 입김으로 착각하는지 확인해야 한다. 소리를 켠 채 안 불고 `W,ON`이 나오는지 본다.
