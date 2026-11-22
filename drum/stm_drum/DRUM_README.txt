NUCLEO-F103RB 2패드 DMA 드럼 (proj_05)

피에조 타격 입력이 활성화된 연주 모드입니다.
파란 B1 USER 버튼을 한 번 누르면 녹색 LD2가 0.25초씩 세 번 켜지고,
킥 -> 스네어 -> 둘 동시 소리를 0.7초 간격으로 재생합니다.
킥 A0와 스네어 A1의 타격으로 소리를 냅니다.
main.c의 BUTTON_AUDIO_TEST_ONLY=0이며 B1 테스트음 기능도 유지합니다.
켜기/끄기 또는 음소거 기능은 추가하지 않았습니다.
UART 진단 PRESS는 B1 누름 횟수, PLAY는 실제 소리 재생 시작 횟수,
PWM은 출력 버퍼 범위, ADMA는 오디오 DMA 처리 블록 수입니다.

센서와 스피커 배선은 최종 통합 배선도와 동일합니다.
킥 A0/PA0, 스네어 A1/PA1, 소리 출력 D10/PB6.
ADC1 DMA1 CH1 / TIM4 PWM DMA1 CH7을 main.c의 레지스터 설정으로 구동합니다.
Debug와 Release의 컴파일 최적화는 -O2입니다.

Run으로 실행합니다. Debug 시작 시 main에서 자동 정지하지 않도록 설정했습니다.
직접 브레이크포인트를 걸어 멈춘 동안에는 TIM3/TIM4도 멈춰서 DMA 버퍼 지연을 방지합니다.
디버거가 일시정지 상태라면 Resume(F8)를 눌러야 소리가 납니다.
이전 버전에서 FAULT=12가 남아 있으면 RESET을 한 번 눌러 다시 시작하세요.
파란 B1 USER 버튼: 킥 -> 스네어 -> 둘 동시 테스트음.
UART2/ST-LINK 가상 COM: 115200, 8N1, flow control 없음.
K/S/B/T 문자로 테스트 가능. FAULT=0, DROP=0을 확인합니다.

main.c와 stm32f1xx_it.c를 함께 드럼 코드로 교체했고,
Core/Inc/drum_samples.h에 킥과 스네어 샘플이 들어 있습니다.
주변장치는 main.c가 설정합니다. 기존 proj_05.ioc로 Generate Code를 누르면
드럼 코드를 덮어쓸 수 있으므로 Generate Code를 사용하지 마세요.
실물 감도는 main.c 상단 KICK_THRESHOLD와 SNARE_THRESHOLD에서 조절합니다.

기존 피아노 코드 백업은 Codex 작업 폴더의
output/proj05_piano_backup_20261120 에 저장했습니다.
