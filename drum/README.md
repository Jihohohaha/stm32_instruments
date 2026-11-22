# 드럼 (STM32 #3)

> 비어 있음. 드럼 담당이 CubeIDE 프로젝트 폴더를 통째로 여기에 올린다.

- 구성: 피에조 2패드(킥·스네어), ADC1 + DMA로 타격·세기, 소리는 D10(PB6) PWM
- 보낼 줄: `D,ON,<KICK|SNARE>,<세기>,<박번호>,<오차ms>` (`docs/serial-protocol.md`)
- 확인 목록: `docs/review-checklist.md` 1·2번. 결과는 이 폴더의 `REVIEW.md`

올리는 법

```
drum/
└─ <프로젝트이름>/        ← .ioc, .project, .cproject, Core/, Drivers/ 그대로
```

`Debug/`, `Release/` 빌드 결과물은 `.gitignore`가 빼 준다. 시리얼 로그를 따로 남겼으면 `drum/logs/`에.
