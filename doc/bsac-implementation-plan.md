# ER BSAC 디코더 구현 계획 (T-DMB 재생 목표)

- 기준 문서: ISO/IEC 14496-3:2009 (PDF 1416쪽)
- 원칙: 표준 문서만을 근거로 한 clean-room 구현. 기존 libav BSAC 패치, ISO 레퍼런스 코드,
  논문에서 언급한 저장소는 참고하지 않음.
- 쪽 번호 표기: `PDF p.N (Subpart 4 p.M)` 형식. PDF 쪽 번호는 뷰어 기준.

---

## 1. 표준 문서 내 BSAC 관련 위치

| 내용 | 절 | PDF 쪽 |
|---|---|---|
| BSAC 확장(SBR/다채널) 시그널링, Table 1.28 | 1.6.8 | 72–73 |
| GASpecificConfig (`numOfSubFrame`, `layer_length`) | 4.4.1, Table 4.1 | 487–488 |
| AudioSpecificConfig의 `epConfig` (AOT 22 포함) | 1.6.2.1 | (subpart 1) |
| 구문: `bsac_payload` ~ `extended_bsac_sac_data` | 4.4.2.6, Table 4.31–4.49 | 503–509 |
| 페이로드 재배열(sub-frame / large-step layer) | 4.5.2.6.1 | 562–565 |
| `bsac_raw_data_block` 의미·복호 절차 | 4.5.2.6.2.1–2 | 565–574 |
| 창·그룹·scalefactor band·coding band | 4.5.2.6.2.3–4 | 574–575 |
| **fine grain layer 구성, `available_len` 계산** | 4.5.2.6.2.5 | 575–579 |
| short window 계수 순서 (4개 단위 interleave) | 4.5.2.6.2.6 | 579 |
| **산술 복호기 (`decode_symbol`, `decode_symbol2`)** | 4.5.2.6.2.7 | 579–581 |
| BSAC 확장 페이로드(SBR, MC, SAC), HF overlap | 4.5.2.11 | 596–598 |
| **Noiseless coding for FGS (bit-sliced 복호, 문맥 모델)** | 4.6.4.2 | 625–628 |
| stereo_info / ms_used / noise_flag | 4.6.4.3 | 628–630 |
| scalefactor / noise energy / IS position | 4.6.4.4 | 630–632 |
| cband_si | 4.6.4.5 | 632–633 |
| Segmented Binary Arithmetic coding (SBA) | 4.6.4.6 | 633–634 |
| 표: Table 4.A.31–4.A.77 | 4.A.5 | 815–824 |
| (참고) 인코더 설명 | 4.B.17 | 부록 |

PDF에서 텍스트 추출은 PyMuPDF(`fitz`)로 잘 되는 편. 다만 Table 4.A.34처럼 2차원 표는
열 순서가 뒤섞여 나오므로 사람이 PDF 원본과 대조해 옮겨야 함.

---

## 2. AAC와 BSAC의 차이

핵심: **BSAC는 AAC의 "noiseless coding" 부분(section data + Huffman scalefactor + Huffman
spectral data)만 bit-sliced arithmetic coding으로 바꾼 것.** 역양자화 이후
(M/S, IS, PNS, TNS, LTP, IMDCT, 창 전환, SBR)는 AAC와 동일.

### 2.1 그대로 같은 부분 (FFmpeg `aacdec` 재사용 가능)

- 창 종류와 전환: `window_sequence`, `window_shape`, `scale_factor_grouping`, KBD/sine 창, IMDCT 1024/128 (960/120)
- scalefactor band 표(`swb_offset_*`), 그룹 계산 (4.5.2.6.2.4의 의사코드는 AAC와 동일)
- 역양자화 `sign(x)·|x|^(4/3)·2^(0.25·(sf−100))`
- M/S 스테레오, intensity stereo, PNS 처리 자체
- `tns_data()`, `ltp_data()` 구문 (general_header 안에서 AAC와 같은 구문으로 전송)
- SBR (`sbr_single_channel_element`, `sbr_channel_pair_element`, `sbr_header` 구문 동일)

