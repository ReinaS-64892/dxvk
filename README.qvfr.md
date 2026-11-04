# VRChat 向け quad view rendering の実装

このブランチには、DXVK 内で両眼の周辺／注視領域を別々に描画し、元のカラー／深度画像へ合成する実験用 QVFR がある。対象は明示したサイズ・format・MSAA 数に一致する、左右の半分ずつの Viewport を使う描画。既定では無効であり、画質と GPU 動作の実機検証が必要。

VRChat のファイル変更、プロセスへのコード注入、デコンパイルは行わない。D3D11 テクスチャを論理上維持し、対応する中間操作では分割表現を引き継ぐ。xrizer / OpenVR へ渡す実際の画像は提出前に DXVK 内で合成する。

## 起動オプションを減らした実行時設定（2026-11-05）

現在の QVFR 用 Steam 起動オプションは次だけでよい。以前の `DXVK_QVFR_SIZE`／`PROFILE`／`GAZE`／`CONTROL_FILE` 等は削除できる。`DXVK_QVFR=0` または未指定では QVFR を初期化せず、ファイルが on でも有効にならない。

```text
DXVK_QVFR=1 %command%
```

ルートの `qvfr-control.txt` が現在のシーン限定モードの設定元。付属ビルドは source root からその絶対 Windows パスを埋め込むため、ゲームの作業ディレクトリに依存しない。旧 `DXVK_QVFR_CONTROL_FILE` による別パス指定も可能。QVFR 有効時のログは、`DXVK_LOG_PATH` の明示指定がなければ、このファイルと同じディレクトリの `logs/` に保存する。通常のログレベルは info。HUD など DXVK 全体の設定は従来の DXVK 設定のまま。

付属ビルドスクリプトは Proton のログレベル既定値も調整する。`DXVK_QVFR=1` では `info`、それ以外では従来どおり `none`。明示した `DXVK_LOG_LEVEL` は優先する。元の Proton は常に `none` を補うため、この調整がないと起動引数からログ設定を外した実行は記録されない。`profile=off` でも通常の stats と設定変更のログは残り、GPU timestamp と CPU 計測を停止する。

### 周辺倍率・注視倍率・画面上の範囲を個別に調整する

シーン限定モードでは次の 3 値を独立して指定できる。すべて `0 < 値 <= 1`。倍率は縦横それぞれに適用する。

| 設定 | 意味 | 既定値 |
| --- | --- | --- |
| `scale` | 周辺の描画解像度倍率。片目全体の元解像度に対する比 | `0.5` |
| `focus_scale` | 注視領域の描画解像度倍率。画面上の同じ範囲を元解像度で描いた場合に対する比 | `1.0` |
| `focus` | 注視領域が占める画面上の幅・高さ。それぞれ片目全体に対する比 | `0.25` |

例えば片目 2592×2592 で `scale=.25 / focus_scale=.75 / focus=.125` の場合、周辺は 648×648、注視の画面上の範囲は 324×324、その描画用画像は 243×243。`focus_scale` だけを変えても画面上の範囲・視線位置は変わらない。注視のカラー・深度・stencil の取り込み／合成も倍率に追従する。最低 1 ピクセルへ切り上げる。`focus_scale` は旧 virtual/post モードでは 1 に固定する。

```sh
./qvfr-control.py set scale .125
./qvfr-control.py set focus_scale .75
./qvfr-control.py set focus .0625
./qvfr-control.py status
```

`status` は片目ごとの周辺画像寸法・注視画像寸法・画面上の注視範囲・描画画素数の比率を表示する。これは要求値からの計算で、実際に反映された値はログの `QVFR control`／`QVFR layout` で確認する。画素数比率は周辺と注視の面積の和であり、atlas の余白、MSAA、合成・頂点処理・通常描画の負荷を含まない。極端な縮小でもそれらの費用は残る。設定変更に伴う atlas の寸法変更では再確保が発生しうるが、視線位置だけの変更では再確保しない。

### QV と通常描画の計測

`profile=on` で 120 フレームごとに計測する。`profile_passes=sampled`（既定）は QV／通常描画それぞれ最大 8 パスの抽出計測で、`replay-pass-sample`／`native-pass-sample` を記録する。これは全描画時間ではない。

`profile_passes=full` は各カテゴリの全パスを追い、`replay-pass-total`／`native-pass-total` を記録する。QVFR の `enabled=off` でも通常描画を計測できる。各カテゴリ最大 8192 パス、GPU query の回収は非同期で同時に 1 計測フレームだけ保持する。`clipped=0`、`failed=0`、該当する `replay_skipped=0`／`native_skipped=0` が全パスを回収できた条件。上限や未完了で計測できない区間を推定で埋めない。全パス計測は query 発行が増え、計測フレームの負荷を増やすため、普段の FPS 比較では sampled または `profile=off` に戻す。

GPU の pass timing は begin rendering・attachment load 後から end rendering・store 前までの区間。取り込み・合成の meta 処理は従来の別 phase で計測し、通常描画の compute、コピー、パス開始前の同期、フレーム全体の待ち時間は pass total に含めない。pass 区間には間に発行された同期等が含まれうるため、純粋な頂点・画素 shader 時間の分類でもない。

`QVFR cpu-record` は当該計測フレームのアプリ draw 呼び出しを外側で測り、QV 化された呼び出しを `qvfr_ms`、それ以外を `native_ms` として合計する。再帰的な周辺／注視 draw は二重計上しない。直接描画・間接描画を対象とし、QV 判定や描画状態の設定も含む。CS 側のコマンド記録時間であり、Unity の更新や D3D11 側の命令生成時間は含まない。GPU timing の各 phase に付く `cpu_ms` は区間の経過時間で、この `cpu-record` と重複するので加算しない。`profile=off` では CPU 計測も停止する。

```sh
./qvfr-control.py set profile on
./qvfr-control.py set profile_passes full
./qvfr-control.py set profile_passes sampled
```

### 危険な取り込み・合成省略（シーン限定モード）

次の 3 項目はすべて既定 `off`。`qvfr-control.txt` で個別に切り替えられる。`SCENE_ONLY=0` の旧実験モードでは適用しない。既存の UNSAFE replay を有効にする必要はない。

| 設定 | `on` で省略する処理 |
| --- | --- |
| `unsafe_skip_image_seed` | 元画像から QV atlas へのカラー・深度の取り込み。途中のカラー・深度 refresh も省略 |
| `unsafe_skip_stencil_seed` | 元画像から QV atlas への stencil 取り込み |
| `unsafe_skip_stencil_compose` | QV atlas から元画像への stencil 合成 |

代替のクリアは行わず、atlas の初期内容または前回の内容をそのまま使う。深度の残留による非表示、残像、マスクの破綻などを許容した性能比較用。stencil 合成を省略した元画像には元の stencil が残る。QV 中の stencil test/write、カラー・深度の最終合成は維持する。`stencil=off` と違い、stencil を使う描画を QV 対象から外す設定ではない。

`QVFR control` は適用中の 3 フラグを記録する。stats の累計 `unsafe_image_seeds_skipped`（片目の初期取り込み 1 組、または途中 refresh ごと）、`unsafe_stencil_seeds_skipped`（片目の取り込みごと）、`unsafe_stencil_composites_skipped`（片目の合成ごと）で省略を確認する。`stencil_composites` は実際に合成した回数だけを数える。画像 seed の timing にはコピー以外の準備や初期割り当ても含むため、省略後も項目自体は残る。切り替え時は変更前の設定で pending を確定してから適用する。

```sh
./qvfr-control.py set unsafe_skip_image_seed on
./qvfr-control.py set unsafe_skip_stencil_seed on
./qvfr-control.py set unsafe_skip_stencil_compose on
```

通常動作に戻すときは 3 項目をすべて `off` にする。

### カラーの一回合成と深度合成の省略

`color_single_pass=on`（既定）では、HDR `R16G16B16A16_SFLOAT` の 2D 画像を片目につき一回の描画で合成する。低解像度を全面へ拡大した後で focus を転送する代わりに、出力位置から atlas の peripheral／focus を選択する。同じ sample 数の MSAA を維持し、focus は `focus_scale=1` なら各 sample をそのまま読み、縮小時は描画画像の対応位置から同じ sample ID を読み、peripheral は従来の nearest MSAA blit と同じ sample-grid mapping を使う。画像サイズ・視線位置を pipeline key に含めず push data で更新するため、視線移動による pipeline 再生成はない。シーン限定モード以外、未対応 format／view 等では従来経路を使う。`color_single_pass=off` で旧方式へ戻せる。

`color_fade_pixels=0`（既定、0〜256 の整数）なら境界は従来と同じ切り替え。例えば `24` なら focus の境界から内側へ 24 出力ピクセルの帯で低解像度から高解像度へ線形にフェードする。実際の幅は focus の短辺の半分以下に制限する。両画像の読み取りが必要なのはこの帯だけで、それ以外は一回の読み取り。深度や stencil はフェードしない。

`unsafe_skip_depth_compose=on` は元画像への深度合成を省略する危険オプション。既定は **off**。元画像の深度が QV 描画結果を反映しなくなるため、後続 UI 等の深度判定・深度 sampling が壊れる可能性がある。QV 自体の深度 test/write やカラー合成は維持する。シーン限定モードでのみ適用し、スキップした pending depth は消費済みとして扱う。

```sh
./qvfr-control.py set color_single_pass off  # 旧合成との比較
./qvfr-control.py set color_single_pass on
./qvfr-control.py set color_fade_pixels 24
./qvfr-control.py set unsafe_skip_depth_compose on  # 深度省略の比較
./qvfr-control.py set unsafe_skip_depth_compose off
```

適用値は `QVFR control` に、累計回数は stats の `color_single_pass_composites`／`color_single_pass_fallbacks`／`unsafe_depth_composites_skipped` に記録する。`depth_composites` は実行回数だけを数える。`compose-color` の timing は選択した合成方式の全体を測る。

合成 shader のオフライン検証（リポジトリの `dxvk/` から実行）:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I subprojects/dxbc-spirv \
  tests/qvfr_composite_ir.cpp /tmp/qvfr-ir-native/libdxbc_spv.a -pthread -o /tmp/qvfr-composite-ir-test
/tmp/qvfr-composite-ir-test /tmp/qvfr-composite-
for n in 1 2 4 8 16; do spirv-val --target-env vulkan1.3 /tmp/qvfr-composite-$n.spv; done
```

```ini
# /home/reina/qvfr/qvfr-control.txt
enabled=on
profile=off
orientation_trace=off
gaze=shared
outline=on
size=5184x2592
samples=2
format=97
scale=0.5
focus=0.25
stencil=on
safe_instancing=on
lazy_depth_stencil=on
gaze_file=Z:/dev/shm/vrchat_dfr_gaze
```

プロファイルの ON/OFF、UV 診断、視線の fixed/shared、枠、対象の寸法／MSAA／format、周辺 scale／注視 focus、stencil、safe instancing、lazy DS をゲーム起動後に変更できる。`samples` はゲームの MSAA を変える値ではなく、QV の対象として一致させる条件。`scale`／`focus` は 0 より大きく 1 以下、寸法は各 1〜16384 で幅が偶数、samples は 1/2/4/8/16。

```sh
./qvfr-control.py set profile on
./qvfr-control.py set profile off
./qvfr-control.py set gaze fixed
./qvfr-control.py set gaze shared
./qvfr-control.py set outline off
./qvfr-control.py set scale 0.5
./qvfr-control.py on
./qvfr-control.py off
./qvfr-control.py toggle
./qvfr-control.py status
./qvfr-control.py cycle --seconds 20
```

CLI は atomic replace で変更し、変更していない設定とコメントを保存する。`on/off/toggle/cycle` で profile 等を消さない。`cycle` 終了時は開始時の enabled だけを戻し、途中で編集した他の設定は維持する。`status` はファイルの要求値であり、実機の応答ではない。実効値は `QVFR control enabled=... profile=... gaze=... file=...` のログ、profile は stats の `profile=0/1` でも確認できる。

DXVK は起動時に読み、以後 endFrame 境界で最大 250 ms ごとに再確認する。同じ設定なら描画状態を変更しない。設定が変わると、旧設定の pending な画像を全て確定して pass を閉じてから適用する。新しい視線／寸法は次のフレームで使い、フレーム途中で注視領域を動かさない。切り替え直後のフレームは一時的な合成コストがあるため性能比較から外す。既に発行した GPU timestamp／UV readback は追跡 fence 完了まで保持し、診断を OFF にしても未完了 query を再利用しない。サイズ変更前の UV capture は、capture 時点の高さで解析する。

旧 `on/off/1/0` だけのファイルも受け付ける。その他は `key=value`、`#` コメント、空行に対応。最大 4096 bytes、キー重複・未知キー・値の範囲外・不正／読めないファイルは要求全体を拒否し、動作中の設定を維持する。未指定キーは起動時の既定値を使う（前のファイルで設定した値を残す patch 方式ではない）。旧環境変数は起動時既定値として互換性を残すが、ファイルで指定した値が優先する。現在の同梱ファイルは全キーを明示し、`profile=off orientation_trace=off` を指定してある。

