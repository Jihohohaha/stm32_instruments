# 하프 (STM32 #2)

> 비어 있음. 하프 담당이 CubeIDE 프로젝트 폴더를 통째로 여기에 올린다.

- 구성: 레이저 모듈 4 + CdS GL5528 4 → 4줄(도·레·미·솔), 마디마다 화음(1·3박)
- 보낼 줄: `H,ON,<DO|RE|MI|SOL>,0,<박번호>,<오차ms>` (`docs/serial-protocol.md`)
- 확인 목록: `docs/review-checklist.md` 1·2번. 결과는 이 폴더의 `REVIEW.md`

올리는 법

```
harp/
└─ <프로젝트이름>/        ← .ioc, .project, .cproject, Core/, Drivers/ 그대로
```

`Debug/`, `Release/` 빌드 결과물은 `.gitignore`가 빼 준다. 시리얼 로그를 따로 남겼으면 `harp/logs/`에.

확인 필요: CdS 반응 지연(수십 ms로 알려짐)을 잰 값이 있으면 여기에 적기.