### 2.2 다른 부분

| 항목 | AAC (LC) | ER BSAC |
|---|---|---|
| 프레임 구조 | `raw_data_block` 안에 SCE/CPE/FIL… 원소들, 원소마다 ICS | 단일 `bsac_raw_data_block`, 채널 원소 없이 `nch`(1 또는 2)로 모든 채널을 함께 부호화 |
| 프레임 길이 | 외부(ADTS/LATM/컨테이너) | `frame_length` 11비트(바이트 단위)가 프레임 맨 앞에 있음 |
| 헤더 | `ics_info` 채널별(또는 common_window) | `bsac_header` + `general_header` 한 번. 창 정보는 항상 공통 |
| section data | codebook 별 구간 | **없음**. 대신 32계수 단위 **coding band**(cband)마다 `cband_si`(MSB plane + 확률표 번호) |
| scalefactor | global_gain + Huffman 차분 | `max_scalefactor[ch]`(8비트) − 산술부호 차분값. 모델은 `base_scf_model`/`enh_scf_model` |
| 스펙트럼 | Huffman codebook 1–11, 2/4-튜플, escape | 계수 절댓값을 **비트평면(bit-slice)으로 쪼개 MSB→LSB 순서로** 이진 산술부호화, 부호는 처음 1이 나온 직후 |
| 엔트로피 부호 | Huffman | 30비트 레지스터 기반 산술부호 (다중심볼 14비트 누적빈도 모델 + 이진 6비트 확률) |
| 스케일러빌리티 | 없음 | 1 kbps/ch 단위 **fine grain layer**. base layer(여러 sub-layer) + enhancement layer. 레이어가 올라갈수록 대역폭이 넓어지고 하위 비트평면이 채워짐 |
| 비트스트림 절단 | 불가 | 뒤를 잘라내도 복호 가능. `available_len[layer]`로 레이어별 비트 예산 추적 |
| M/S 마스크 | `ms_mask_present` 0/1/2 | 0/1/2/**3**. 3이면 sfb별 2비트 `stereo_info`(독립/MS/IS in-phase/IS out-of-phase 또는 noise) |
| PNS 신호 | band_type == NOISE_HCB | `pns_data_present` + `pns_start_sfb`, sfb별 `noise_flag`, 스테레오일 때 `noise_mode`(독립/상관/역상관) |
| IS 신호 | band_type INTENSITY_HCB/HCB2 | `stereo_info` 2/3 + `is_position_index` |
| pulse data, gain control | 있음 | 없음 |
| 오류 내성 | ER AAC는 VCB11/RVLC/HCR | `sba_mode`=1이면 segment마다 산술복호기 재초기화 (SBA) |
| 확장 | FIL 원소의 extension_payload | 프레임 끝 `zero_code`(32비트 0) + `sync_word`(1111) + 4비트 `extension_type` 반복 |
| SBR 신호 | FIL/EXT_SBR_DATA | `EXT_BSAC_SBR_DATA(_CRC)`. `cnt` 4비트(+esc 8비트)이며 길이 계산 방식이 AAC FIL과 다름 |
| 다채널 | 여러 SCE/CPE | 기본 1–2채널 + `extended_bsac_raw_data_block`(channel_configuration_index로 추가 채널) |
| 전송 | 1 AU = 1 프레임 | `numOfSubFrame`개 서브프레임 × `numOfLayer`개 ES로 interleave 가능 (4.5.2.6.1) |

### 2.3 복호 흐름 요약

```
bsac_raw_data_block
 ├ bsac_base_element
 │   ├ frame_length(11)
 │   ├ bsac_header : header_length, sba_mode, top_layer, base_snf_thr,
 │   │               max_scalefactor[ch], base_band, cband_si_type[ch],
 │   │               base_scf_model[ch], enh_scf_model[ch], max_sfb_si_len[ch]
 │   ├ general_header : window/grouping/max_sfb, pns, ms_mask_present, tns, ltp
 │   ├ byte_alignment
 │   └ bsac_layer_element(0 .. slayer_size-1)          ← base layer
 ├ bsac_layer_element(slayer_size .. top_layer+slayer_size-1)  ← data가 남아 있는 동안
 │     ├ layer_cband_si   (새 cband의 cband_si)
 │     ├ layer_sfb_si     (새 sfb의 stereo/pns/scf/IS/noise energy)
 │     ├ bsac_layer_spectra   (새로 추가된 스펙트럼의 비트평면)
 │     └ bsac_lower_spectra / bsac_higher_spectra (남은 예산으로 미처 못 보낸 비트평면)
 ├ byte_alignment
 └ [zero_code, sync_word, {extension_type, payload, byte_alignment}*]
```

레이어별 비트 예산(`available_len`)은 비트스트림에 직접 실려 있지 않고
`frame_length`, `header_length`, `top_layer`, `nch`, 샘플레이트, `max_sfb_si_len`,
`cband_si_type` 등으로 **디코더가 계산**함 (4.5.2.6.2.5). 이 계산이 인코더와 한 비트라도
어긋나면 그 뒤가 모두 깨지므로, 구현의 가장 큰 위험 지점.

---

## 3. 표준 문서에서 발견한 오류·모호점 (샘플로 검증 필요)

| # | 위치 | 내용 | 잠정 해석 |
|---|---|---|---|
| E1 | Table 4.43 `bsac_spectral_data` | 부호 복호 직전 `if (layer_data_available()) return;` | `!layer_data_available()` 의 오타로 보임 |
| E2 | 4.5.2.6.2.5 | `for (layer = slayer_size – 1; layer >= 0; slayer--)` | `layer--` |
| E3 | 4.5.2.6.2.5 underflow 분기 | `if (layer <= (underflow_size%slayer_size))` | `m`의 오타일 가능성. 또는 `<` 여부도 불명확 |
| E4 | 4.5.2.6.2.5 `layer_end_sfb` | 긴 창에서도 `swb_offset_short_window[..]*window_group_length` 사용 | 긴 창에서는 `swb_offset_long_window`가 맞을 것 |
| E5 | 4.6.4.4.3 IS position | `is_position_sign[g][sfb]%2` | `is_position_index`의 오타 |
| E6 | 4.6.4.4 | `max_noise_energy`(구문상 9비트), `dpcm_noise_energy_index`, `is_position_index`에 쓰는 **산술 모델이 명시되지 않음** ("the same arithmetic model is used") | scalefactor 모델(enh/base_scf_model)일 가능성. 9비트는 균등 모델 또는 raw 비트 가능성. **샘플로 결정** |
| E7 | Table 4.39 | `noise_pcm_flag[ch]`, `stereo_side_info_coded[][]` 초기화 시점 미기재 | 프레임마다 각각 1, 0으로 초기화 |
| E8 | 4.6.4.6.3 `terminal_layer` | 루프가 top layer 직전까지만 설정 | 최상위 레이어는 항상 terminal |
| E9 | 4.5.2.6.2.2.4 | `base_band`를 "minimum spectral line"이라 기술 (의미 절은 maximum) | maximum이 맞음 |
| E10 | Table 4.A.34 | PDF 추출 시 열이 섞임 | PDF 원본 렌더링 이미지로 직접 확인해 옮길 것 |
| E11 | 4.5.2.6.2.7 | `decode_symbol2(buf_idx, freq0, …)` 선언과 본문 `p0` 불일치, `p1 = 16384 – p0`인데 p0 표기는 "6-bit, 상위 6비트만 유효" | 14비트 스케일(16384=1.0)로 통일해서 사용 |
| E12 | 4.6.4.3 | `ms_mask_present==3`, `stereo_info==3`, 두 noise flag가 0 → out-of-phase IS. `sfb < pns_start_sfb`일 때 noise flag 자체를 읽지 않음 | 구문표(Table 4.39)를 기준으로 구현 |
| E13 | 4.5.2.6.2.5 `available_len` | "산술부호가 레이어 시작에서 초기화되었으면 1을 뺀다" — SBA에서 segment 시작 레이어에만 해당 | SBA가 아니면 layer 0에만 적용 |
| E14 | 4.5.2.6.2.5 `layer_end_sfb` | `if (layer_end_index <= swb_offset[sfb]) layer_end_sfb = sfb + 1` | **샘플로 확인: `+ 1`이 오류.** 레이어 끝 이후에서 시작하는 band는 포함하지 않음 (`layer_end_sfb = sfb`). `+ 1`이면 레이어 0의 scf 심볼 수가 어긋나 c0부터 깨짐 |
| E15 | Table 4.39 vs 4.B.17.5 | 규범 구문은 모든 sfb에 `acode_scf_index`를 두지만, 정보성 부록은 "값이 모두 0인 sfb의 scalefactor는 전송하지 않는다", "첫 scf만 max 기준, 나머지는 직전 scf 기준 차분"이라고 기술 | **샘플로 확인: 부록 설명이 틀림.** 기본 레이어에서 0 대역 scf를 건너뛰면 c9·c10부터 깨지고, 확장 레이어에서만 건너뛰어도 개선 없음. 규범 구문(모든 sfb 전송, 모두 `max_scalefactor` 기준 차분)이 맞음 |
| E16 | 4.5.2.6.2.5 / E13 | 산술부호 초기화 레이어에서 `available_len`에서 1을 뺀다 | **샘플로 확인: 빼지 않아야 함.** 빼면 레이어 0 끝이 1비트 어긋나 c1부터 깨짐 |
| E17 | 4.6.4.2.3 `min_p0`/`max_p0` | `available_len < 14`이면 p0를 Table 4.A.35/36 범위로 제한 | **샘플로 확인: 제한하지 않아야 함.** 인덱스를 ±1 옮기거나 부호 비트만 빼는 변형도 모두 악화. 이 인코더는 제한을 쓰지 않는 것으로 보임 |
| E18 | 4.5.2.6.2.2.12 | "남은 비트가 있으면(redundant bits) 다음 레이어 `available_len`에 더한다" — 음수(초과 사용)는 언급 없음 | **샘플로 확인: 음수도 그대로 넘겨야 함.** 전체 비트 수가 보존됨 |
| E19 | 4.5.2.6.2.5 `layer_bit_offset` | `(int)(nch·bitrate·BLOCK/fs/8)·8` — 채널 합계를 바이트로 내림 | **샘플로 확인: 채널별로 내린 뒤 곱해야 함** `nch·((bitrate·BLOCK/fs)/8·8)`. 44.1 kHz에서는 ss+4까지 두 식이 같고 ss+5부터 8비트 차이(KBS 레이어 16의 "-8" 현상이 이것). 48 kHz(YTN)는 기본 레이어 끝(680 대 672)부터 달라서, underflow 프레임의 30%가 기본 레이어에서 깨졌음. 채널별 내림으로 바꾸자 YTN 조기 실패 23%→7.5%, 프레임별 underflow 흔들기 검사에서 d=0이 뚜렷한 최고점 |
| E20 | 4.5.2.6.2.5 `layer_end_index` (44.1/48 kHz) | `end_index%32 == 0`이면 +8, 아니면 +12 → cband당 8/12/12줄 | **샘플로 확인: 12/12/8이 맞음.** band 경계를 넘지 않는 선에서 +12 (`min(12, 32 - end%32)`). 8/12/12로 두면 값이 있는 첫 확장 cband 다음부터 모두 깨짐(값이 0인 cband는 줄 배치가 보이지 않아 통과). 12/12/8로 바꾸자 KBS는 c0–c13 전부, YTN은 c0–c14 전부 MSB 검사 불량 0에 가까움 |
| E21 | 산술 복호기 | p0가 0이거나 16384이면 range가 0이 되어 이후 나눗셈이 0으로 나눔 | 손상 스트림 대비로 p0를 [1, 16383]로 제한하고 range 0을 검사 |

### 3.0 샘플 목록과 인코더 특성

| 샘플 | fs | `base_band` | `top_layer` | `ms_mask_present` | scf 모델(base/enh) | `cband_si` 특성 | 상태 |
|---|---|---|---|---|---|---|---|
| KBS (녹화) | 44.1k | 10 | 10 | 0,1,2 | 0–4 / 0–7 | c≥1 홀수만 | E20 반영 후 전 대역 정상 |
| MBC (시험방송) | 44.1k | 7 | 12 | 1 | 0 / 0 | c≥1 짝수만 | c0–c7 정상, c8부터 깨짐 |
| DMB (시험방송) | 44.1k | 18 | 12 | 2 | 4 / 0 | 혼합 | 거의 전부 기본 레이어라 끝까지 정상 |
| YTN (녹화) | 48k | 11 | 12 | 2 | 1–6 / 1–4 | 혼합 | E19·E20 반영 후 전 대역 정상 |

- MBC의 `header_length`는 항상 0(실제 헤더 12바이트) → 헤더 길이는 비트 수로 직접 계산해야 함
- 네 샘플 모두 TS에 TEI·연속성 오류 없음
- 모든 인코더에서 **첫 확장 cband의 `cband_si`까지는 맞고, 부분 cband 레이어(8/12/12줄)를 지난 다음 cband부터 깨짐.**
  KBS에서 c10이 전부 0인 프레임은 c11이 13/13 정상(scf 모델 무관) → 확장 레이어의 예산·lower spectra·scf는 맞고,
  부분 cband 줄의 스펙트럼 복호가 원인. 레이어 예산을 ±40비트 흔들어도 맞는 오프셋이 없음(산술 상태 자체가 어긋남)
- 시간 상관 지표: 연속 프레임 사이 `cband_si` 차이 / 임의 프레임 쌍 차이. 1에 가까우면 무작위

### 3.1 T-DMB 샘플(KBS)로 확인한 사실

- AOT 22, 44.1 kHz, 스테레오, `numOfSubFrame`=1, `layer_length`=1792, `epConfig`=0
- 모든 프레임: `frame_length`=151 바이트 = 패킷 크기, `top_layer`=10, `sba_mode`=0, `base_snf_thr`=0,
  `base_band`=10, ONLY_LONG, `max_sfb`=33, PNS 없음, LTP 없음, `ms_mask_present` ∈ {0,1,2}, 일부 프레임 TNS
- `header_length + 7` = 실제 헤더 바이트 수 (TNS 파싱 포함 헤더 해석 검증됨)
- 검증 지표
  1. 각 coding band의 MSB plane에는 1이 적어도 하나 있어야 함 (`cband_si`가 가리키는 MSB와 실제 최댓값 비교)
  2. 각 `cband_si` 심볼 비용 ≤ `max_cband_si_len`(0번 band는 11), 각 sfb side info 비용 ≤ `max_sfb_si_len + 5`
  3. **(가장 강력)** 이 인코더는 홀수 `cband_si`(확률표 1,3,5,7,9 계열)만 사용함. 0 또는 홀수가 아니면
     그 cband는 동기를 잃은 것. 동기를 잃은 복호는 모든 cband에서 똑같은 분포(짝수 60% 이상)를 보임
- TS에는 TEI·연속성 카운터 오류가 없고, 오류율은 파일 전 구간에 고르게 분포 (초반 집중 아님)
- `base_scf_model`=0(scf 전송 없음)인 프레임과 `max_sfb_si_len`=0이 흔함. scf는 대부분 `max_scalefactor`와 같음
- 레이어 0 예산은 약 126비트로 c0의 상위 2~3개 평면만 담김. `bsac_lower_spectra`는 기본 레이어 1~4에서는
  거의 실행되지 않고(예산이 새 cband에서 소진), 레이어 10 이후에는 거의 매번 실행됨
- 현재 상태(E14·E16·E17·E18 반영): c0~c9의 MSB 검사 불량 0%, `cband_si` 분포 정상.
  c10은 `cband_si`까지 맞음. c10에 값이 있으면 레이어 10~12에서, c10이 비어 있어도 레이어 13~15에서 동기를 잃음

---

## 4. FFmpeg 현황

- `libavcodec/mpeg4audio.c`: `AOT_ER_BSAC` + SBR 명시적 시그널링 시 `ext_chan_config`(4비트) 읽기는 이미 있음.
- `libavcodec/aac/aacdec.c` `decode_ga_specific_config()`: `numOfSubFrame`(5), `layer_length`(11)를
  **읽고 버림**. 또한 ER AOT의 `epConfig` 읽기 목록에 **AOT 22가 빠져 있음** (표준은 17, 19–27, 39 모두 포함) → 수정 필요.
- `aacdec_ac.c`는 USAC용 산술부호기라 BSAC와 무관 (재사용 불가).
- `aac_decode_er_frame()`은 ER AAC(LC/LTP/LD/ELD)의 채널 원소 루프 → BSAC는 별도 경로 필요.
- SBR 진입점 `ff_aac_sbr_decode_extension()`은 AAC FIL 원소 기준으로
  "이미 4비트 extension_type을 읽었다"는 가정(`cnt*8 - 4`)을 함. BSAC의
  `extended_bsac_sbr_data()`는 `cnt`(4비트, esc 8비트)와 정렬 계산 방식이 다르므로 얇은 어댑터 또는 분리가 필요.

---

## 5. 설계

### 5.1 파일 구성

```
libavcodec/aac/aacdec_bsac.h      공개 진입점, BSACContext 정의
libavcodec/aac/aacdec_bsac.c      프레임 파싱, 레이어 구성, 산술복호, bit-slice 복원
libavcodec/aac/aacdec_bsac_tab.c  Table 4.A.31–4.A.77
libavcodec/aac/aacdec_bsac_tab.h
tests/checkasm 불필요, tests/fate/aac.mak 에 BSAC 테스트 추가 (샘플 확보 후)
```

### 5.2 AAC 디코더와의 접점

1. `decode_audio_specific_config_gb()` / `decode_ga_specific_config()`
   - `numOfSubFrame`, `layer_length` 저장, `epConfig` 읽기에 AOT 22 추가
   - `AOT_ER_BSAC`에서 `nch = channelConfiguration`(1 또는 2)만 우선 허용
2. `aac_decode_frame_int()`의 AOT 분기에 `AOT_ER_BSAC` → `ff_aac_bsac_decode_frame()`
3. `ff_aac_bsac_decode_frame()`의 출력은 **기존 `ChannelElement`/`SingleChannelElement`를 채우는 것**:
   - `IndividualChannelStream` (window_sequence, use_kb_window, max_sfb, num_window_groups, group_len, swb_offset, num_swb…)
   - `band_type[]` : 일반 밴드는 임의 비영(非零) 코드북, PNS → `NOISE_BT`, IS → `INTENSITY_BT`/`INTENSITY_BT2`
   - `sfo[]`(scalefactor 인덱스), `ms_mask[]`, `tns`, `ltp`
   - `coeffs[]` : 정수 양자화값을 `|x|^(4/3)`(기존 `ff_cbrt_tab`) × scalefactor로 역양자화, short window는 de-interleave 후 창별 배치
   - 그 뒤는 기존 `apply_channel_coupling` 없이 M/S → IS → PNS → TNS → `spectral_to_sample()` 경로 재사용
4. 스테레오 처리에 쓰이는 `static` 함수(`decode_tns`, `decode_ltp`, `apply_prediction` 등)는
   필요한 만큼만 `ff_` 접두어로 노출

1차 목표는 **float 디코더만** (`aac` 디코더). `aac_fixed`는 `AVERROR_PATCHWELCOME` 반환 후 나중에 역양자화 부분만 fixed 템플릿으로 추가.

### 5.3 산술 복호기

- 표준의 레지스터 동작(`value`, `range`, `est_cw_len`, 초기 30비트, 14비트 정규화, `half[]` 표)을 그대로 따르되 코드는 새로 작성.
- 비트 읽기는 **segment 전용 비트리더**: segment 끝을 넘으면 0을 돌려주는 방식으로 "32비트 zero stuffing"을 구현 (버퍼 복사 불필요).
- 심볼 하나를 읽을 때마다 `available_len[layer] -= est_cw_len`, 0 이하가 되면 `layer_data_available()`=0.
- `available_len < 14`일 때 p0를 `min_p0`/`max_p0`(Table 4.A.35/36)로 클램프.
- SBA(`sba_mode`=1)면 terminal layer마다 새 segment로 재초기화, 남는 예산은 다음 레이어로 이월.
- `range`/`value` 곱셈은 32비트를 넘을 수 있으므로 `uint64_t`로 계산.

### 5.4 레이어 구성 (프레임마다)

4.5.2.6.2.5의 의사코드를 순서대로:

1. `slayer_size`, `end_cband[g]` (short window에서 샘플레이트별 반올림 규칙 포함)
2. `layer_group[]` (base sub-layer → 그룹 순서, 이후 8주기 반복)
3. `layer_start/end_index[]`, `layer_start/end_cband[]` (샘플레이트별 증분 8/12, 16, 32, 64)
4. `layer_start/end_sfb[]` (E4 주의)
5. `layer_si_maxlen[]`, `layer_bit_offset[]`(비트레이트 → 비트 위치), overflow/underflow 보정 (E2, E3 주의), `available_len[]`
6. `terminal_layer[]` (SBA)

이 단계는 순수 함수로 분리하여 단위 테스트 가능하게 작성: 입력(헤더 값) → 출력(배열).
샘플이 들어오면 **가장 먼저 이 결과가 실제 비트스트림 경계와 맞는지** 확인.

### 5.5 스펙트럼 복원

- 상태: `sample[ch][g][i]`(정수), `cur_snf`, `unc_snf`, `sign_is_coded`, `higher_bit_vector`
  (short window에서 그룹별 최대 1024계수, 채널당 1024 정수면 충분)
- 확률 선택
  1. `cband_si` → 확률표 번호·MSB plane (Table 4.A.33)
  2. significance(MSB, MSB-1, MSB-2, others) × 상위 비트 zero/non-zero로 sub-table 선택 (Table 4.A.56–77)
  3. 상위 비트가 0이 아니면 `p0_index = min(higher_bit_vector, 16) - 1`, 0이면 이웃 4계수 문맥(Table 4.A.34)
- Table 4.A.67–77은 "Table 9/10과 같고 MSB plane만 다름" → 표 공유
- 복원: `sample += bit << (snf-1)`, 첫 1 이후 부호(p0 = 8192 고정)
- 복호 순서: `bsac_layer_spectra` → (non-SBA) `bsac_lower_spectra` / (SBA, terminal) lower + higher

### 5.6 side information

- `cband_si`: 0번 cband는 Table 4.A.51, 나머지는 `cband_si_type`에 따른 4.A.44–50. 최대값 초과 시 오류.
- scalefactor: `scf = max_scalefactor[ch] - diff`, base layer는 `base_scf_model`, 이후는 `enh_scf_model` (Table 4.A.32, 4.A.37–43). `scf_model == 0`이면 차분 0.
- ms_used(4.A.52), stereo_info(4.A.53), noise_flag(4.A.54), noise_mode(4.A.55)
- noise energy / IS position: E6 참고. 여러 가설을 옵션으로 두고 샘플로 확정.

### 5.7 확장 영역

1. 우선: `zero_code`/`sync_word` 확인 후 `extension_type`별 길이만큼 **안전하게 건너뛰기**
   (`EXT_BSAC_CHANNEL`은 `element_length`, SBR은 `cnt`, 기타는 `extended_bsac_data`의 `cnt`)
2. SBR (`EXT_BSAC_SBR_DATA`, `_CRC`): `ff_aac_sbr_decode_extension()`을 BSAC 길이 규칙에 맞게 부르는 어댑터.
   암시적 시그널링(Table 1.28)에서 출력 샘플레이트 2배 처리. HF overlap(`core_max_band = layer_max_freq·64/1024`)은
   코어가 SBR 시작 대역보다 높게 복호된 경우 그 구간을 코어로 대체.
3. 다채널(`EXT_BSAC_CHANNEL*`), MPEG Surround(`EXT_BSAC_SAC_DATA`): `avpriv_report_missing_feature` 후 무시.
   T-DMB는 스테레오까지이므로 우선순위 낮음.

### 5.8 페이로드 재배열

- 단일 ES(`numOfLayer`=1)이면 AU = `numOfSubFrame`개 프레임의 단순 연결이며 각 프레임 앞의 `frame_length`로 분리 가능.
- `numOfSubFrame > 1`이면 패킷 하나에서 프레임 여러 개가 나오므로 디코더를 `receive_frame` 방식으로 바꾸기보다
  **BSF 또는 파서에서 서브프레임 단위로 쪼개는 쪽**을 우선 검토.
- 다중 ES(layer별 ES) 결합은 FFmpeg 스트림 모델과 맞지 않으므로 범위 밖.
- T-DMB 샘플에서 실제 `numOfSubFrame`, `layer_length` 값을 확인한 뒤 결정.

---

## 6. 단계별 작업 순서

| 단계 | 작업 | 완료 기준 |
|---|---|---|
| 0 | ASC 파싱 보완(`epConfig` AOT 22, `numOfSubFrame`/`layer_length` 저장), BSAC 분기 뼈대, `PATCHWELCOME` 경로 | 기존 FATE 무변화 |
| 1 | 표 옮기기(4.A.31–77) + 표 무결성 검사(누적빈도 단조감소, 마지막 0 등) | 표 개수·길이 검증 스크립트 통과 |
| 2 | 산술 복호기 + segment 비트리더 | 자체 소형 인코더(테스트 전용)로 왕복 검증 |
| 3 | 헤더 파싱 + 레이어 구성(5.4) | 헤더 덤프 로그, 샘플의 `header_length`와 일치 |
| 4 | side info(cband_si, stereo/pns, scf) | 샘플에서 scf가 AAC 통상 범위, cband_si가 최대값 이내 |
| 5 | bit-sliced 스펙트럼 + 부호 + short de-interleave | 모노 긴 창 샘플에서 들리는 소리 |
| 6 | AAC 파이프라인 연결(역양자화, M/S, IS, PNS, TNS, LTP, IMDCT) | 스테레오·short window 샘플 정상 재생 |
| 7 | 확장 건너뛰기 → BSAC+SBR | HE-BSAC 샘플 재생, 출력 샘플레이트 정상 |
| 8 | SBA, 절단 스트림(상위 레이어 누락) 처리, 오류 내성 | 레이어를 잘라낸 입력에서도 크래시 없이 저음질 재생 |
| 9 | FATE 테스트, fuzz(`tools/target_dec_fuzzer`) | FATE 추가, fuzzer 장시간 무크래시 |
| 10 | (별도) MPEG-2 TS 내 MPEG-4 SL/IOD 경로(T-DMB) | T-DMB TS 직접 재생 |

단계 2의 "테스트 전용 소형 인코더"는 표준 4.5.2.6.2.7 복호기의 역연산만 구현한
작은 C/Python 스크립트이며 저장소에 넣지 않음. 표준 적합성이 아니라 복호기 자체의
경계 조건(`est_cw_len`, zero stuffing, 레이어 경계) 확인 용도.

---

## 7. 검증 방법

- 실제 샘플(T-DMB TS 또는 BSAC 원시 프레임 + ASC)을 받으면
  1. 프레임 헤더 덤프로 `frame_length`, `header_length`, `top_layer`, `max_sfb`가 일관적인지 확인
  2. 레이어 경계에서 산술복호기가 소비한 비트 수와 `available_len` 비교 → E2–E4, E13 확정
  3. PNS/IS 사용 프레임에서 E6 확정
- 가능하면 ISO/IEC 14496-26(오디오 적합성) BSAC 적합성 비트스트림과 기준 PCM으로 비교.
  (레퍼런스 디코더를 **실행**해서 기준 출력을 만드는 것은 코드 복사가 아니므로 문제없음. 코드는 열람하지 않음.)
- `-debug`용 `av_log(AV_LOG_TRACE)`로 레이어별 비트 소비 로그를 남겨 디버깅.