UV 診断を後から有効にできるよう QVFR 起動時は元の PS IR を保持するが、診断用 shader／readback の実行は `orientation_trace=on` の間だけ。既存の virtual candidate としての IR 保持も継続する。旧実験の scene-only/virtual/UNSAFE/debug モード選択は起動時の設定のままで、上記ファイルでは現在の scene-only モードの調整を扱う。以下の古い環境変数による手順は互換用の記録で、通常の QVFR 設定にはこのファイルを使う。

C++／Python の parser の共通入力比較、旧形式、ファイル優先、不正要求の rollback、CLI の set/toggle/status、コメント・他設定の保持、atomic replace をテストした。ゲーム内での動的切り替えと既定ログ保存は実機未確認。

## 現在の既定: シーン描画のみ（2026-11-05）

`DXVK_QVFR=1` のとき `DXVK_QVFR_SCENE_ONLY=1` を既定とする。以前の起動オプションに UNSAFE／UNSAFE_POST／UNSAFE_INSTANCING／VIRTUAL が残っていても、このモードでは無効にする。アバター／ワールドという Unity の名前は DXVK へ渡らないため、指定サイズ・HDR format・MSAA 数・左右半分の viewport・対応する shader／深度／stencil 等の既存の安全な条件に合うシーン描画だけを周辺／注視へ replay する。通常の direct instancing は後述の SAFE_INSTANCING で扱う。追加 stage、full-width geometry、indirect 等を UNSAFE の条件で強行しない。

保持したシーン画像への最初の shader／compute の参照、copy／resolve／blit／外部アクセス、または VR サイズの対応外描画で、保持した全眼のカラーを通常画像へ合成する。深度／stencil は後述の必要時合成を使う。以後、同じ DXVK フレーム内では分割描画を再開しない。対応外描画は省略せず元の shader と viewport で実行する。後処理の compact snapshot・virtual shader への置換・分割 copy は行わない。影などの別サイズの描画は対象画像への依存がなければ切り替えの理由にしない。

`QVFR scene-only enabled` が新版の目印。`QVFR scene-end ... reason=... vs=... ps=... resume=next-endFrame` が切り替え理由とその時の shader ID。stats の `scene_only=1 scene_ends=...` と `post_cache_batches=0 post_snapshot_batches=0 unsafe_dropped_draws=0` を確認する。シーンの途中の GrabPass や対応外描画でも早めに通常経路へ移るため、左右や全てのアバター／ワールドが必ず分割される保証はない。

再開は DXVK の `endFrame`（desktop Present）の次のフレームが基準。左右の外部提出のたびに再開すると片目間や提出後に分割へ戻ってしまうため、提出イベントでは再開しない。OpenXR フレームの厳密な識別はしていない。旧実験モードへ戻す明示的な設定は `DXVK_QVFR_SCENE_ONLY=0`。

Y Flip 調査は `DXVK_QVFR_ORIENTATION_TRACE=1` で有効化する。2026-11-05 の負荷調査版から既定は無効。性能比較時は起動オプションに以前の `=1` が残っていれば `=0` に変更する。既存の実 UV 測定を全幅に加えて左右半分の viewport、部分 scissor、blend を使う描画へ拡張した。診断用の別 attachment だけで blend／write mask を解除し、元の描画の前に復元する。元の scissor 外や片目 viewport 外の測定点は unknown となる。深度／stencil テスト・追加 stage・画像への副作用などを持つ描画は引き続き対象外。

`QVFR orientation ... left=flip-y right=... vs=... ps=...` が測定点で確認した上下反転。`terminal_candidate=1` は過去に外部提出された画像への描画であり、`target` と `QVFR external-read ... resource` を照合する。既知の提出先用に 2 回の追加測定枠を設け、中間処理が先に測定枠を使い切らないようにした。最大は中間 2＋提出先候補 2 回／フレーム、合計 1024 回／context、pending 8。非同期 readback であり、測定結果が得られてから同じ draw を事前に切り替える仕組みではない。shader ID の固定や名前だけで Unity の最終 Y Flip と断定せず、GPU で測れた UV と画像の提出経路を証拠にする。単一の正規化 2D sampling site に対応し、複数 sample 等は unknown／unsupported のままにする。

旧版で UI 以外が見えなくなった実行のログと manifest は `../logs/strict-unsafe-ui-only-20261105/` に保存した。実機結果と Y Flip の測定結果は次段に記録。以下の virtual／UNSAFE の各節は `SCENE_ONLY=0` の旧実験モードの説明である。

### 実機確認済みの基準（2026-11-05）

ユーザーから「枠も高解像度範囲も正常」と報告された。今回のシーン限定版を表示の動作確認済みの基準とする。ログと manifest は `../logs/scene-only-confirmed-20261105/`。導入済み DLL の SHA-256 を manifest と照合済み（d3d11: `3b41b9f45569fdb6e8b009f860c611a5bde6d46c2ad290614a692fbaa47b1366`、dxgi: `56b9b84ed87cecca57795e026b47a3ab3d492a5d0b6ea21916df98cd60399f17`）。

有効時の frame 7560 では `scene_ends=7077`、カラー／深度／stencil の合成は各 14154 回（各シーン終了につき左右 2 回）。後処理 replay、virtual copy／resolve／read、UNSAFE の描画・compute 省略はいずれも 0。最初のシーン終了は frame 21 の `reason=resolve-read`。終了までの一部フレームでは control によって `enabled=0` となっており、全実行時間を有効状態として扱わない。FPS／性能改善はこの報告では未評価。

Y Flip の実測も得られた。VS `vs.09f46cdd7650c27388161a211b5e17d5` と PS `fs.f0bde3d56fe0f0cceffcb66b525c3952` の組で 250 capture が `flip-y` を記録した。左右それぞれの半幅 viewport で確認され、もう片目の測定点は viewport 外のため unknown。描画先 resource `28820956`／`28820968` は、どちらも同じログの `QVFR external-read` に現れた。外部提出される画像に対する終盤の Y Flip 候補を実行時 UV と描画先の対応で捕捉できた。ログの `terminal_candidate=0` は、この版では非 virtual モードで学習集合が更新されないためであり、上記の外部提出との照合結果は resource ID による。Unity の元 shader 名や、これが提出前の厳密な最後の draw であることは未確認。現在の正常表示を維持するため、この shader に合わせた描画動作の変更は行わない。

### 視線追従時の負荷調査版（2026-11-05）

ユーザー報告は QVFR OFF 30 fps、ON 25 fps（フレーム時間は約 33.3 → 40 ms）。今回の比較ログと変更前の manifest は `../logs/scene-only-gaze-slow-20261105/` に保存した。50 個の計測フレームにおける GPU 時間の中央値は、カラー合成 0.679 ms、深度合成 0.690 ms、stencil 合成 2.585 ms、カラー／深度の seed 0.603 ms、stencil の seed 0.803 ms。phase ごとに中央値を求めた値で、GPU 全フレーム時間の内訳や ON/OFF 差の厳密な分解ではない。frame 3480→3600 と 4680→4800 では各 120 フレームに QV 720 バッチ、seed 240 組、scene-end 120 回。QV が 6 バッチ／フレームでも、両眼の取り込み・合成を払う区間がある。

コード上、注視領域を画像端に寄せても `makeScenePlan` の focus／peripheral の幅と高さは変わらない。`createImage` の条件は format・sample 数・tile の寸法であり、視線の x/y は条件に入らない。位置は dynamic viewport／scissor で変える。source の取り込み直しは新しいテクスチャの生成とは別処理。この scene-only 版では固定視線でもフレーム終了・依存で pending を確定するため、次フレームの seed 自体は必要。旧 virtual 版の固定視線での継続と同一視しない。実際に再生成／再配置が毎フレーム発生していないかを確認するため、次の累計カウンターを追加した。隣り合う有効時 stats の差分を見る。

| カウンター | 数える対象 |
| --- | --- |
| `atlas_images_created` | QV scratch のカラー／深度 Vulkan 画像を実際に生成した数。通常は一組につき 2 |
| `atlas_rebuilds` | 一度確保した cache slot の画像を format・sample 数・寸法の違いで作り直した組数。初回は含まない |
| `atlas_seed_reuses` | 新しい seed で、既存の atlas 画像をそのまま再利用した組数。draw 間の `cache_hits` と異なる |
| `seed_mapping_changed` / `seed_mapping_same` | 既存 atlas がある slot の再 seed で、前回の注視 crop の整数 x/y が変わった／同じだった組数。割り当てを数える値ではない |
| `compatibility_checks` | QVFR の busy 区間（seed／compose／replay／診断等）で `ensureImageCompatibility` を呼んだ回数 |
| `compatibility_barriers` | 上記で fast return できずアクセス／layout／usage の更新経路へ進んだ回数。GPU barrier 命令の正確な個数ではない |
| `compatibility_relocations` | 上記で backing storage を新しく割り当てて relocation を要求した回数。QVFR 以外からの割り当てや driver 内部のメモリ移動は数えない |
| `stencil_write_batches` / `stencil_readonly_batches` | QV の stencil test で、更新しうる／書き込み不要と判定した元描画バッチ数 |
| `native_direct_batches` / `native_direct_draws` | 元の画像へ実行した direct／indexed のバッチ数／draw 数。OFF 中や VR 以外の描画も含む。QV replay・UV probe は除外。間接描画・内部合成・compute は含まない |

`QVFR atlas-create ... cause=initial|format|samples|dimensions` と `QVFR compatibility-relocate` を最初の数件記録する。視線で crop が動いているのに `atlas_images_created`／`atlas_rebuilds`／`compatibility_relocations` が増えなければ、視線移動ごとの画像再生成・この経路の再配置は起きていない。ゲーム側が作る別のテクスチャまでは否定しない。`replay_passes` は QVFR busy 区間の render-pass 開始数であり、scene replay のほか内部の取り込み／合成等も含む。

安全に省ける処理を二つ修正した。以前は stencil test が有効で write mask が非ゼロなら更新扱いだったが、front／back の fail・pass・depth-fail が全て KEEP の場合は更新せず、read-only の test とする。実際の書き込みが先行していた場合、その dirty 状態は維持する。これにより読み取りしかない継続では stencil の合成を省けるが、stencil の seed は test に必要なため残す。また scene-end 後の direct／indexed draw は QV の shader／cache／適合判定を迂回し、元の経路を実行する。明示的な UV probe が有効なら、その診断だけは引き続き実行する。性能比較では UV probe を無効にする。

CPU テストでは、視線を上下左右端まで動かしても atlas の割り当て寸法が一定で crop だけ動くこと、KEEP／mask=0 が stencil dirty を生まないこと、fail／depth-fail の更新を取りこぼさず後続の read-only draw でも dirty が消えないことを検証した。新版での実機 FPS 改善とカウンターの増加パターンはまだ未確認。

### 深度／stencil の必要時合成（2026-11-05）

固定視線と追従で差がなく、軽いワールドでも GPU 99〜100%・60 fps 前後になるとの報告を受けた。変更前ログ・manifest は `../logs/scene-only-fixed-gpu-bound-20261105/` に保存。この実行は `gaze=fixed`。累計で atlas の初回生成は 8 画像、`atlas_rebuilds=0 compatibility_barriers=0 compatibility_relocations=0` であり、このログの低速化は繰り返しの atlas 再生成／この compatibility 経路の再配置によるものではない。frame 2520→2640 の平均は QV 347.47 バッチ／フレーム、通常 direct/indexed 22 draw／フレーム（間接描画や内部コピーを除く）。

20 個の計測フレームの GPU 中央値は、カラー合成 0.678 ms、深度合成 0.689 ms、stencil 合成 3.083 ms、seed 0.571 ms、stencil seed 0.795 ms、replay-pass 1.984 ms。phase ごとの値であり、GPU 全フレーム時間の厳密な分解ではない。前回の KEEP 判定修正後も、実際に stencil を書く描画があり毎フレーム合成されていた。ログでは `extShaderStencilExport=0` で、拡大 stencil コピーは bit ごとの 8 回の描画を使う経路である。

この版は scene-only のとき `DXVK_QVFR_LAZY_DEPTH_STENCIL=1` を既定とする。scene-end と endFrame でカラーだけを確定し、深度／stencil は scratch に保持する。QuadView の scene replay 自体は終了したまま。後段の shader SRV／compute／copy／resolve／readback／通常描画の depth/stencil test／外部画像アクセス等が必要とした時点で、既存のアクセス追跡経路から該当 plane を通常画像へ確定する。closed scene の direct/indexed 描画にも、この依存チェックを残す。使用しないまま次の full clear／discard へ進んだ plane は、合成せず破棄できる。部分 clear では先に必要な内容を保存する。full clear の plane 選択・単一 mip/layer 等の既存の制約は維持する。

Proton の既知の提出画像への遷移はその画像の依存を事前に確定するため、その直後の interop flush で無関係な private depth/stencil まで確定しない。画像が特定されていない汎用 interop／外部 rendering と QVFR OFF 操作では全 plane を確定する。cache slot の再利用でも未確定 plane を保護する。元の状態に戻す比較設定は `DXVK_QVFR_LAZY_DEPTH_STENCIL=0`。この場合は scene-end／endFrame の全 plane 合成と従来の clear 処理を使う。

目印は `QVFR lazy depth/stencil enabled` と stats の `lazy_ds=1`。`retained_depth`／`retained_stencil` は、その stats 時点で未合成の cache slot 数。`discarded_depth`／`discarded_stencil` は上書きで不要になった plane 数の累計。`compose-depth`／`compose-stencil` と各合成回数の増分が減り、discarded が増えれば、未使用だった合成を省けている。必要な後段処理があれば合成は残り、`compose_fallback-depth`／`compose_shader-image`／`compose_image-access`／`compose_interop-flush` 等から要求元を追う。画像の seed と二回の geometry replay は引き続き必要。全ての描画で速度改善することは保証しない。

CPU テストではカラー公開後も depth/stencil が保持されること、後段の depth 読み出しが stencil を消さないこと、次の full clear で不要 plane を破棄できること、plane の公開が同じフレームの QV を再開させないこと、既知の提出画像と汎用 interop で公開範囲を使い分けることを確認した。表示維持と FPS 改善は実機未確認。

### インスタンス描画による早期終了と HUD フレームタイムの調査（2026-11-05）

ユーザーは必要時 DS 合成版でも、重い場所で約 5 fps 低下し、軽い場所では DXVK HUD のフレームタイムグラフのばらつきが大きいと報告。視点や注視枠そのものの揺れを指した報告ではない。ログ・manifest は `../logs/lazy-ds-no-gain-20261105/` に保存。今回は `discarded_depth=0 discarded_stencil=0 retained_depth=0 retained_stencil=0` が続き、実際の通常描画が直後に depth/stencil を必要としたため、必要時合成では処理を省けなかった。110 計測フレームの中央値はカラー合成 0.692 ms、深度 0.697 ms、stencil 2.584 ms、seed 0.815 ms、stencil seed 0.697 ms。atlas 再生成・compatibility relocation は引き続き 0。

frame 14040→14160 では、QV は 4 バッチ／フレーム、通常 direct/indexed は 774.25 draw／フレーム。記録された終了理由は `instancing` が多く、最初の multi-instance draw で scene が閉じ、その後の描画を全て通常経路にしていた。別区間では QV が 300 回以上／フレームあり、場面による適用率の差が大きい。元のデータ・shader・instance ID は変えず、viewport/scissor を周辺／注視へ変えて replay する仕組みなので、通常の direct instancing は半幅 viewport と他の scene 安全条件が合えば継続できる。

この版は scene-only のとき `DXVK_QVFR_SAFE_INSTANCING=1` を既定とする。direct／indexed の instanceCount と firstInstance を保持し、通常描画と同じ配列を両 replay へ渡す。対象は左右各眼の正規の半幅 viewport に限定。全幅 stereo instancing、array layer、viewport/layer export、追加 stage、storage side effect、transform feedback、query 等は既存の制約で除外する。安全条件に通らなければ scene を閉じ通常描画へ移る。後処理 replay や indirect の UNSAFE 緩和は行わない。比較用に `DXVK_QVFR_SAFE_INSTANCING=0` で従来の単一 instance の条件へ戻せる。起動時の `safe_instancing=1` と `QVFR safe direct instancing enabled` が新版の目印。`instanced_batches` の増加、QV／native の同じ有効区間の増分、`QVFR scene-end ... reason` を比較する。

フレームタイムの揺れについては、以前の `PROFILE=1` は 120 フレームごとの計測フレームに replay-pass 全体のタイムスタンプを最大 256 区間まで挿入し、軽いワールドでも 150〜180 区間を計測した記録がある。ジッターの主因とはまだ断定できないが、診断用の負荷が集中しないよう replay-pass の計測を 8 区間までにした。他の seed／compose の計測は維持する。新しい phase 名は `replay-pass-sample` で、GPU／CPU 時間は最初の最大 8 区間の合計であり、全 replay 時間として旧版と比較してはいけない。`replay_samples`／`replay_skipped` に計測数・省略数を出す。描画自体は省略しない。前フレームの計測が in-flight の場合は既存の上限・追跡 fence の扱いを維持する。

HUD の性能比較は `DXVK_QVFR_PROFILE=0 DXVK_QVFR_ORIENTATION_TRACE=0` で診断負荷を除く。その状態でも通常 stats の描画・合成・割り当てカウンターは残る。各フレームの GPU 時間を保存したログではないため、今回のログだけからフレームタイムの分散や p99 を求めることはできない。CPU テストは左右半幅と全幅の識別、SAFE の opt-out、旧 UNSAFE 条件との分離、計測が 8 回に収まり次の計測フレームで再開することを確認する。表示の維持・FPS・HUD のばらつき改善は実機未確認。

## ビルドとテスト

このディレクトリで実行する。Meson、Ninja、MinGW、glslang が必要。DLL はルートのスクリプトでビルドし、Steam が参照する固定先 `../GE-Proton-QVFR/` へ反映する。ゲームは停止しておく。

```sh
../build-proton-qvfr.sh --jobs 4

g++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=undefined,address \
  tests/qvfr_layout.cpp -o /tmp/qvfr-layout-test
/tmp/qvfr-layout-test
g++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=undefined,address \
  tests/qvfr_gaze.cpp -o /tmp/qvfr-gaze-test
/tmp/qvfr-gaze-test
g++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=undefined,address \
  tests/qvfr_cache.cpp -o /tmp/qvfr-cache-test
/tmp/qvfr-cache-test
python3 -B -m unittest discover -s tests -p 'test_qvfr*.py' -v
```

分割テクスチャの座標変換とシェーダー変換の追加検証。ここで作る native library はテスト用の dxbc-spirv であり、DXVK DLL のビルド・反映には上記スクリプトを使う。

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=undefined,address \
  tests/qvfr_virtual.cpp -o /tmp/qvfr-virtual-test
/tmp/qvfr-virtual-test
meson setup /tmp/qvfr-ir-native subprojects/dxbc-spirv -Denable_tests=false -Denable_tools=false
ninja -C /tmp/qvfr-ir-native -j4
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I subprojects/dxbc-spirv \
  tests/qvfr_virtual_ir.cpp /tmp/qvfr-ir-native/libdxbc_spv.a -pthread -o /tmp/qvfr-virtual-ir-test
/tmp/qvfr-virtual-ir-test /tmp/qvfr-virtual-test.spv
spirv-val --target-env vulkan1.3 /tmp/qvfr-virtual-test.spv
spirv-val --target-env vulkan1.3 /tmp/qvfr-virtual-test.spv.unsafe-0.spv
spirv-val --target-env vulkan1.3 /tmp/qvfr-virtual-test.spv.unsafe-2.spv
spirv-val --target-env vulkan1.3 /tmp/qvfr-virtual-test.spv.unsafe-3.spv
spirv-val --target-env vulkan1.3 /tmp/qvfr-virtual-test.spv.unsafe-4.spv
```

ビルド出力は `../build/dxvk-win64/` にある。固定先への反映内容は `../GE-Proton-QVFR/qvfr-build.json` の SHA-256 で照合できる。

## QVFR の有効化

実機ログで観測した HDR 描画先は 5184×2592、format 97、MSAA 4 倍。左右それぞれの Viewport は 2592×2592。まず固定中央の注視領域で画質とログを確認する。

```text
DXVK_QVFR=1 DXVK_QVFR_SIZE=5184x2592 DXVK_QVFR_GAZE=fixed DXVK_LOG_LEVEL=info DXVK_LOG_PATH=/home/reina/qvfr/logs %command%
```

| 変数 | 内容 |
| --- | --- |
| `DXVK_QVFR=1` | 描画の複製・合成を有効化。省略時は元の描画 |
| `DXVK_QVFR_SIZE=5184x2592` | 対象サイズ。必須で横幅は偶数 |
| `DXVK_QVFR_FORMAT=97` | 対象 VkFormat の数値。既定は R16G16B16A16_SFLOAT |
| `DXVK_QVFR_SAMPLES=4` | 対象の MSAA 数。既定は 4 |
| `DXVK_QVFR_SCALE=0.5` | 周辺領域の一辺の解像度倍率。0 より大きく 1 以下 |
| `DXVK_QVFR_FOCUS=0.25` | 注視領域の一辺の比率。画素密度は元画像と同じ |
| `DXVK_QVFR_GAZE=fixed` | 中央固定。省略時も fixed。`shared` は既存の DFR 視線 bridge を利用 |
| `DXVK_QVFR_VIRTUAL=1` | 中間コピー／resolve と対応する読み取りへ分割表現を継承。QVFR 有効時の既定 |
| `DXVK_QVFR_STENCIL=1` | ステンシルも分割 atlas に保持して描画を継続。virtual 有効時の既定。`0` で以前の除外動作 |
| `DXVK_QVFR_UNSAFE_POST=1` | 既定オフ。画面座標などの画質破綻を許容して、適合する全幅 HDR 後処理も分割描画する。virtual 有効・DEBUG 無効時のみ |
| `DXVK_QVFR_UNSAFE_INSTANCING=1` | 既定オフ。半幅 viewport などの条件を満たす direct draw では instance 数／first instance の除外を緩める。POST 設定とは独立 |
| `DXVK_QVFR_UNSAFE=1` | 既定オフ。POST／INSTANCING を含めて制限をまとめて緩め、間接描画や追加 stage、RGBA8、近似 sampling にも分割表現を使う。virtual 有効・DEBUG 無効時のみ |
| `DXVK_QVFR_PROFILE=1` | 120 フレームごとに QVFR の GPU 時間と CPU 記録時間を非同期計測。既定は無効 |
| `DXVK_QVFR_DEBUG=1` | 合成した注視領域に水色の枠を描く。診断用で、通常は無効 |
| `DXVK_GAZE_FILE` | shared 時の視線ファイル。既定は `Z:\dev\shm\vrchat_dfr_gaze` |

既定の寸法では、各目の周辺画像は 1296×1296、注視画像は 648×648 になる。ログの `QVFR composed reason=... eye=... low=... focus=...` は最初の 8 回の合成を示す。`QVFR rendering enabled` のみでは、条件に一致する描画を実行した保証にはならない。従来の `DXVK_QVFR_TRACE=1` を併用すると、描き直した小さい画像への描画も確認できる。

`QVFR skipped reason=...` は除外条件の最初の例と、指定サイズに一致する画像での最初の例を記録する。ビューと内部画像の format、MSAA 数、mip／layer、Viewport、ステンシル、query、シェーダーの flags を含む。`target_size=1` はサイズの一致を示すだけで、対象パスと判定した意味ではない。

`QVFR stats` はコンテキストごとに最初の 3 フレームと 120 フレームごとに累計を記録する。`replayed_batches=0 composed_groups=0` なら複製・合成の経路へ入っていない。reason の件数は元の draw バッチの除外数で、GPU 完了や HMD 上の表示を保証する数値ではない。`DXVK_QVFR_DEBUG=1` の枠も、この画像が最終出力へ使われるかを見分けるための目印とする。

HDR の float ビューと RGBA16 typeless 由来の UNORM 内部画像は、mutable format の組み合わせに限り対応する。画像サイズと MSAA 数は元の条件を維持し、転送時は RGBA16 のビットを保持する。

各描画を同じ時点のリソースで周辺／注視用に実行し、カラー画像・深度画像・目の組で最大 4 グループの scratch 画像を保持する。左右の切り替え、別の描画先への切り替え、バッファの更新、物理 render pass／コマンドリストの終了だけでは合成しない。空きがなければ最も古く使われたグループを合成して再利用する。周辺と注視の画像は、グループごとに同じカラー／深度 atlas の重ならない矩形へ配置する。既定サイズでは atlas は 1944×1296 で、周辺 1296×1296 は左、注視 648×648 は右。空白部分を含め、RGBA16F／D32S8・MSAA 4 では 4 グループで約 0.6 GiB の scratch メモリを使う。デバイスの画像・framebuffer サイズ上限を超える設定は除外する。

カラー・深度・ステンシルの未合成状態は別々に保持する。通常のカラー MSAA resolve の前はカラーだけを合成し、深度と atlas を維持する。対応 draw が再開したら、元画像から再初期化せず既存 atlas へ描き足す。カラー／深度は次の draw で未合成に戻し、ステンシルは書き込みがある draw のみ未合成にする。固定視線かつ virtual 有効時は、同じ mapping をフレーム境界でも保持する。共有視線で mapping を作り直す場合、通常描画・clear の書き込み境界、退避などでは必要な未合成分を確定する。

合成するのは、対象画像を実際に使うシェーダーの読み書き、対象画像への通常描画・clear・copy・resolve・blit、画像のストレージ変更、外部共有・提出などの境界。virtual 無効時は従来どおりフレーム終了でも合成する。シェーダーが使わない古い SRV binding や無関係な画像は合成の理由にしない。部分画像のアクセスでも依存判定は画像単位なので、同じ画像の両眼を保守的に合成する。元の画像を必要とするポスト処理の前には合成が残る。

`QVFR stats` の `color_composites`、`depth_composites`、`stencil_composites` はそれぞれの合成回数。`composed_groups` は少なくとも一つの plane を確定したイベント数で、別々に確定した場合は別々に加算するため、従来の値と単純比較しない。`seeded_groups` の減少も再初期化の削減を示す。

`QVFR stats` の `composed_groups`、`seeded_groups`、`cache_hits` と `compose_shader-image`、`compose_resolve-read`、`compose_fallback-color`、`compose_fallback-depth`、`compose_external-transition` などは累計。連続する記録の差分をフレーム差で割って比較する。旧実装の実機ログでは frame 120→240 の間に 10,464 グループ（約 87.2／フレーム）を合成していた。遅延合成後の DEBUG=1 の実機ログでは frame 120→240 が 480 グループ（4／フレーム）、frame 840→960 が 1,200 グループ（10／フレーム）で、主因は MSAA resolve 前と対象外描画前だった。DEBUG=0 でも 30fps という報告があり、合成以外の負荷も減らす必要がある。性能比較では `DXVK_QVFR_DEBUG=0` とし、トレースは無効にする。

各 draw の周辺・注視描画は同じ atlas attachment の Viewport／Scissor だけを切り替えて実行する。論理的なアプリの描画先・Viewport を復元する際も物理 pass は終了しない。次の対象 draw が同じ atlas を使う場合、既存の `updateRenderTargets` の一致判定で pass を継続する。以前の「周辺 draw → attachment 切替 → 注視 draw → pass 終了」という処理による、draw ごとの二つの pass を避ける。依存するバッファ／画像の更新や別グループへの移動では、通常の DXVK の同期による pass 分割は残る。

`QVFR rendering enabled` に `scratch=atlas` と出ればこの方式の DLL。`QVFR stats` の `replay_passes` は QVFR 処理中に開始した物理描画 pass の累計、`paired_in_one_pass` は周辺・注視の間で pass を継続した draw バッチの累計。後者を `replayed_batches` と比較し、前者のフレームあたりの増分で実機の pass 削減を確認する。GPU 時間や FPS を示す数値ではない。

### MSAA を変えて比較する場合

`DXVK_QVFR_SAMPLES` は対象画像を選ぶ条件であり、ゲームの MSAA 設定を変更する変数ではない。既定値は 4。ゲームの MSAA を 2 倍や無効へ変えた際は、起動オプションも合わせて変更して再起動する。

| ゲームの MSAA | 対象の指定 |
| --- | --- |
| 4 倍 | `DXVK_QVFR_SAMPLES=4` |
| 2 倍 | `DXVK_QVFR_SAMPLES=2` |
| 無効 | `DXVK_QVFR_SAMPLES=1` |

指定値を合わせても他の条件で除外される場合がある。`QVFR rendering enabled` だけで判断せず、比較区間で `replayed_batches` が増えていることを確認する。ゲーム内で MSAA を切り替えても、この環境変数は DXVK 初期化時の値を保持する。

2026-11-04 の報告では 4 倍が約 20fps、2 倍が 72fps／GPU 使用率 70%、無効が 72fps／50%。ただし、そのログの対象指定は `samples=4` のままで、frame 1782 に実際の画像が samples=2 となって `reason=samples` で除外された。frame 1800→3240 は `replayed_batches=463395` から増えていない。4 倍の QVFR 描画と、2 倍以下の通常描画を比べた結果を含むため、MSAA だけによる速度差とは結論付けない。ログは `../logs/msaa-comparison-20261104/` に保存した。

この回の計測 frame 480（clipped=0）では、seed 5.23ms、カラー合成 5.16ms、深度合成 6.48ms、replay pass 2.59ms。計測区間内では画像の取り込み・合成の時間が大きく、4 倍 MSAA 画像の転送や blit を優先して調べる根拠になる。ゲーム全体の GPU 時間や、2 倍以下での QVFR 性能はこの値からは分からない。

### 2 倍 MSAA での負荷と深度の保持

`DXVK_QVFR_SAMPLES=2` を合わせた実機ログでは、QVFR 有効時に約 50fps、通常描画より GPU 使用率も高いとの報告。QVFR の追加負荷が描画量の削減を上回っている。frame 6840 の計測（clipped=0、failed=0）は replay pass 計 1.64ms、seed 計 4.16ms、カラー合成計 3.78ms、深度合成計 3.70ms。取り込み・合成は合計 11.63ms あり、この区間では転送・blit 側の負荷が大きい。ログは `../logs/msaa2-qvfr-50fps-20261104/` に保存した。

このログを受け、通常描画のカラー出力は `fallback-color` としてカラーだけを確定するよう変更した。深度テスト・書き込み・範囲外の depth bounds が必要な場合は従来どおり `fallback-depth` で確定する。深度を使わないカラー描画の後は atlas のカラーを無効と記録し、次の対象 draw でカラーだけを取り込み直して、未合成の深度を維持する。`color_refreshes` と timing の `refresh-color` がこの経路を示す。深度を読む sampled image は深度だけ、カラーを読む sampled image はカラーだけを確定する。storage image は保守的に全体を確定する。

当初の深度保持版はステンシル描画を除外し、focus のコピーも深度 aspect のみにして元画像のステンシルを保持した。現在は後述のステンシル保持版に進めている。`DXVK_QVFR_STENCIL=0` では以前の除外動作へ戻せる。カラー／深度の転送は引き続きステンシルを書き換えず、ステンシルの転送を独立に管理する。

### GPU／CPU 計測

atlas 版の実機ログ（DEBUG=1、約 20fps という報告）の frame 2040→2160 では、337.49 replay バッチ／フレームのすべてが周辺・注視の間で同一 pass を維持していた。一方、物理 replay pass は 185.89 回／フレーム、合成は 11.25 回／フレーム残っていた。これらの回数だけでは GPU と CPU のどちらが支配的か、atlas 化による速度の変化の原因は判断できない。ログは `../logs/atlas-20fps-20261104/` に保存した。

既存の起動オプションで `DXVK_QVFR_DEBUG=0`、`DXVK_QVFR_PROFILE=1` を指定すると、120 フレームごとに `seed`、`refresh-color`、`replay-pass`、`compose-color`、`compose-depth` に加え、`virtual-copy`、`virtual-resolve`、`virtual-materialize` を計測する。枠の描画は `outline-source` で計測する。`QVFR timing frame=... phase=... samples=... gpu_ms=... cpu_ms=... failed=... clipped=...` が出る。ms は当該フレームで収集した区間の合計で、1 回あたりの平均でも全フレームの平均でもない。

Vulkan timestamp query と既存の GPU tracking timeline を使い、数フレーム後に完了済みの結果だけを回収する。CPU 側で GPU 完了を待たず、計測のための追加 submit も行わない。一度に保持するのは 1 フレームの最大 256 区間（512 query）。上限なら `clipped=1` となり、そのフレームの全 QVFR 時間を表さない。`failed=0` で回収できた結果を比較する。timestamp の valid bit 数と周期を使って GPU 時間へ換算する。

GPU 時間には区間内で完了を待つ処理も含み、timestamp 自体も計測フレームに負荷を加える。`replay-pass` の CPU 時間は pass の記録中の区間であり、間の状態更新なども含む。いずれもゲーム全体・フレーム全体の CPU／GPU 時間ではない。性能だけを比較する回は PROFILE と DEBUG を両方無効にする。

### 中間画像へ分割表現を引き継ぐ実装

`DXVK_QVFR_VIRTUAL=1`（QVFR 有効時の既定）では、GrabPass という名前を判定せず、DXVK で観測したコピー／resolve／SRV 読み取りの依存関係へ分割表現を引き継ぐ。元の D3D11 テクスチャとサイズは論理上維持し、その内容を両眼の周辺・注視領域を詰めた不変の snapshot で表す。既定サイズの snapshot は 3888×1296 で、元の 5184×2592 より小さい。

現在は単一 mip／layer、全領域の RGBA16F カラー copy と平均 MSAA resolve に対応する。対象の両眼の描画結果が分割状態で揃っていれば、元の大きな MSAA 画像に合成せず、小さい領域だけを取得する。resolve も小さい領域内でサンプルを平均する。既存の snapshot から同じサンプル数で全体をコピーする場合は、不変の backing image を共有し GPU コピーを省く。後続のコピー／resolve もこの表現を引き継ぐ。描画中の可変 atlas を直接共有することはしないので、取得元を描き足しても過去の snapshot は変化しない。

通常の float4 2D sampling に対して、PS のテクスチャ座標を snapshot 内の座標へ変換する。屈折などで注視領域外を読む場合は周辺画像を使い、左右の目と crop の位置を各 snapshot の情報から選ぶ。bilinear の参照が別領域や padding へ入らないよう半 texel 内側へ clamp する。対応する sampler は normalized coordinate、clamp-to-edge、異方性なし（最大異方性 1 まで）、min/mag の filter が同じもの。単一 mip の implicit LOD または明示的な LOD 0 のみを扱う。

通常モードでは IR で同じ SRV の全参照を確認し、Load、Gather、寸法取得、offset、grad、比較サンプル、配列／動的 binding、整数・sparse 戻り値などが混ざっていれば、その SRV を通常画像へ確定して従来経路で処理する。compute、間接描画、storage descriptor／stream output を伴う描画も従来経路。後述の UNSAFE モードでは一部を近似処理に切り替える。通常の描画用シェーダーは変更せず、snapshot を読む draw のみで変換版 PS と専用の読み取り専用 mapping buffer（内部 CBV slot 127）へ切り替える。同じシェーダーの変換版を再利用し、視線やテクスチャごとに再コンパイルしない。

この変換版を作るため、virtual 有効時の通常 shader では runtime converter を保持する。通常描画へ変換の GPU 負荷を足さない一方、現在はその候補 shader の永続 IR cache の読み書きを省略するため、起動時のコンパイル時間とメモリは比較対象に含める必要がある。ゲームのファイルや Unity シェーダーのソースは変更せず、ゲームをデコンパイルしない。

snapshot の論理ハンドルはコンテキストごとに最大 8 個保持し、超過時は一つを通常画像へ確定する。共有 image、CPU 読み戻し、対応外の操作、通常の描画先への書き込み、外部の明示的な画像遷移なども確定地点になる。CPU から map できる転送先へは仮想化しない。通常の後処理の出力すべてを分割描画に変える実装ではなく、現時点で分割状態で通せるのは上記のコピー／resolve と読み取りの範囲。未対応の画面空間出力は通常の描画を行う。

フレーム終了や、Proton が提出画像を明示した後の interop flush だけでは、無関係な中間画像を一律に統合しない。D3D11 のデスクトップ Present も blitter が使用する入力／出力のカラー画像だけを確定し、無関係な VR 用画像は保持する。画像が指定されない汎用 interop flush と外部 rendering では保守的に全結果を確定する。次に実際に読まれるまで保持し、全領域 clear／discard で上書きする場合は対応する未合成成分を捨てる。カラーだけを clear する場合は未変更の深度を保持し、深度を clear する場合は必要なカラーだけを確定する。通常描画でも color write mask が 0 の attachment はカラーを確定・無効化しない。深度への依存やシェーダーからの読み取りは別に判定する。

初回の virtual 実機ログは `../logs/virtual-first-unchanged-20261104/` に保存した。対応経路は動いており、frame 4200→4320 の差分では平均 3 resolve／3 shader bind／2 snapshot 統合があった。一方、旧 `beginExternalRendering` がデスクトップ Present でも全画像を確定し、毎フレーム 2 件の group 合成と 2 件の snapshot 統合を発生させていた。これを上記の画像指定付き Present に変更した。この修正の実機効果は未確認。通常描画へのフォールバックも残るため、これだけで最終提出のみの合成になるとは限らない。

`QVFR rendering enabled` の `virtual=1` が有効化の目印。`virtual_copies` と `virtual_resolves` は分割表現へ引き継いだ操作、`virtual_aliases` はそのうち GPU コピーを省いた操作、`virtual_reads` は分割画像を読み取りへ bind した回数、`virtual_materialized` は通常画像へ戻した回数、`virtual_live` は保持中の snapshot 数。`discarded_color`／`discarded_depth`／`virtual_discarded` は full clear により合成せずに捨てた件数。これらは GPU 完了や表示を保証する数値ではない。`QVFR virtual materialize reason=...` は最初の 16 回の確定理由を示す。

`DXVK_QVFR_PROFILE=1` では 120 フレームごとに、未合成画像を必要とした通常描画を最大 8 件 `QVFR fallback` として記録する。plane、color mask、depth test／write／compare、stencil の compare／write mask／fail／pass／depth-fail、viewport、PS cookie を含む。全 draw の詳細トレースを有効にせず、残るカラー／深度の往復を発生させた状態を調べるための記録。

### ステンシルを含む分割描画の継続

Present を限定した実機ログは `../logs/present-scoped-55fps-20261104/` に保存した。約 55fps／GPU 100% との報告で、負荷軽減の目的は未達。frame 4200 の計測はカラー合成 4.31ms、深度合成 3.12ms、seed 3.02ms、refresh-color 0.34ms、replay 1.83ms。PS cookie 300／303 の描画が stencil ALWAYS／REPLACE、depth GREATER_OR_EQUAL で毎回通常描画へ戻り、両 plane を合成・再取り込みしていた。cookie はこの起動内の識別子で、恒久的なシェーダー名ではない。

`DXVK_QVFR_STENCIL=1` では、既存の適合条件を満たす stencil draw も周辺／注視領域へ直接描画する。初回に元画像の stencil を atlas へ取り込み、後続の stencil test と write は同じ atlas 上で継続する。カラーの snapshot／resolve のためには stencil を合成しない。実際の stencil 読み取り、通常の depth／stencil 書き込み、退避などで必要となった plane だけを元画像へ戻す。full clear は上書き対象を破棄し、残る plane を失わないよう保存する。固定視線では同じ atlas をフレーム間でも再利用する。

stencil の縮小／拡大は既存の DXVK framebuffer copy shader に入力側 extent を渡し、point sampling で行う。MSAA は sample ID ごとに保持する。`VK_EXT_shader_stencil_export` が有効なら直接出力し、ない場合は既存の 8 bit-pass の copy を使う。今回の実機ログでは同 extension は無効であり、初期化や確定時の bit-pass 自体のコストも評価が必要。focus の同サイズ領域は通常の stencil aspect copy を使う。depth-only／stencil-only の転送で他方の内容を破棄しないよう attachment と view を設定する。

`stencil_batches` が分割実行した stencil 描画数、`stencil_composites` が通常画像へ戻した回数、`discarded_stencil` が full clear により統合せず破棄した件数。当初の `seed-stencil` は `seed` 内部の計測区間であり、両者を足すと二重計上する。後述の遅延初期化版では別区間になる。実機では 72fps 固定・GPU 約 80% との報告。ログは `../logs/stencil-72fps-80gpu-20261104/` に保存した。frame 5520 の計測はカラー合成 0.68ms、深度合成 0.69ms、refresh-color 0.33ms、replay 2.82ms、snapshot 統合 0.36ms、compact resolve 0.38ms。近傍では追加 seed はなく、ステンシルによる描画の中断は大きく減った。残る full-width の後処理や対応外の読み取りは、引き続き統合の理由になる。

### 速度優先の全幅 HDR 後処理（optional）

ユーザーが画面空間処理の整合性より速度を優先する場合の追加設定は `DXVK_QVFR_UNSAFE_POST=1`。既定は `0` で、前述の 72fps／GPU 80% を得た経路を維持する。virtual 無効時や DEBUG=1 では有効にしない。今回のログに残る full-width HDR 描画は PS cookie 154 で、これが毎フレーム両眼のカラーを確定し、次の描画でカラーを再取り込みしていた。

追加モードでは、指定サイズの full-width・負の高さの viewport、単一 RGBA16F RTV、depth／stencil test 無効、instance 1 の direct draw を対象に、同じ draw を左周辺／左注視／右周辺／右注視へ実行する。viewport は元の全幅を保持したまま各 tile へ写し、scissor で crop する。両目へ左半分を繰り返す座標変換にはしない。`SV_Position` は実際の atlas 内座標のままなので、画面座標から UV を求める shader、texel size、微分、ぼかし半径、ディザなどの整合性は保証しない。これらの見た目の破綻を許容して、中間の全解像度処理と統合・再取り込みを減らす選択。

出力先が既存の両眼 MSAA atlas と一致する場合は、そのカラーへ直接描き足し、深度・ステンシルは維持する。両眼の有効な cache が揃わない場合は通常描画へ戻る。単一サンプルの HDR 後処理では、対応する virtual SRV を読む draw を、新しい compact snapshot へ出力できる。こちらは blend／logic op 無効、全 RGBA 書き込み、全画像を覆う scissor に限定し、全画面 geometry による置き換えを仮定する。古い出力を取り込まず zero clear した snapshot に描くため、実際の geometry が部分的なら未描画部分は黒になる。新しい snapshot は後続の対応読み取りや copy／resolve にそのまま渡す。

通常の query、storage／UAV、feedback loop、geometry／tessellation、layer／viewport 出力、sample coverage などの除外は維持する。ゲームの draw を丸ごと省略する設定ではなく、対応する後処理も小さい画像上で実行する。RGBA8 などの最終出力、縮小 bloom chain、compute、対応外の SRV 操作・sampler、depth 読み取りなどは引き続き通常経路であり、すべてのポスト処理を仮想化するものではない。

`QVFR rendering enabled ... unsafe_post=1` が有効化の目印。`post_cache_batches` は既存 atlas への後処理、`post_snapshot_batches` は compact snapshot 出力、`post_passes` はその実行領域数（通常 1 batch あたり 4）。`post-replay` は後処理開始から終了までの区間で、`replay-pass` や初期化と重なるため、計測値を単純に合計しない。このモードの実機での画質と速度は未検証。比較には `DXVK_QVFR_UNSAFE_POST=0` を使う。

ゲーム側 MSAA×2 と合わせる起動例:

```sh
DXVK_QVFR=1 DXVK_QVFR_SIZE=5184x2592 DXVK_QVFR_SAMPLES=2 DXVK_QVFR_GAZE=fixed DXVK_QVFR_VIRTUAL=1 DXVK_QVFR_STENCIL=1 DXVK_QVFR_UNSAFE_POST=1 DXVK_QVFR_DEBUG=0 DXVK_QVFR_PROFILE=1 DXVK_LOG_LEVEL=info DXVK_LOG_PATH=/home/reina/qvfr/logs %command%
```

### 重いワールドでの通常描画との往復

初回の重いワールドの試行は `../logs/first-heavy-world-20261104/` に保存した。軽い環境より GPU 使用率・性能が悪化したとの報告だが、同じ重いワールドで POST=0 の比較はまだなく、POST モードが原因とは判定できない。旧版ログの frame 19320 では、記録された区間にカラー合成 28 件／10.03ms、深度合成 28 件／10.49ms、seed 28 件／21.76ms、stencil 合成 6 件／8.08ms がある。`clipped=1` でフレーム全体は採取されておらず、旧版の seed は stencil 初期化を内包するため単純加算しない。PS cookie 409／507 などが stencil 無効・深度書き込み有効の状態で通常描画へ戻っている。instance 除外の累積も大きいが、旧ログには各 fallback の除外理由がなく、同一 shader の当該 draw が instance 除外だったとは断定しない。

ここでの「通常描画」は元の全解像度画像で実行する経路を指す。分割で実行することが原理的に不可能な描画とは限らず、現時点の除外条件によるものも含む。

`DXVK_QVFR_UNSAFE_INSTANCING=1` は、直接描画の instance 数 1／first instance 0 という制限だけを緩める独立したオプション。元の instance 数、first instance、instance buffer と shader をそのまま使い、既存の半幅 viewport／対象画像／pipeline の条件を満たしたときに周辺・注視へ複製する。全幅の stereo packing、query、間接描画、geometry／tessellation、storage／UAV などの除外は緩めない。未知の stereo instancing や画面空間処理の画質は実機確認が必要。POST モードを切り替える設定ではなく、`UNSAFE_POST=0` でも有効にできる。

この版では、stencil 無効の通常描画が depth を使う場合、depth だけを元画像へ確定して stencil を atlas に保持する。元 depth を書き換えた場合は `depthStale` と記録し、分割描画を再開するとき depth だけを取り込み直す。ステンシルの初期化も初めて stencil test を行う draw まで遅らせ、stencil を一度も使わない区間での 8 bit-pass の初期化を省く。これらの転送削減は追加オプションの有無にかかわらず使う。

`instanced_batches` は新たに分割実行したバッチ数、`depth_refreshes`／`refresh-depth` は stencil を保持した深度の再取り込み。`QVFR fallback` に `reason`、先頭 draw の `instances`／`first_instance`、pipeline flags も追加した。`reason=other` は間接描画などの別入口であり、instance 情報は direct draw の前回値となるため判定に使わない。比較時は同じワールド・位置・ゲーム設定・DLL で `UNSAFE_INSTANCING=0/1` のみを変え、`UNSAFE_POST` は同じ値に固定する。POST 自体の比較はその後、INSTANCING を固定して POST のみを変える。この版の性能・画質は実機未確認。

### 分割維持を最優先する UNSAFE（optional）

`DXVK_QVFR_UNSAFE=1` は、画質の破綻や描画の欠落を許容し、提出先候補へ書く直前まで分割表現を残すモード。既定はオフ。virtual 有効・DEBUG=0 で POST／INSTANCING を自動有効にする。個別設定が `0` でも master が優先する。`UNSAFE_POST=1` だけの場合は、従来の対応条件と通常描画へのフォールバックを維持する。

UNSAFE では shader の画像参照、通常描画への退避、内部 copy／resolve、画像アクセス、共有画像の準備、フレーム終了等を理由に全解像度へ戻さない。転送やアップロードなど未対応の操作が論理画像を更新しても、その更新が保持中の分割表現へ反映されず、古い内容が残る場合がある。全クリアによる破棄は引き続き可能。GPU の同期・layout・view type・format・sample 数等の制約は維持する。

- direct／indirect／indirect-count を周辺・注視へ再実行する。全幅 viewport も左右の目へ再実行する。追加 stage は従来どおり受け入れるが、layer／viewport 出力や storage side effect 等で replay できない指定サイズの VR 描画は、通常描画へ戻さず省略する。サイズの異なる補助描画は通常経路を使う。`unsafe_dropped_draws` が省略した回数。
- single-sample の HDR／RGBA8／BGRA8 の後処理は、全幅 viewport・深度／stencil／depth bounds テストなしの描画を compact snapshot へ再実行する。blend・部分 scissor は受け入れる。片目の viewport や深度／stencil を使う描画は左右のシーン replay で処理し、後処理として両眼へ再実行しない。新しい出力はゼロで初期化し、以前の出力を必要とする blend 等は正しく保存しない。入出力の相対配置は実測済み identity パスで入力計画を継承し、未測定でも `mapping=unsafe-inherit` として入力計画を引き継ぐ。実際の UV 変形の検出ではなく、破綻を許容した仮定である。
- 全領域の HDR に加え RGBA8／BGRA8 の copy／resolve も、互換な format と対応する sample 数なら compact 状態へ引き継ぐ。cache の scratch を再利用するときは、両眼が揃えばカラーを compact snapshot に退避する。退避できないカラーや深度／stencil は欠落しうる。snapshot は最大 8 個とし、上限では古い snapshot を合成せず捨てる。
- float4 2D Sample は既存の UV 変換を利用し、offset／grad／bias／LOD／寸法参照は近似処理する。変換できない参照でも、2D・単一 sample・互換 format 等を満たせば compact atlas を元の shader へ直接 bind する。Load／Gather／未変換の UV などは誤った位置や範囲外を読み、黒や崩れた画像になりうる。`unsafe_raw_reads` がこの bind 回数。
- 指定サイズの VR 画像を扱う compute は、分割出力に未対応なので原則省略する（`unsafe_dropped_compute`）。GPU の引数等を生成する writable buffer がある compute は省略せず、元の画像を参照して実行するが、その画像内容が古かったり、画像への書き込みが分割表現に反映されなかったりする。提出先候補へ書く compute は通常経路へ切り替える。

境界は、単一 layer・2D・指定サイズ・single-sample のカラー画像が外部読み取りの `TRANSFER_SRC_OPTIMAL` へ遷移することを目印にする。初めての提出先候補はこの遷移の直前に該当画像を合成する。以後はその論理画像 ID を記憶し、次にその画像へ graphics draw／compute／copy／resolve／blit で書く直前から通常経路へ切り替える。graphics draw では入力と描画先を合成し、元の shader を元の viewport／画像で実行するので、もしその最終描画が Y Flip を行うなら、その処理を分割 replay せず実行できる。画像 ID は最大 128 個。既知の提出先への最初の書き込みが、常に最後の一つの描画だと保証するものではない。

通常経路への切り替えは次の提出境界まで維持する。提出後の新しいシーンは分割へ戻す。未測定の中間 Y Flip や OpenVR Submit の bounds は検出していない。提出直前の外部読み取りを認識する仕組みであり、Submit そのものの通知ではない。外部 acquire／release、汎用 interop flush、外部 rendering、QVFR の OFF 操作は、外部利用や通常動作への復帰に必要な公開境界として合成する。

`DXVK_QVFR_ORIENTATION_TRACE=1` が残っていても、UNSAFE 中は UV probe を抑止する。probe のために入力を通常画像へ戻すことを避けるためで、起動時に `orientation probes suppressed` と出る。起動ログの `strict UNSAFE enabled` が新版の目印。`QVFR external-read` に論理画像の `resource` ID と Vulkan handle を出し、`QVFR terminal-normal ... target=...` に学習した画像への切り替えを記録する。`target=0` の graphics 記録は、既に通常経路へ切り替えた後の後続描画である。

stats の `unsafe_deferred_flushes` は抑止した合成要求数。`unsafe_retained_evictions` は scratch のカラーを compact へ退避した回数、`unsafe_dropped_snapshots` は破棄した snapshot 数、`unsafe_dropped_planes` は退避できないカラーや深度／stencil の件数。`terminal_normal_batches` は提出先候補による通常経路の操作回数（draw／compute／転送）、`terminal_images` は記憶した候補数。画像が正しいことや性能改善を保証する数値ではない。

旧版の「UNSAFE でも対応外は通常描画へ戻す」という方針を変更した。明示的な公開境界以前は、対象の VR 描画を欠落させても内部依存を理由に合成しない。実機ではどれだけ描画が残るか、外部遷移直前の合成と学習済み提出先での通常描画へ切り替えられるかを確認する。CPU テストは公開理由の判定、提出先の学習、通常経路の継続と提出後の解除、候補数の上限を検証する。実機の GPU 動作は未確認。

使用例（ゲーム側 MSAA×2）:

```sh
DXVK_QVFR=1 DXVK_QVFR_SIZE=5184x2592 DXVK_QVFR_SAMPLES=2 DXVK_QVFR_GAZE=fixed DXVK_QVFR_VIRTUAL=1 DXVK_QVFR_STENCIL=1 DXVK_QVFR_UNSAFE=1 DXVK_QVFR_DEBUG=0 DXVK_QVFR_PROFILE=1 DXVK_LOG_LEVEL=info DXVK_LOG_PATH=/home/reina/qvfr/logs %command%
```

この設定でゲームの MSAA を変更しても master は自動で対象にするため、QVFR 無効の比較には `DXVK_QVFR=0` を使う。従来の frame 中の samples 除外件数を性能の比較指標にしない。

DEBUG=1 では輪郭を調べる従来経路を使い、virtual copy は行わない。比較や切り分けには `DXVK_QVFR_VIRTUAL=0` でこの経路を無効化できる。画質、実際に virtual 経路へ入る回数、GPU 時間と FPS の改善は次の実機確認が必要。

### 最終提出の境界

GE-Proton11-12 の [vrcompositor_manual.c](https://github.com/GloriousEggroll/proton-ge-custom/blob/GE-Proton11-12/vrclient_x64/vrcompositor_manual.c) は、D3D11 テクスチャを Vulkan に変換し、`TransitionSurfaceLayout(...TRANSFER_SRC_OPTIMAL)` → `FlushRenderingCommands` → `LockSubmissionQueue` の後に native OpenVR compositor へ提出する。DXVK では `transformImage` の前に該当画像を合成し、interop の `FlushRenderingCommands` にも CS コマンドを明示的に挿入し、提出画像が既に指定された場合は無関係な中間画像を保持したまま同期する。汎用 interop 呼び出しでは全未合成分を確定する。`QVFR external-read` は transfer-source への遷移時の VkImage handle・format・extent・samples を記録する。この遷移は他の interop 利用でも発生しうるため、ログだけで OpenVR Submit と断定はしない。

手元の [xrizer submit_impl](../xrizer/src/compositor.rs) は OpenVR Submit の中で [`copy_texture_to_swapchain`](../xrizer/src/graphics_backends/vulkan.rs) を実行する。OpenXR の `xrEndFrame` はコピー後なので、そこで元画像の合成を始めても遅い。現在の構成では Proton の提出前の遷移・flush が適切な確定地点になる。途中のポスト処理も低解像度／注視画像で継続させるには、その処理の依存関係と画面空間座標への対応をさらに実装する必要があり、提出直前の合成だけに強制することはできない。xrizer 側の変更は行っていない。

周辺領域は nearest で拡大し、注視領域は同じ MSAA 数の各 plane をサンプルごとコピーする。対象領域外のカラー／深度／ステンシルは保持する。グループ開始時は元の画像から初期値を取り込み、既存の描画や clear の内容を引き継ぐ。依存関係に従って合成を遅延するが、draw の複製や物理 render pass の切り替えコストも残るため、速度向上は実機で測る。

shared モードは DFR と同じ GAZ2 ABI、Win32 mapping、sequence の heartbeat 検査を使う。起動時の古い valid packet は採用せず、100 ms 更新されなければ失効する。両目の視線を DXVK のフレーム単位で固定し、視線が無効なフレームは元の描画へ戻す。bridge の起動や新規の OpenXR セッション作成は行わない。

## 実行中の ON/OFF 比較

### 通常経路を維持した注視枠

`DXVK_QVFR_DEBUG=0 DXVK_QVFR_OUTLINE=source` は、高解像度 atlas の実際の tile の内側に水色の枠を描く。各目で最初にカラー書き込みを許可した QVFR シーン draw が終わった直後に一度だけ挿入し、それ以降の draw・合成・コピー・resolve・後処理では追加しない。枠と同じ 8 pixel の辺に Depth も最前面の値を書き込み、後の深度テスト付きシーン描画・スカイボックスによる上書きを抑える。LESS／LESS_OR_EQUAL では 0、その他では Unity の reversed-Z を前提に 1 を使う。Stencil と枠の外側の Depth は変更しない。深度テスト無効・ALWAYS の後続描画や Depth の再クリアによる上書きは防げない。Unity の UV 変換・上下反転・後処理には入力画像の一部として追従する。

一回の管理は cache slot や desktop Present ではなく、左右の目に共通の状態で行う。設定サイズの単一 layer・2D・single-sample カラー画像が外部読み取りの transfer-source layout へ遷移したことを提出境界の目印にし、その後の最初のシーン描画で左右の実行済みフラグを戻す。左右の Submit に伴う連続した遷移だけでは再描画しない。この遷移は OpenVR 専用の通知ではないため、同じ条件の別の interop 読み取りも境界とみなす。
このモードでも virtual／UNSAFE の設定を維持し、枠のために全解像度へ戻したり追加の画像をコピーしたりしない。枠の clear の書き込みと必要な pass の切り替えは追加されるため、負荷ゼロではない。`PROFILE=1` では `phase=outline-source` を計測する。色や形は後処理の影響を受け、別の描画で隠れたり、UV の対応が誤っていれば枠が一部または全部消えたりしうる。これは実際の高解像度データの伝播を確認するための目印であり、提出先に推定した矩形を重ねる枠とは異なる。

`DXVK_QVFR_OUTLINE=1` も `source` と同じ高解像度 tile への書き込みを使う。既定では無効。`DEBUG=1` の枠もこの方式に統一し、合成後の通常画像や提出直前の画像へ枠を追加するコードと、その矩形・フレームの追跡は削除した。`DEBUG=1` による virtual／UNSAFE の診断用制限は従来どおり維持する。

### 切り替え操作

起動オプションに以下を追加する。初期化には引き続き `DXVK_QVFR=1` が必要。

```sh
DXVK_QVFR_CONTROL_FILE='Z:\home\reina\qvfr\qvfr-control.txt'
```

別の端末でルートディレクトリから切り替える。

```sh
cd /home/reina/qvfr
./qvfr-control.py off
./qvfr-control.py on
./qvfr-control.py toggle
./qvfr-control.py status
./qvfr-control.py cycle --seconds 20
```

`cycle` は OFF から開始し、20 秒ごとに切り替える。Ctrl-C で終了して開始前の要求に戻す。同じワールド・視点・ゲームの MSAA を維持して比較し、切り替え直後の数秒は評価から外す。

要求ファイルは atomic replace で更新し、DXVK はフレーム終了時に最大 250 ms ごとに確認する。OFF へ移る前に未確定の分割画像を合成し、virtual snapshot も確定してから通常の描画に戻す。ON 復帰時は通常画像から必要な atlas を取り込む。読めないファイルや不正な値では直前の設定を維持する。実際に反映された状態は `QVFR control enabled=0/1` と `QVFR stats ... enabled=0/1` で確認する。CLI の `status` は要求値であり、DLL からの応答ではない。

OFF の間は QVFR の replay／virtual copy を使わないが、確保済み atlas と shader cache は保持する。起動時から `DXVK_QVFR=0` にした場合とメモリ使用量まで同じになる比較ではない。旧実験の UNSAFE／DEBUG の選択は起動時に固定される。現在の gaze／profile／scale 等は冒頭の設定ファイルから実行中に変更できる。

### 上下反転の GPU 診断

`DXVK_QVFR_ORIENTATION_TRACE=1` を既存の起動オプションに追加すると、対応する全幅・単一サンプルの後処理で、入力画像を読むシェーダーが計算した UV を GPU から回収する。`DXVK_QVFR=1` と `DXVK_QVFR_SIZE` が必要。`DXVK_LOG_LEVEL=info`、既存の `DXVK_LOG_PATH` を維持する。既定では無効。視線の配置を修正する機能ではなく、反転する描画を特定するための診断である。

```text
DXVK_QVFR_ORIENTATION_TRACE=1
```

定数名や固定した Unity の定数バッファ配置には依存しない。元の PS のテクスチャ読み取り直前にある UV の計算を残し、カラー出力を UV と有効フラグに置き換えた診断用 PS を作る。元の VS・頂点／インデックス・定数・Viewport を使って、左右の目の中央 X と上下 1/4・3/4 の位置にある各 2×2 pixel だけを別の RGBA32F 画像へ再描画する。`SV_Position` と補間も元の画面座標を保つため、診断用画像の寸法は指定サイズと同じにする。5184×2592 では約 205 MiB の追加画像を保持する。

アプリの出力へ診断色は描かない。入力の分割画像は測定前に確定し、元の画像と元の UV を使う。したがって、診断中は追加描画・画像の確定・pass の分割・shader の生成に負荷がかかり、QVFR の後処理経路も変わりうる。性能比較には使わない。元のアプリの副作用を再実行しないよう、storage・transform feedback・query・追加 stage 等は除外する。深度／stencil テストなし、blend なし、全幅の単一描画、単一の正規化 sampler を使う 2D 入力が対象。virtual sampling の対応判定とは独立して UV を測る。同じ SRV の複数の sampling 命令、texel offset、出力の読み戻し等は未対応。診断中の追加描画を通常の QVFR trace と併用すると、そちらにも診断用描画が含まれる。

`QVFR orientation frame=... sequence=... source=... target=... slot=... vs=... ps=... left=... right=... uv_tl_tr_bl_br=...` が結果。上下の全測定 pixel で入力 V が出力の Y/高さと近ければ `identity-y`、1−Y/高さと近ければ `flip-y`。他の有限値は `other`、未描画・discard・無効値は `unknown` とする。左右を別々に判定する。これは測定点の一致を示すだけで、任意の画像変形が画面全体で単純な反転だと保証するものではない。`uv_tl_tr_bl_br` は左上・右上・左下・右下の patch の先頭 pixel の実測 UV。

結果は GPU tracking timeline でコピーの完了を確認してから出力する。CPU の GPU 待ちや追加 submit は行わず、フレーム境界で回収する。各コンテキストで最大 2 capture／フレーム、未回収 8 capture、合計 1024 capture。同じ VS／PS・入力／出力画像・slot は 120 フレーム以上空ける。画像の組み合わせは最大 512、診断 shader は不対応を含め最大 256 組。`orientation unsupported` は対象の UV を一意に取り出せなかった場合であり、反転なしを意味しない。対応候補に入らないパスには結果自体が出ない。

`QVFR orientation-transfer` は指定サイズに関係する backend の copy／resolve／blit の要求を記録する。入力／出力 ID、MSAA 数、subresource、転送矩形の両端が入り、virtual copy／resolve も対象。実際の GPU コピーの実行を保証する記録ではない。入力／出力 ID と種類の組ごとに 120 フレーム以上空け、最大 512 組・合計 4096 記録へ制限する。`sequence` はコンテキスト内の直接描画バッチと転送要求の順序で、capture のログが非同期で後から出ても対応付けられる。間接描画は UV probe の対象外。

```sh
rg 'QVFR orientation' ../logs/VRChat_d3d11.log
```

`source`／`target` は同一実行内の論理画像 ID。これを使い、シーンから resolve した画像と、`flip-y` を観測した後処理の入出力を結び付ける。実行中に測定結果から新しい配置規則を学習する機能はない。保存済みの実機ログから確認した既知の HDR パスには、次節の明示的な規則を適用する。診断が無効でもその規則は有効。新しいパスや動的に変わる条件は引き続き測定する必要がある。

検証用の shader 生成と Y 判定テスト:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I subprojects/dxbc-spirv \
  tests/qvfr_orientation_ir.cpp /tmp/qvfr-ir-native/libdxbc_spv.a -pthread -o /tmp/qvfr-orientation-ir-test
/tmp/qvfr-orientation-ir-test /tmp/qvfr-orientation-
spirv-val --target-env vulkan1.3 /tmp/qvfr-orientation-0.spv
spirv-val --target-env vulkan1.3 /tmp/qvfr-orientation-1.spv
spirv-val --target-env vulkan1.3 /tmp/qvfr-orientation-6.spv
spirv-val --target-env vulkan1.3 /tmp/qvfr-orientation-7.spv
```

## 既存 DFR の視線マップを共有する

Y 反転オプションは削除した。起動オプションから `DXVK_QVFR_OUTPUT_FLIP_Y` と旧 `DXVK_GAZE_FLIP_Y` を取り除く。残っていても読み取らない。既定の仮定として、bridge の視線 V を `1−V` にして最初のシーンの高解像度領域を配置する。枠もその実際の tile に描く。これは Unity／Submit の向きを検出した結果ではなく、上下反転を既定とする実験方針。生の bridge UV は保持し、変換は計画を作る際のコピーにだけ適用する。固定中央は変わらない。未測定の出力も生の UV から同じ既定の向きに配置し、入力をさらに反転しない。反転しないと実測できた HDR パスは、下記のとおり入力の配置をそのまま継承する。

実機では `1` でも上下逆、`0` では枠は正常に見えるが高解像度領域がずれるとの報告があり、上下配置と枠の一致は未解決。オプションの削除はこの問題を修正するものではない。

UNSAFE_POST／UNSAFE の単一サンプル後処理 snapshot は、実測済みの HDR パスでは入力 snapshot の両眼の計画を継承し、出力の高解像度領域を同じ位置へ配置する。入力 atlas を読む shader の mapping は入力側のまま維持し、出力の viewport／scissor と snapshot の UV mapping を継承した領域にそろえる。現在の視線から計画を作り直さないため、入力画像の実際の高解像度画素の位置を維持できる。連続した同じパスでも反転や位置のずれを累積しない。入力 snapshot が持つ計画を使うので、このパスでは新しい視線を取得できなくても現在の入力をそのまま処理できる。

2026-11-04 のログ `../logs/orientation-identity-first-20261104/` では、frame 349〜2389 の 18 capture すべてで両眼が `identity-y` だった。対象は `vs.135010b798b162853b35cfe2e49f79ab` と `fs.3649abe482390ade1473c1588ebba0d1` の組、入力は PS t0、出力は HDR（format 97）。この組に追加 stage がなく、t0 の入力も HDR の場合に限定して領域を継承する。画像 ID は起動ごとに変わるため照合条件には使わない。shader の識別子が変わった場合や、それ以外の条件へ適用する一般的な Unity 反転検出ではない。

適用時の `mapping=measured-identity-y` では `source_y` と `output_y` が左右とも一致する。以前の frame 350 の記録は `source_y=750,775 output_y=1194,1169` であり、実測が identity なのに逆側の領域を描く配置だった。修正後のログ `../logs/orientation-identity-applied-misaligned-20261104/` では、post-output の 28 記録すべてが `measured-identity-y` で左右の source_y／output_y が一致し、frame 360〜2760 の 21 capture も両眼とも `identity-y` だった。しかしユーザーは高解像度領域と枠の上下位置が違うと報告しており、問題は未解決。外部読み取りの画像は format 37（RGBA8）だが、その画像へ至る描画の UV と Submit の上下指定は測定できていない。現在の全幅・単一 instance・深度／stencil テストなし等の probe 条件では全描画を捕捉できない。HDR の測定結果だけで HMD 上の座標との一致を保証しない。

この既知の組で t0 の HDR snapshot が利用できなければ、その snapshot 後処理を実行せず通常経路へ戻す。他の HDR／RGBA8／BGRA8 パスは既存の、そのフレームの視線から出力を逆向きに計算する unsafe な推定を維持する。これは `mapping=default-flip-y` と記録し、有効な視線を取得できたフレームのみ適用する。過去ログの `legacy-flip-y` に相当する出力側の計算は維持するが、シーン側も同じ既定の向きへ変更しているため、「各パスが入力を反転する」という意味ではない。

既定反転版のログは `../logs/default-flip-detail-mismatch-20261104/` に保存した。UV capture は 53 件すべて両眼 identity-y、post-output は 60 記録すべて source_y／output_y が一致した。ユーザーからは、枠の表示位置とワールドのオブジェクトが高解像度に見える範囲がずれ、途中で縮小されたように見えるとの報告。細部が失われた段階や原因は未特定。枠は色と深度を tile の辺へ書いたもので、枠の可視性や ROI の一致は内部の物体の解像度を保証しない。実装上の等倍 copy／focus 描画と、実際の pixel 内容が保存されることを区別する。今後は高周波の診断模様や後処理の比較により、描画時の密度と後続の sampling／合成での保持を切り分ける必要がある。

今回の方針変更を反映した DLL は、起動ログに `scene_gaze_y=default-flipped` と出る。post-output には現在保持している生の `bridge_v` も記録する。過去の 2 回の測定はシーン側の反転を行う前の結果であり、新しい既定の向きが HMD 上で正しいかは再検証が必要。実測済み HDR の領域継承は診断の有効／無効によらず適用する。未捕捉の最終 RGBA8 描画と Submit の問題が解決した保証はない。

枠はシーンの高解像度 tile に描いて後処理へ伝播させる。`QVFR post-output ... format=... mapping=... source=... target=... source_slot=... vs=... ps=... source_y=... output_y=...` で適用した規則と配置を記録する。記録頻度は最初の 8 snapshot と以後 120 snapshot ごと。shader の UV 変換の自動検出や任意の画面変形には対応していない。UNSAFE の分割維持優先モードでは前述の `unsafe-inherit` と提出先候補での切り替えが優先する。CPU テストは上下の相対関係と atlas の UV mapping を検証するが、実機の shader に対する配置の正しさを保証しない。

`/home/reina/dfr/vrchat-dfr-linux/shared/gaze_shm.h` と QVFR の共有ヘッダーは、コメントを除いて同一の GAZ2 ABI v1（64 byte）。既存の `openxr-gaze-bridge` が出力しているファイルをそのまま読む。QVFR 自体は取得プロセスを起動しない。DFR の現行 bridge は OpenXR の視線 API を使う。

既定の出力先を使っている場合は、起動オプションの `DXVK_QVFR_GAZE=fixed` を次に置き換える。

```sh
DXVK_QVFR_GAZE=shared DXVK_GAZE_FILE='Z:\dev\shm\vrchat_dfr_gaze'
```

bridge に `--file` を指定している場合は、その実際の出力先の Wine パスを使う。Steam のコンテナと取得プロセスで `/dev/shm` が共有されない場合も、双方から見えるファイルが必要。例えば既存 bridge を次の出力先へ変更し、QVFR に同じファイルを指定できる。

```sh
/home/reina/dfr/vrchat-dfr-linux/dist/gaze/openxr-gaze-bridge --file /home/reina/qvfr/gaze.shm --verbose
# 対応する Steam 起動オプション:
DXVK_QVFR_GAZE=shared DXVK_GAZE_FILE='Z:\home\reina\qvfr\gaze.shm'
```

同じ出力ファイルへ二つの取得プロセスを起動しない。古い WiVRn export の `wivrn_gaze`（GAZE 形式）は互換ではない。有効な更新がない間は固定中央に切り替えず、そのフレームの対象描画を通常経路へ戻す。現状、shared モードではフレームごとに atlas mapping を作り直すため、固定中央で測った性能を維持できるかは別途確認が必要。

2026-11-04 にユーザーが既存マップで視線追従を確認。その後の上下反転の報告を受け、上下配置の切り替えを試したが解決せず、Y 反転オプションを削除した。

同日の Home での ON/OFF 比較では ON の方が GPU 使用率が高いとの報告。比較ログと旧 DLL の manifest は `../logs/shared-home-first-20261104/` に保存した。frame 9720→9840 の 120 フレームでは seed 480 回（4／フレーム）、stencil 合成 240 回（2／フレーム）、cache-reuse に伴う合成 240 回（2／フレーム）が増加している。frame 9720 の非 clipped 計測は compose-stencil 2.917 ms、seed-stencil 0.703 ms、seed 1.247 ms。これらは個別区間の値で、フレーム全体の GPU 時間ではない。shared モードで mapping を毎フレーム作り直すことに伴う転送コストが残っており、今回の Y 反転修正でその性能問題が解決したとは扱わない。

## 対応範囲と制限

通常の描画の複製は single color target と書き込み可能な深度 attachment、単一 mip／layer、instance 数 1、first instance 0、左右独立の半分幅 Viewport に限定する。上記の optional な後処理モードでは、深度を使わない全幅 HDR 描画も追加で扱う。query、UAV／storage descriptor、stream output、geometry／tessellation shader、layer／Viewport 出力、sample mask／coverage／stencil reference 出力、feedback loop、間接描画などは元の描画へ戻す。ステンシルを read-only とする layout で書き込み state が指定された場合も除外する。array stereo や一つの full-width Viewport の内部で左右を packing するジオメトリ描画には適用しない。

注視領域は、元の大きさの Viewport を整数画素分だけ移動し、Scissor と atlas 内の矩形で切り出す。投影行列やアプリのシェーダーは書き換えない。通常の投影結果のラスタライズ位置を合わせられるが、ジオメトリのクリッピング範囲は元の視野のまま。atlas 内の位置を含む `SV_Position` や元画像の texel size を使う画面空間効果の座標補正、透明物や複雑な深度依存処理の画質は未検証であり、影響を受ける描画では不一致が起きうる。

## 描画トレース

環境変数は DXVK の初期化時に読み込む。通常は無効。

| 変数 | 内容 |
| --- | --- |
| `DXVK_QVFR_TRACE=1` | DXVK の info ログに `QVFR {…}` 形式の JSON を出力 |
| `DXVK_QVFR_TRACE_LIMIT=4096` | 全バックエンドコンテキストを合わせた最大レコード数。1〜1000000 |
| `DXVK_QVFR_TRACE_SKIP=0` | サイズ条件に一致した先頭のレコードを指定数スキップ。0〜1000000000 |
| `DXVK_QVFR_TRACE_SIZE=4000x2000` | 指定サイズの framebuffer のみ記録。省略時は全サイズ。各辺 1〜16384 |

サイズの数値は設定例であり、VRChat の実測値ではない。最初はサイズを指定せずに採取し、描画先を調べてから絞り込む。起動時の描画だけで上限に達する場合は `SKIP` を使う。フィルター前の描画は `SKIP` に含まれない。`DXVK_LOG_LEVEL=info` または `debug` が必要。

ログの集計例:

```sh
python3 tools/qvfr_trace.py /path/to/VRChat_d3d11.log
```

記録対象は backend の graphics state 設定に成功した直接描画、indexed 描画、間接描画、間接 count 描画、stream output による DrawAuto。deferred context の記録時ではなく、バックエンドへの再生時に採取する。DXVK がまとめた直接描画は元の draw ごとに記録し、間接描画は一つの呼び出し／バッチにつき一レコードとする。

レコードには framebuffer のサイズ・レイヤー数、全カラー／深度 attachment の画像 ID・Vulkan handle・format・mip・レイヤー・サンプル数、Viewport、Scissor、シェーダー名（VS、TCS、TES、GS、FS の順）、instance 数、storage descriptor／transform feedback／layer export の有無が入る。Viewport は Vulkan 座標であり、D3D11 由来では高さが負になる。Scissor は DXVK が保持する値で、GPU に渡す前の framebuffer／Viewport による clamp は反映しない。

attachment のシリアライズでは format の enum と mip の `uint8_t` を数値型へ変換する。元の型を直接 stream に渡すと、それぞれ記号名と文字になり JSON を壊す。特に mip 0 の NUL は Wine の C 文字列としての改行検出を妨げ、ログ出力のバッファ超過例外を発生させる。回帰テストは本番の serializer が出す JSON を解析して、数値の型と制御文字の不在を検査する。

`tests/qvfr_log_smoke.cpp` は本番の DXVK Logger と serializer を使う Wine 用のコンソールテスト。短いレコードと 1018 byte を超えるレコードを交互に出力する。引数に DXVK ログを渡すと QVFR レコードのみを再送でき、過去の NUL を含むログでの再現にも使える。ゲームや Vulkan の初期化は行わない。

`pass` はコンテキストごとの render pass 開始番号であり、ゲームの論理パスやフレームを表さない。`frame` は DXVK コンテキストのフレームカウンター。画像 ID・handle・コンテキストの数値は同一実行内の相関用で、解放後に再利用される可能性がある。

間接描画では GPU の引数を読み戻さないため `instances` と `first_instance` は `null`。`count` はバッチ内の描画数で、間接 count 描画では上限値。集計の `draw_count_or_upper_bound` もこの区別を引き継ぐ。`storage=true` は descriptor の存在を示し、書き込みの断定ではない。

集計はサイズ、attachment、subresource ごとに行う。横長の画像や偶数の instance 数だけでは Single Pass Stereo と判定しない。ミラー、シャドウ、ポスト処理も同じ特徴を持ちうる。シェーダー名は相関用の識別子として使い、アプリケーションのシェーダー本体は出力しない。

## 領域と投影の計算

`src/util/util_qvfr.h` の `qvfr::makePlan` は、明示した double-wide／2-layer array レイアウト、周辺領域の解像度倍率、注視領域の一辺の比率、左右それぞれの視線 UV を受け取る。視線は各目の左上を `(0, 0)`、右下を `(1, 1)` とする。無効な視線や非対応サイズでは計画を返さない。

周辺領域は目ごとに切り上げてから並べ、奇数の per-eye サイズでも左右の境界を維持する。注視領域は元画像と同じ画素密度とし、画面端では矩形全体を内側に移動する。合成先矩形も同時に返すため、切り出した画素の位置と元画像の位置が一致する。

返す投影変換は各目の同次 clip 座標に対する `x' = scaleX*x + offsetX*w`、`y' = scaleY*y + offsetY*w` で、z と w は保持する。double-wide への左右の packing **より前**に適用する必要がある。`Viewport` の縮小だけでは注視領域の投影にはならない。Unity の定数バッファ内の行列配置や、packing 後の座標への適用をこの実装が保証するものではない。

## 次の検証と拡張

1. 固定中央の枠と内外の解像度差は実機確認済み。遅延合成後も表示は維持された。atlas 版では全 replay バッチで周辺・注視の同一 pass を維持したが、約 20fps の報告。2 倍 MSAA と virtual／Present 限定版でも約 50〜55fps／GPU 100% で、画像取り込み・合成が重い。今回は `stencil_batches` の増加と `fallback-depth`／カラー・深度合成／seed の減少、UI／ステンシルの画質と `compose-stencil` の追加コストを実機で確認する。
2. 透明物、ステンシルを使う UI、soft particle、深度を読むエフェクト、ミラーなどの対象外描画との混在を確認し、必要なら対象シェーダーをさらに絞る。
3. 中間コピー／resolve と 2D 読み取りの分割表現の継承を実機で確認する。`virtual_reads`／`virtual_aliases` の増加と `virtual_materialized` の理由、full clear による破棄件数、合成時間と FPS を比較する。画面空間出力や深度の伝播など、現在の対応範囲外へ広げる。
4. 一つの full-width Viewport の内部で左右を packing する方式、array stereo、MRT、画面空間座標の補正へ対応範囲を広げる。

CPU のテストは領域計算、atlas 内の矩形の非重複・両目と端の視線の Viewport／Scissor の対応、カラー／深度／ステンシルの独立した確定と後続描画による dirty 化、深度を取り込み直した後の stencil の保持、両眼キャッシュの保持・画像依存・LRU 退避、共有視線の heartbeat、JSON 出力と集計を検査する。virtual のテストは左右の UV と crop／境界 clamp、通常シェーダーを変えない候補判定、通常／近似各 8 パターン（計 16）の sampling 対応判定、通常の Sample と近似 offset／grad／寸法参照の生成 SPIR-V の構造検証を含む。この環境では Wine のコンソールテストも `Bad system call` で停止するため、これらのテストと Windows DLL のビルド成功は GPU 動作の確認を意味しない。

2026-11-04 の残像修正: `logs/strict-unsafe-left-frozen-20261104` に旧 DLL のログと manifest を保存。フレーム 1800→1920 では scene replay は 480 回、post cache replay は 23952 回増え、描画省略は 46 のまま。片目 viewport や深度／stencil を使う描画まで post 経路が受け付けると、両眼 replay とカラーだけの更新管理になり、scene cache の更新を迂回するため、全幅・カラー処理に制限した。また深度／stencil の clear 後、保持した atlas の該当データを stale として取り込み直す。分割維持と提出先候補での通常描画への切り替えは継続する。起動ログの `post_classification=fullwidth-color-only` が修正版の目印。実機の残像解消は未確認。
