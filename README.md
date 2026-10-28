# M5OS

教育・実験用の小型 OS。M5Stack CoreS3 (ESP32-S3, Xtensa LX7) 上で、**FreeRTOS を一度も起動せず**に
scheduler / task / timer / IPC / allocator / FS などを自作していく。

## 進捗
| Phase | 内容 | 状態 |
|---|---|---|
| 1 | Bare-metal boot, UART(USB-Serial-JTAG) 出力 | ✅ 実機確認 |
| 2 | Cooperative scheduler (`task_create/yield/sleep/exit`) | ✅ 実機確認 (ABAB…, sleep 100ms 刻み) |
| 3 | Preemptive scheduler (CCOMPARE1 tick, level-3 ISR, 自前ベクタテーブル) | ✅ 実機確認 (hog 2本が約1:1、prio3 task が負荷中も210msで起床) |
| 1.5 | LCD (CoreS3 ILI9342, `display_*` API) | ✅ 実機確認 (文字表示) |
| 4 | Memory: free-list heap, internal SRAM / PSRAM 分離, `kmalloc*` | ✅ 実機確認 (selftest ALL PASS) |
| 5 | Device abstraction (`device_open/read/write/ioctl`), mutex (BLOCKED), uart/display/buttons | ✅ 実機確認 (タッチは要手動確認) |
| 6 | M5FS + SD ブロックデバイス | ✅ M5FS は RAM ディスク上で実機検証済 / ⚠️ SD ドライバは**カード未挿入のため未検証** |
| 7 | VFS (`/dev` = devfs, `/` = M5FS), マウントテーブル | ✅ 実機確認 (VFSTEST ALL PASS) |
| 8 | Shell (`help ps kill ls cat echo mem uptime run` ほか) | ✅ 実機確認 |
| 9 | App lifecycle (`app_start/stop/list`, 権限, 資源の自動回収) | ✅ 実機確認 (APPTEST ALL PASS) |
| 10 | App loader (M5EX 独自形式, 動的ロード) | ✅ 実機確認 (M5EXTEST ALL PASS + ホストからの push 実行) |
| 11 | IPC (メッセージキュー, ブロッキング受信) | ✅ 実機確認 (MSGTEST ALL PASS) |
| 12 | GUI (ウィンドウ管理, ランチャー, Terminal/Clock/Calculator/Snake) | ✅ ロジックと入力経路は実機テスト済 / 見た目は目視確認が必要 |
| (旧) 12 | GUI / Launcher / VFS / Shell / App / IPC / GUI | 未着手 |

## ボード
| | M5Stack CoreS3 | M5Stack Tab5 |
|---|---|---|
| SoC | ESP32-S3 (Xtensa) + 8MB PSRAM, 16MB flash | ESP32-P4 (RISC-V) + 32MB PSRAM, 16MB flash |
| 画面 | ILI9342C 320x240 (SPI, 電源 IC 経由) | MIPI-DSI 5インチ 720x1280 を横向き 1280x720 (ST7121 / ILI9881C) |
| 入力 | タッチ (FT6336U) | **キーボード** (Tab5 Keyboard の I2C / USB-A の HID) |
| 画面の使い方 | GUI (ウィンドウ + ランチャー) | **全画面コンソール** (`fbcon`) + シェル |
| ビルド | `tools/build.sh cores3` (ESP-IDF 4.4.3) | `tools/build.sh tab5` (ESP-IDF 5.4.4) |

```sh
tools/build.sh tab5 build
tools/build.sh tab5 flash -p /dev/cu.usbmodemXXXX          # ボードごとに build ディレクトリと sdkconfig が別
tools/build.sh cores3 build                                # 従来どおり (build/ と sdkconfig)
```
ボード固有のコードは `*_cores3.c` とボード専用ドライバに分け、CMake (`M5OS_BOARD`) が選ぶ。
カーネルが知るボード情報は `kernel/include/m5os/board_config.h` (名前、画面サイズ、タッチ/キーボードの有無)。

## ビルドと実行
```sh
. tools/env.sh                       # IDF v4.4.3 の venv を有効化
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash
python tools/monitor.py /dev/cu.usbmodemXXXX 5   # 起動ログを数秒間表示
python tools/shell_test.py /dev/cu.usbmodemXXXX 'ps' 'ls /dev'   # シェルへコマンドを送って応答を表示
screen /dev/cu.usbmodemXXXX 115200  # 対話的に使う (終了: Ctrl-A K)
M5OS_SELFTEST=1 idf.py build        # 開発用: シェルの代わりに全回帰テストを起動時に実行
```
(USB の抜き差しでポート名が変わることがある: `ls /dev/cu.usbmodem*`)

## アーキテクチャ
```
 電源投入
   │
   ▼
 ROM ──► Espressif 2nd stage bootloader ──► call_start_cpu0()        (IDF: cache/flash/clock)
                                              │
                                              ▼
                                       start_cpu0_default()          (IDF: heap_caps_init ...)
                                              │ esp_startup_start_app()
                                              │   ▲ 本来: vTaskStartScheduler()
                                              ▼   │ -Wl,--wrap で奪取
                                     __wrap_esp_startup_start_app()  kernel/boot/boot_hook.c
                                              │
                                              ▼
 ┌──────────────────────────── M5OS ───────────────────────────────┐
 │ user/    shell  launcher                apps/  hello clock ...   │
 │ ───────────────────────── syscall 相当 API ───────────────────── │
 │ kernel/  task ─ scheduler ─ timer ─ memory ─ ipc ─ irq           │
 │ fs/      vfs ── m5fs            drivers/  uart lcd button sd ... │
 │ ───────────────── arch.h / hal_*.h  (移植境界) ───────────────── │
 │ hal/esp32s3/  context_switch.S  console_usbjtag.c  time_systimer.c│
 └──────────────────────────────────────────────────────────────────┘
```
`kernel/` は `hal/` の関数宣言 (`kernel/include/m5os/arch.h`, `hal_*.h`) だけを見る。
別 SoC への移植は `hal/<chip>/` の再実装で済む。

## FreeRTOS を使わないための規則
* libfreertos.a は IDF の他コンポーネントが依存するためリンクはされるが、**scheduler は起動されない**
  (`vTaskStartScheduler` は `--gc-sections` でバイナリから消える)。
* 使う IDF: ROM/bootloader/startup、リンカスクリプト、`soc/*_reg.h`、`hal/*_ll.h` (static inline)、`esp_rom_*`。
* 使わない IDF: `driver/*`、`esp_timer`、`esp_ipc`、newlib の `printf`/lock (内部で FreeRTOS mutex)、VFS。
  → 出力は自前の `kprintf`。
* 機械検査:
  `for o in $(find build/esp-idf/main -name '*.obj'); do xtensa-esp32s3-elf-nm -u $o; done | grep -Ei 'xTask|xQueue|xSemaphore|vPort'`
  が空であること。

## 検証済みの技術的事実 (ESP-IDF v4.4.3 ソースで確認)
| 事項 | 根拠 |
|---|---|
| 起動の最終呼び出しは `esp_startup_start_app()` で、定義は freertos 側 | `esp_system/startup.c:454`, `freertos/port/xtensa/port.c:488` |
| CoreS3 のホスト接続は USB-Serial-JTAG | `usb_serial_jtag_ll.h` を直接使用 |
| 窓付き ABI: 呼び出し元の a0–a3 は callee の sp−16 に退避、RETW が未 live の窓へ戻ると Underflow 例外で復元 | `xtensa_vectors.S` `_WindowUnderflow4` |
| 窓退避は `window_spill4` マクロで可能 (HAL 関数不要) | `xtensa/coreasm.h` |
| 割り込みベクタ (`_UserExceptionVector`, `_Level2..6Vector`, window vectors) は現在 freertos の `xtensa_vectors.S` にある | Phase 3 で自前化が必要 |

## Cooperative context switch (Phase 2)
`hal/esp32s3/context_switch.S` に詳細コメントあり。要点:
1. `window_spill4` で他の窓を全部スタックへ。
2. 自分の `a0`(戻り番地+窓増分) と `sp` だけ保存。TCB に保存するのは `sp` 1 個。
3. `sp` を入れ替え、`retw`。Underflow ハンドラが新タスクのレジスタをスタックから復元。
4. 新規タスクは `arch_task_stack_init()` が「一度 switch-out された状態」のスタックを偽造し、同じ経路で開始。

## Preemptive scheduler (Phase 3)
* **ベクタ**: IDF の medium 割り込みは FreeRTOS の `_frxt_int_enter/exit` を呼ぶので使えない。
  `VECBASE` を自前の 1KiB テーブル (`hal/esp32s3/vectors.S`) へ向け、全スロットは IDF 既存ハンドラへの `j`、
  **level 3 だけ自前**。窓ベクタや例外処理は従来どおり動く。
* **tick**: CCOMPARE1 (INT15, level 3, `core-isa.h` で確認) を 10ms 周期。`INTENABLE` を直接操作 (esp_intr_alloc は使わない)。
* **ISR の流れ**: レジスタ全保存 (割り込まれたタスクのスタック上) → **窓を全 spill** → `PS.EXCM` クリアで C 実行 →
  `sched_tick()` → 復元 → `rfi 3`。切替は C 内で協調と同じ `arch_context_switch()` を呼ぶだけなので、
  **全タスクは常に「arch_context_switch の中」で停止**し、保存形式が 1 種類で済む。
* **排他**: 単一コアなので割り込みマスク (`arch_irq_disable/restore`) で保護。
* **ハマりどころ (実機で踏んだ 2 件)**:
  1. 割り込みフレームの最上位 16 バイトは「割り込まれた関数の呼び出し元の a0–a3 退避領域」(窓付き ABI の base save area)。
     状態をそこに置くと spill で壊れる → フレーム上位 16B を予約。
  2. spill は **sp を下げる前** に行う。後だと呼び出し元レジスタが 128B 低い所に退避され、復帰時の
     Window Underflow が別の場所を読んでゴミを復元しクラッシュする。IDF の `_xt_context_save` も同じ順序。

## LCD (CoreS3)
値はすべて M5Stack 公式 (M5GFX / M5Unified, MIT) のソースから転記。推測値なし。
| 項目 | 内容 |
|---|---|
| 内部 I2C | SCL=GPIO11, SDA=GPIO12。ビットバング (`hal/esp32s3/i2c_bitbang.c`) |
| AW9523 (0x58) | ID reg 0x10 = 0x23。P1_1 = LCD_RST。出力 0x02/0x03、方向 0x04/0x05 |
| AXP2101 (0x34) | chip ID reg 0x03 = 0x4A。DLDO1 (reg 0x90 bit7, 電圧 reg 0x99) = バックライト |
| LCD バス | SCLK=36, MOSI=37, CS=3, D/C=35 (MISO 兼用; 読まないので出力のまま)。ビットバング SPI |
| パネル | ILI9342C (既定) / E 版は `-DLCD_PANEL_ILI9342E`。RGB565, MADCTL=BGR, INVON |

* 起動順: AW9523/AXP2101 設定 → パネル初期化 → 黒クリア → **その後** バックライト ON (ゴミ表示を見せない)。
* フレームバッファ無し。描画はパネルへ直接ストリーム。複数タスクからの同時描画は `/dev/display` の mutex で直列化される (カーネル内の `display_*` を直接呼ぶ場合は自己責任)。
* TODO: ハードウェア SPI (+DMA) 化、パネル C/E の自動判別 (M5GFX は D9h 経由の read-back)。

## Memory (Phase 4)
| ヒープ | 場所 | 管理 | 用途 |
|---|---|---|---|
| internal | `.bss` 内の静的 arena 192 KB (IDF の heap_caps は使わない) | `kmalloc_internal` / `kmalloc_dma` | タスクスタック、DMA、割り込みから触るもの |
| PSRAM | 8 MB, `[SOC_EXTRAM_DATA_HIGH - size, HIGH)` (IDF v4.4.3 `spiram.c` で確認) | `kmalloc_psram` | 大きなバッファ、フレームバッファ |

* `CONFIG_SPIRAM_USE_MEMMAP`: IDF はチップ初期化とマップだけ行い、自分の heap には入れない → 全域を M5OS が管理。
  初期化は `call_start_cpu0` 内 (FreeRTOS 非依存)。見つからなければ `g_spiram_ok=false` で PSRAM 無しとして継続。
* アロケータ (`kernel/memory/heap.c`): 境界タグ付き free-list。first-fit、分割、左右結合。
  16 byte ヘッダ (magic / size / prev_size) で payload は 16 byte 整列。magic で double free / ヘッダ破壊を検出。
* `kmalloc()` は internal 優先、枯渇時のみ PSRAM へフォールバック。スタックは必ず `kmalloc_internal`。
* ロックは割り込みマスク。tick ISR からも呼べる。
* `kmem_selftest()` (起動時にタスクとして実行 = tick にプリエンプトされながら): 結合の検証、3000 回のランダム
  alloc/free + パターン検証 + `heap_check()`、PSRAM 1 MiB 書込み検証、タスク 20 個の生成/終了でスタックがリークしないこと。
* TODO: PSRAM DMA 用のキャッシュライン整列、`krealloc`、PSRAM を 80 MHz へ。

## Device 層と mutex (Phase 5)
```
 app ── device_open("display") ──► handle ──► device_t { name, ops, lock }
        device_write / read / ioctl                  │ mutex_lock(dev->lock)
                                                     ▼ ops->write(...)  ──► drivers/ ──► hal/
```
* アプリは `device_*` だけを使い、レジスタや `display_*` を直接呼ばない (デモの `ui` / `log` タスクがその実例)。
* 各操作はデバイスの mutex で直列化される。エラーは負の `ERR_*` (`m5os/errno.h`)。
* `mutex` (`kernel/ipc/mutex.c`): 取れなければ `TASK_BLOCKED` (wait_obj=mutex)。unlock が最優先の待機者へ **直接ハンドオフ**。
  優先度継承なし。idle は BLOCKED になれないので競合時は yield でポーリング。割り込み文脈では使えない。
* `kprintf` は 1 呼び出し = 1 ロックで行が混ざらない (タスク文脈かつ割り込み有効のときだけロック。panic/ISR は無ロック)。
* 検証: 2 タスクが hog で CPU 飽和中に 100 行ずつ同時出力 → 202/202 行が無傷で連番も正しい。

| デバイス | read | write | ioctl |
|---|---|---|---|
| `uart` | 今ある分だけ (非ブロッキング) | コンソールへ (kprintf と排他) | - |
| `display` | - | テキスト端末 (バンド内で折り返し) | CLEAR / FILL_RECT / DRAW_RECT / DRAW_TEXT / BLIT / SET_TERM / BACKLIGHT / GET_INFO |
| `buttons` | 1 byte: bit0=A bit1=B bit2=C | - | GET_TOUCH |

* **CoreS3 に物理ボタンは無い** ので、タッチパネル下部 40px を 3 分割した **仮想ボタン A/B/C** を定義した。
  FT6336U (I2C 0x38) を `touchd` タスクが 20 ms ごとにポーリング。chip ID reg 0xA3 = 0x64 を確認済み。
  (reg 0xA8 は 0x11 ではなく 0x02 を返した。ログのみで判定には使っていない。)

### 実機テストで見つかったスケジューラのバグ (修正済み)
ラウンドロビンの起点を「直前に走ったタスクの次」にしていたため、低スロットで頻繁に起きる高優先度タスク
(`touchd`, 20 ms) が起点を毎回テーブル先頭へ巻き戻し、後ろのスロットの同優先度タスクが飢餓した
(新タスクの起動遅延が最大 570 ms)。**優先度ごとに独立したラウンドロビン位置** を持たせて解決し (最大 49 ms)、
`sched selftest` として回帰テスト化した。

## ファイルシステム M5FS (Phase 6)
```
 fs_open/read/write/...  ──►  M5FS (fs/m5fs/m5fs.c)  ──►  blockdev_t { read(lba,n,buf), write(...) }
                                                              ├─ ramdisk   (PSRAM, テスト用)
                                                              └─ sd0       (drivers/sd/sd_spi.c, SPI モード)
```
### オンディスクフォーマット v1 (リトルエンディアン, ブロック 512 B) — 定義は `fs/m5fs/m5fs.h`
| 領域 | 位置 | 内容 |
|---|---|---|
| superblock | block 0 | magic `M5FS`, version, block_size, total_blocks, 各領域の開始/長さ, free_blocks, **CRC32** |
| bitmap | 1 .. | 1 bit = 1 ブロック (1=使用中)。メタデータ領域と末尾の余りも使用中扱い |
| directory | bitmap の直後, 16 ブロック | 固定 128 エントリ × 64 B |
| data | その後 | ファイル本体 |

ディレクトリエントリ (64 B): `name[24]`(NUL終端, 最大23文字) / `type`(0=空,1=ファイル,2=dir) / `parent`(親のインデックス, 0xFF=ルート) / `flags` / `size` / `extent[4]{start,count}`。
* ファイルは最大 **4 extent** (連続ブロック列)。FAT 方式は使わない。
* 親はインデックスで指すので、パス解決は名前を辿るだけ (ディレクトリの中身を読まない)。階層の深さは任意。
* 割り当て: まず末尾 extent をその場で延長 → 無理なら first-fit の連続領域 → それも無ければ最大の空き。
  extent が 4 つで足りなければ **短い書き込み** で返し、次の書き込みは `ERR_NOSPC`。
* 一貫性: ジャーナル無し。データ → メタデータの順で書き、フォーマット時は superblock を **最後** に書く
  (途中で電源が落ちても半端なボリュームはマウントできない)。メタデータは変更ブロックだけを `fs_close()`/`fs_sync()` で書く。
* `fs_format(bd, base_lba, nblocks)` はボリュームを **デバイス上の領域** として指定する (カード全体でなくてよい)。
  `/system /apps /home` を作る。**指定範囲の内容は消える。**
* マウント時に superblock の CRC・レイアウト整合・全エントリの extent 範囲を検査し、不整合は `ERR_CORRUPT`。

検証 (`fs_selftest`, RAM ディスク 4 MiB, 実機で ALL PASS): 一覧/stat/mkdir、重複・長すぎる名前・存在しないパスのエラー、
777 バイト刻みの 100 KB 読み書き (部分ブロック RMW)、seek 上書き、append、**アンマウント/再マウント後の永続性**、
開いている fd がある間の unmount 拒否、削除で空きが完全に戻ること、断片化 (2 ブロックの穴 20 個) と extent 上限、
ディスク満杯、superblock 破損の検出、PSRAM リークなし。

### SD カード (`drivers/sd/sd_spi.c`)
* SPI モード (CMD0 → CMD8 → ACMD41 → CMD58 → CMD9/CMD10)、SDHC/SDSC 対応、読み/書きは 512 B 単位。ピンは M5GFX と同じ (CS=GPIO4)。
* LCD とバス (SCLK=36, MOSI=37, GPIO35) を共有するため `spi_bus_lock()` で排他し、GPIO35 は SD アクセス中だけ MISO 入力にする。
* ⚠️ **実カードでの動作は未検証** (カード未挿入: 起動時に `sd: no card (CMD0 -> 0xff)`)。ビルドと、RAM ディスク上の
  ファイルシステムは検証済み。SD は **自動でフォーマットしない** (`fs_format` を明示的に呼んだときだけ消える)。

## VFS (Phase 7)
```
 vfs_open("/dev/buttons")   vfs_open("/home/a.txt")
          │                          │          最長プレフィックス一致 (コンポーネント境界で判定)
          ▼                          ▼
   mount "/dev" → devfs       mount "/" → M5FS (fs.h)
   (Phase 5 の device_t)       (SD の M5FS、無ければ RAM ディスク)
```
* ファイルシステムは `vfs_fs_t` (open/close/read/write/seek/ioctl/stat/readdir/mkdir/unlink) を実装して `vfs_mount()` するだけ。
  将来の FS や、動的ローダ用の `/apps` を別マウントにする拡張も同じ口。
* **devfs**: `device_register()` 済みのデバイスが自動で `/dev/<name>` に現れる。`read("/dev/buttons")` で 1 byte のボタン状態、
  `ioctl("/dev/display", DISPLAY_IOC_*)` も同じ `vfs_read`/`vfs_ioctl`。
* `ls /` は M5FS の中身 + マウントポイント (`dev`) を合成して返す。パスは `.` `..` `//` を正規化 (相対パスは未対応: cwd は shell で)。
* 名前は `vfs_open/read/write/close`: libc の `open/read/...` と衝突するため。`#define M5OS_POSIX_NAMES` で短い名前に別名を張れる。
* ルート選択 (`kernel/boot/rootfs.c`): SD に **既に M5FS がある** ときだけ `/` にマウント。無ければ RAM ディスクをフォーマットして使う
  (揮発)。**M5FS の無いカードを自動でフォーマットすることはない**。
* デモの `ui` / `log` タスクは `vfs_*` だけでデバイスを使っている。
* 検証 (`vfs_selftest`): 名前空間、`/dev` の列挙、uart 書込 / buttons 読出 / display ioctl、ファイル I/O と seek、パス正規化、
  `/devices` が `/dev` に誤マッチしないこと、ハンドル枯渇と回復、使用中の `umount` 拒否、devfs への mkdir/unlink 拒否。
* 実機テストで見つけたバグ: コールバックが列挙を中断しても VFS が合成したマウントポイントを通知し続けた → 中断をラッパーで伝播。

## シェル (Phase 8)
```
M5OS 0.1
CPU: ESP32-S3
Scheduler: M5OS
FreeRTOS: disabled

> ps
PID  NAME             PRI  STATE
1    idle             0    RUNNABLE
2    touchd           2    SLEEPING
3    ui               3    SLEEPING
4    shell            2    RUNNING
```
| コマンド | |
|---|---|
| `help` `ps` `kill PID` `ls [PATH]` `cat FILE...` `echo TEXT [> F \| >> F]` `mem` `uptime` `run APP` | 仕様の必須セット |
| `pwd` `cd` `mkdir` `rm` `mount` | 追加。相対パスは cwd 基準で VFS が正規化 |

* シェルも **VFS だけ** を使う (`/dev/uart` を read/write)。出力は 1 メッセージ = 1 回の console ロック付き書込み。
* 入力は `/dev/uart` の **ブロッキング read** (`UART_IOC_SET_RX_TIMEOUT`, -1 = 待つ)。待っている間も書込みは止まらない
  (`DEV_F_UNLOCKED_READ`: read だけデバイスの mutex を取らない)。受信は FIFO を 1 tick ごとにポーリング (割り込み駆動は将来)。
* 行編集: Backspace, Ctrl-U (行消去), Ctrl-C (取消)。矢印キーなどの ESC シーケンスは読み捨て。履歴なし。`"..."` で空白を含む引数。
* `run APP` は Phase 9 の `app_start()` を呼ぶ。`apps` で一覧、`stop ID` で停止 (下記)。
* `kill PID`: idle とシェル自身は拒否。**殺されたタスクが持っていた mutex は解放される**
  (`task_t.held` に保持リスト。さもないと `kprintf` のロックを持ったまま殺されると全出力が止まる)。ただし mutex が守っていた
  データは更新途中のままになり得る。`kill` はアプリ用であり、カーネルタスクには使わない。
* `ps` は `task_snapshot()` (短い割り込み禁止区間でコピー)。`kprintf` は `ksnprintf` のためコールバック方式に作り直し、`%-5d` 左寄せにも対応。
* 検証: 実機でコマンドを送って応答を確認 (`tools/shell_test.py`)。編集キー、リダイレクト、cwd、エラー表示、`run hog` ×2 → `ps` → `kill` →
  `ps`/`mem` で復帰、シェルは hog 2 個が走る中でも応答。回帰テスト (`KILLTEST`: mutex 所有者を kill → 待機側が起床、スタック回収) 追加。

## アプリのライフサイクル (Phase 9)
カーネルタスクとユーザーアプリを論理的に分離する。`struct app` (`kernel/include/m5os/app.h`):
`app_id` (一意・再利用なし) / `name` / `state` / `entry` / `owner_task` (主タスクの pid) / `permissions` (+ cleanup 状態)。

```
 app_start("name") ─► RUNNING ──owner が return ─► EXITED ─┐
                         └─ app_stop / kill owner ─► KILLED ─┤  appd が後始末:
                                                             ▼   子タスク kill / 開いたままの VFS ハンドル close / umalloc メモリ解放
```
| API | |
|---|---|
| `app_start(name, &id)` | アプリ表 (`apps/apps.c`) から起動。タスクはそのアプリに属する (子タスクも継承) |
| `app_stop(id)` | 主タスクを kill → KILLED |
| `app_list(out, max)` | 実行中と終了済み (スロットは古い終了済みから再利用) |

### 権限 (`permissions`)
`C`=console(/dev/uart) `D`=display `I`=input(buttons) `r`/`w`=ファイル読み/書き(作成・削除・mkdir 含む) `s`=タスク生成。
強制する場所は OS の入り口: `device_open` (デバイスごとの `required_perm`)、M5FS アダプタ (`open/stat/readdir/mkdir/unlink`)、`task_create`。
カーネルタスク (app 無し) は全権限。拒否は `ERR_PERM`。

> **これはセキュリティ境界ではない。** ESP32-S3 には MMU が無く、全タスクが 1 つのアドレス空間を共有する。
> アプリが HAL を直接呼んだり生アドレスに書いたりすれば止められない。権限は「OS の API を通る限りの能力管理」であり、
> 事故防止と設計のモデル化のためのもの。

### 資源の回収
* `umalloc()` で確保したブロックのヘッダに所有 app_id を記録 (`ufree` = `kfree`)。カーネル内部の確保 (タスクスタック等) は
  記録しないので、アプリ終了に巻き込まれて解放されることは無い。
* VFS のハンドルは開いた app を記録。終了時に `vfs_close_owned()`。
* 後始末は専用カーネルタスク `appd` (20 ms 周期) が行う。理由: ハンドルを閉じる処理は mutex を取るため、スケジューラの
  フック (割り込み禁止) の中ではできない。フックは「状態を変えて cleanup を予約」するだけ。

### 検証 (実機)
`APPTEST ALL PASS`: 未知の名前、正常終了 (RUNNING→EXITED)、**権限** (`rogue` は display/ファイル書込/spawn を全て `ERR_PERM`、
ファイルは実際に作られない)、`notes` (rw 権限ありで書ける)、**回収** (`leaker`: 28 KB の umalloc と 3 ハンドルが stop 後に 0)、
**子タスク** (`family`: stop で親子とも終了)、`kill` で owner を殺しても stop と同じ、全体でヒープ使用量とタスク数が元に戻る。
シェルでも `run` / `apps` / `stop` / `ps` (APP 列) で確認。

## アプリローダ M5EX (Phase 10)
SD も ELF も WASM も要らない、ホストでビルドして実行時に読み込める**独自形式**。仕様は `kernel/include/m5os/m5ex.h`。

```
 host:  app.c ──gcc(xtensa)──► ELF ──tools/m5exbuild.py──► app.m5ex ──tools/m5ex_push.py (base64/serial)──► /apps/app.m5ex
 board: run app ─► validate ─► kmalloc_internal_owned ─► copy + zero bss + relocate ─► task (app_main(api, arg))
```
* **形式**: `header(64B) | image | 再配置表`。番地 0 でリンクした 1 枚のブロブ (`code+literals | rodata | data`) + bss。
  import 表もシンボル表も無い。再配置表は「リンク時アドレスが入った 32bit ワードのオフセット」の列。
* **S3 特有の工夫**: 内部 SRAM は D バス (`0x3FC8xxxx`) と I バス (`+0x6F0000`, `SOC_I_D_OFFSET`) の 2 つのアドレスで見える
  (`soc.h` で確認)。コードを D バスで書き込み、**コードを指すワードには I バスの別名、データを指すワードには D バスのアドレス**を足す。
  1 つのコピーが「実行」と「バイト単位のデータアクセス」の両方に使える。
* **API は import ではなく関数テーブル** (`m5_api_t`, `kernel/include/m5os/m5ex_api.h`): `app_main(const m5_api_t *api, void *arg)`。
  アプリはカーネルのシンボルを一切参照しない (参照するとリンクエラー)。各関数は普通のカーネル関数なので、**呼び出したアプリの権限で検査される**。
  テーブルは末尾にだけ伸ばす (互換)。`version` は非互換変更のときだけ上げる。
* **マニフェストはアプリのソース内** (`M5EX_APP("name", M5_PERM_*, stack)`) → ビルドツールがヘッダへ。
* **検証** (`m5ex_validate`): magic / version / flags / api_version / 権限ビット / サイズの整合 / 4 バイト整列 / entry がコード内 /
  ファイル長の厳密一致 / 名前 / CRC32、さらに**すべての再配置が「イメージ内の 1 ワード」で、そのワードの値が「モジュール内のアドレス」**であること。
* **回収**: イメージと起動コンテキストは新しい app_id を所有者としてヒープに確保 → アプリ終了時に Phase 9 の仕組みで自動回収。
* **SDK**: `apps/sdk/` (`m5ex_sdk.h` / 最小 C ランタイム / リンカスクリプト)。サンプルは `apps/m5ex/`。
  ファームウェアには `tools/build_m5ex.sh` が生成する `apps/m5ex/images.c` (サンプル 3 個) を埋め込み、起動時に `/apps` へ配置する (RAM ディスク時)。

```sh
. tools/env.sh
python tools/m5exbuild.py apps/m5ex/primes.c -o /tmp/primes.m5ex          # 新しいアプリをビルド
python tools/m5ex_push.py /dev/cu.usbmodemXXXX /tmp/primes.m5ex /apps/primes.m5ex --run
> primes below 5000: 669 (largest 4999), computed in 1 ms by loaded code
```
> **信頼について**: ローダが確認するのは「ファイルとして整っているか」であり、「コードが無害か」ではない。ロードした機械語は
> MMU が無いのでカーネルと同じハードウェア権限で動く。権限ビットは OS の API だけを制限する。**未知の .m5ex は未知のプログラムとして扱うこと**。
> 現状マニフェストの権限要求はそのまま許可される (署名・承認は将来)。

検証 (`M5EXTEST`, 実機): **不正イメージ 28 種を拒否** (各項目 1 つずつ壊す: magic/版/サイズ/切詰/余り/整列/entry/スタック/名前/CRC/再配置の
不整列・範囲外・モジュール外…) し、拒否時に**メモリを確保しない**こと。`m5test` (再配置された関数ポインタ表、文字列表、`.data`/`.bss`、
再帰、`switch`、除算、**libgcc の 64bit 乗算**、構造体コピー、API 経由の確保を自己検査 → 15/15)。ファイルシステムからの起動、
ディレクトリ/非 M5EX/不在ファイルのエラー、`stop` でコード+確保メモリが戻ること、全体でヒープとタスクが元に戻ること。
さらに**ファームウェアに入っていない新アプリ (`primes`) をホストでビルド → シリアルで送信 (CRC 検証) → 実行**して正しい結果 (π(5000)=669) を確認。

## IPC: メッセージキュー (Phase 11)
FreeRTOS の queue は使わず自前実装 (`kernel/ipc/msg.c`, API は `kernel/include/m5os/msg.h`)。

```
 sender ── msg_send(pid, &m) ──►  [ mailbox of pid: 8 × 32 B, FIFO ] ──► msg_receive(&m) ── receiver (BLOCKED while empty)
```
* **メッセージは固定 32 バイト**: `sender`(4) + `type`(4) + `data`(24)。送信時にコピーし、**`sender` はカーネルが刻印する (偽装不可)**。
* **メールボックスはタスクごと**、深さ 8。最初に送受信されたとき作り、タスクの回収時に解放。
* | API | |
  |---|---|
  | `msg_send(pid, m)` | 待たない。満杯なら `ERR_BUSY` |
  | `msg_receive(m)` | 来るまで **BLOCKED** で待つ |
  | `msg_try_receive(m)` | 待たない。空なら `ERR_NOMSG` |
  | `msg_send_timeout` / `msg_receive_timeout` | 期限付き (`0` = 待たない, `MSG_WAIT_FOREVER`)。切れたら `ERR_TIMEOUT` |
  | `msg_lookup(name)` | 名前からタスク/アプリの pid を引く |
* **期限付き待機**: BLOCKED のタスクにも期限 (`wakeup_us`) を持たせ、スケジューラが tick で起こす。
* **相手が消えても固まらない**: 受信側が exit/kill されると、満杯のメールボックスで送信待ちだったタスクは起こされて `ERR_NOENT`。
* 優先度の高い待機者を起こしたら、送信側はすぐ譲る (次の tick を待たない)。待機者が複数いれば優先度の高い順。
* **権限 `m` (`APP_PERM_IPC`)** を追加。アプリは持たないと送受信も `msg_lookup` もできない (`ERR_PERM`)。
* **ロード済み (M5EX) アプリからも使える**: `m5_api_t` の **末尾に** `msg_send/msg_receive/msg_lookup` を追加 (末尾追加なので既存イメージと互換)。
  `m5_msg_t` は `msg_t` と同じレイアウト (コンパイル時に検査)。
* 制約: タスク文脈のみ (割り込み/割り込み禁止中は不可)。idle は待てない (`ERR_INVAL`)。送信側がブロックしたまま死んだ場合などの回収は
  タスクの回収に任せる。ブロードキャストや送信元フィルタ付き受信は未実装。

シェル: `run echod` (カーネル側のエコーサーバ) / `run m5echo` (**ロードした機械語**のエコーサーバ) → `send PID TEXT` → `recv [MS]`。
```
> run echod
started echod as app 1  (permissions C-----m)
> send 6 hello
> recv
from pid 6 (type 0x8001): "echo:hello"
```
検証 (実機, `MSGTEST`): 自分宛てのループバック、`sender` の刻印 (偽装不可)、FIFO と満杯 (`ERR_BUSY`/`ERR_TIMEOUT` と待ち時間の実測)、
ブロッキング受信が **期限ではなく送信で起きる** こと (遅延を実測)、満杯のとき送信が **受信側の読み出し開始まで待って成功** し順序が保たれること、
**受信側を kill すると送信待ちが `ERR_NOENT` で解放される**こと、kernel タスクと **ロード済みコード** 双方のエコー往復、
`rogue` の IPC 拒否、全体でメールボックスとスタックが厳密に元へ戻ること。

## GUI (Phase 12)
```
 touch panel ──(touchd)──► guid ──events (IPC)──► front window's app        status bar  (y 0..15)
                             │                                               title bar   (y 16..29)
 app ── window_create / window_fill_rect / window_draw_text ... ──► display  client 320x170 (y 30..199)
                                                                             [ Home | Next | Close ] (y 200..239)
```
* **ウィンドウは最大化したスタック**。前面の 1 枚だけが見える (320x240 に重なり処理は割に合わない)。前面でないウィンドウへの描画は
  破棄され、前面に戻ったとき `GUI_EVT_EXPOSE` が届くのでアプリが再描画する。
* **入力は IPC メッセージ** (Phase 11): `guid` (カーネルタスク) がタッチ座標から DOWN/MOVE/UP を作り、前面ウィンドウの持ち主へ送る。
  アプリは `gui_next_event()` で受ける。座標はクライアント相対。
* **システムボタン** (下部 3 つ): **Home** = ランチャー (`GUI_WIN_HOME`) を前面へ / **Next** = 前面を最背面へ送る /
  **Close** = `GUI_EVT_CLOSE` を送り、1 秒待っても終了しなければ `app_stop`。ランチャーは Close されない。
* **API**: `window_create/destroy/focus/draw`, `window_clear/fill_rect/draw_rect/draw_text/draw_bitmap` (クライアント座標・クリップ)。
  `window_create` は **表示権限 `D`**、イベントを受けるには **`m`**。**他アプリのウィンドウへの描画・破棄・フォーカスは `ERR_PERM`**。
* **アプリが終われば窓も消える**: `appd` が `gui_release_owner()` を呼ぶ。強制 stop でも同じ。
* ロック: `s_lock` が窓表と描画を直列化。順序は `gui lock → SPI バスのロック`。
* **ランチャー**: `[Terminal] [Clock] [Calculator] [Snake]` の縦 4 ボタン。実行中のアプリはもう 1 つ起動せず前面へ。
* **Terminal**: コンソール出力のミラー (`kconsole_log_read`: 直近 2 KB のリング) とコマンドボタン (`ps mem up ls apps help`)。
  ボタンは `kconsole_inject()` で **シェルの入力へ文字を注入** する → 実際にシェルが実行する (シリアルのシェルと同一)。**キーボードは未実装**。
* **Clock**: デジタル + アナログ (整数の正弦表)。RTC は読まないので **起動からの経過時間**。
* **Calculator**: 整数のポケット電卓 (即時実行: `2+3*4=` は 20、9 桁、`/0` とオーバーフローは `Error`、`C` で復帰)。
* **Snake**: 画面タッチで「蛇の頭から見たタッチ位置の方向」へ曲がる。他の窓が前面の間は一時停止。盤面は `umalloc` (kill されても回収)。
* シェル: `tap X Y` (指の代わり。実タッチと同じ経路), `gui` (窓の一覧)。

検証 (実機 `GUITEST`): **ロジックは純粋関数にして単体テスト** (電卓 20 通り、スネークの移動・反転無視・壁・食事・自己衝突・尾の通過、時計の三角関数)。
ウィンドウ API (作成・前面・フォーカス・破棄・不正 ID・クリップ・ビットマップ範囲)。**タッチ → クライアント座標のイベント** (DOWN/MOVE/UP)、
タイトルバーとステータスバーは無反応、**Close ボタン → CLOSE イベント**。他アプリ窓への侵入拒否。アプリ終了/強制停止で窓が消える。
そして **ランチャーを `tap` で操作**: Calculator 起動 → Home → 再タップで **2 つ目を起動せず前面へ** → Next → 電卓キーを `tap` して
**7 + 8 = が 15** → `C` → Close で終了し窓が消える → Terminal の `ps` ボタンがコンソール入力へ `ps\r` を入れる → Snake/Clock の起動と Close。
終了後にヒープ使用量とタスク数が厳密に元へ戻る。
* 画面の内容そのものは読み戻せない (LCD の読み出し未実装) ので、**見た目は目で確認が必要**。

### 実装中に分かったこと
* `window_create` は権限検査の前に「GUI 起動済みか」を見ていた → 権限の無いアプリに別のエラーが返る (情報が漏れる)。**権限検査を先頭へ**。
* **ビットバング SPI は遅い**: 全画面で約 115 ms、ボタン 1 つの再描画に十数 ms。負荷 (CPU を食うタスク) があるとタッチの反映が数百 ms
  遅れ得る。固定時間の待ちに頼ったテストが落ちたので、条件を待つ形に直した。ハードウェア SPI (+DMA) 化が最大の改善点。
* `display_draw_text` は 1 文字ごとに窓設定していたため遅かった → 文字列全体を 1 回の転送にした。

## M5Stack Tab5 への移植 (ESP32-P4, RISC-V)
ビルド: `tools/build.sh tab5 build` / 書き込み: `tools/build.sh tab5 flash -p <port>` (ESP-IDF 5.4.4。他ボードは 4.4.3)。
実機 (Tab5, ST7121 パネル, Tab5 Keyboard) で確認したこと: 起動、プリエンプティブスケジューラ、シェル (シリアルと画面+キーボード)、PSRAM、表示、`hog` による横取り。

P4 で分かったこと (どれも実機で症状を見てから IDF のソースで裏を取った):
* 割り込みコントローラは CLIC。ベクタテーブル (MTVT) は **命令フェッチ**で読まれるので実行可能 RAM に静的に置く必要がある。コード RAM は書き込み保護なので実行時に埋められない。→ `hal/esp32p4/vectors_p4.S`
* MRET が割り込みレベル (mintstatus.mil) を戻すのは mcause の bit31 (interrupt) が立っているときだけ。コンテキスト切り替えが MRET で復帰するときこれを立てないと、最初の切り替え後に tick が二度と来ない。→ `context_switch.S`
* 空きの割り込み線は IDF の `_interrupt_handler` へ流す。IDF の DSI ドライバは画面更新の再開を DMA 割り込みで行うため。スケジューラ停止中は `rtos_int_enter/exit` がすぐ戻るので動く。tick 用の線は `esp_intr_reserve` で IDF から隠す。
* IDF の DPI ドライバはフレームバッファを IDF のヒープから取る。カーネルが PSRAM を全部持つと確保できないので、上位 4 MB (`M5OS_PSRAM_RESERVE`) を IDF ヒープに残す。
* **パネルは 2 種類ある** (ILI9881C / ST7123・ST7121)。見分けはタッチ IC の I2C 応答だが、ベンダー式の LCD/タッチ リセット (100 ms) を一度かけるまでタッチ IC は応答しない。この機体は ST7121。ST7123 は未実装。
* パネルは縦置き 720x1280。画面は横 1280x720 として扱い、フレームバッファへ書くときに 180° 回転した位置に置く (`display_tab5.c` の `put()`)。CPU キャッシュ越しなので描画のたびに `esp_cache_msync` で書き戻す。
* 周辺の電源・リセット (パネル/タッチのリセット、USB 5V、Wi-Fi 電源) は I/O エキスパンダ PI4IOE5V6408 (I2C 0x43/0x44) 経由。→ `hal/esp32p4/board_tab5.c`

キーボード: Tab5 Keyboard (I2C 0x6D, Ext.Port1 の G0/G1) を HID モードで読む (`drivers/tab5kbd`)。無ければ USB-A の HID ブートキーボードを探す (`drivers/usbkbd`, DWC2 を IDF の HAL 層でポーリング駆動)。**USB キーボード側は実機で未検証** (手元にあるのは I2C のキーボードだけ)。

未対応: ST7123 パネル、タッチ (GT911 / ST7123 のタッチ)、SD、RISC-V 向け M5EX SDK、バックライトの PWM 調光。

## ウィンドウとファイラー (キーボード付きボード: Tab5)
`user/tui/dqwin.c` はドラクエ風のテキストウィンドウ部品 (黒地・太い白枠・角丸・▶カーソル)。文字セル単位 (8x8 グリフ × `M5OS_FBCON_SCALE`) で配置するので 40 桁の LCD でも Tab5 の 80 桁でも同じコードで動く。描画は `display_*` API、入力は `kconsole_getc()` (矢印キーの ESC シーケンスは `dq_getkey()` が解釈) なのでシリアル端末からも操作できる。

`user/filer/filer.c` は VZ Editor のファイラーに倣った全画面ファイラー。シェルで `files [DIR]` を実行する。複数カラムのリスト・マーク・キー一発操作・最下行のキーガイド。

| キー | 動作 |
|---|---|
| ↑↓ ←→ PgUp PgDn Home End | 移動 (←→ は 1 列ぶん) |
| Enter / BS | ディレクトリに入る・ファイルを見る / 一つ上へ |
| Space | マーク切り替え |
| C / M | コピー / 移動 (マークがあればマーク全部、なければカーソル位置) |
| D | 削除 (確認あり。ディレクトリは空のときだけ) |
| K | ディレクトリ作成 |
| N | ファイル名変更 (M5FS に rename が無いのでコピー+削除) |
| V | テキストのビューア (先頭 8 KB) / 画像 (BMP・PPM) は画像ビューア |
| E / F | エディタで開く / 新規ファイルを作って開く |
| Q / ESC | 終了 |

画像ビューア (`user/viewer/imgview.c`, シェルから `view FILE` でも起動): BMP (非圧縮 8/16/24/32 ビット) と PPM/PGM (P6/P5, 8 ビット) を RGB565 に変換し、画面に収まる大きさに縮小 (小さい画像は整数倍に拡大) して中央に表示する。縮小・拡大は最近傍法。JPEG/PNG のデコーダはツリーに無いので未対応。`imgview_write_bmp()` は RGB565 の画面データを 24 ビット BMP で書き出す (カメラ用)。失敗したときは理由を `imgview_last_error()` で返し、画面にも出す。

エディタ (`user/edit/edit.c`, シェルから `edit FILE` でも起動): 32 KB までの ASCII テキスト。Ctrl-S 保存、Ctrl-Q / ESC 終了 (未保存なら確認)、矢印・Home/End・PgUp/PgDn、Tab は空白 2 つ。バッファは平らなバイト列でカーソルはオフセット、行は '\n' を走査して求め、画面は毎キー全面再描画する (どれも「単純で間違えにくい」ための選択)。未対応: 取り消し、検索、2 桁以上の Tab 幅、ASCII 以外。

制約: ディレクトリのコピー/移動/改名は未対応。fbcon は `fbcon_pause()` で描画を止め、終了時に全面を描き直す。ASCII のみ (8x8 フォントに日本語が無い)。

## 既知の制約 / TODO
* CPU1 は起動しない (`CONFIG_FREERTOS_UNICORE=y`)。
* タスクスタックは 4KB 固定 (kmalloc_internal)。オーバーフロー検出 (guard) は未実装。
* idle は `waiti` で待つ。スリープ精度は tick (10ms) 粒度。
* **タスク内で浮動小数点 / PIE (コプロセッサ) を使わないこと**: S3 は FPU(CP0) を持つが、レジスタの lazy 保存は未実装。
* strict priority なので、CPU を使い続ける高優先度タスクは低優先度を飢餓させる (テストは直列実行で干渉を避けている)。
* mutex に優先度継承がない (優先度逆転あり)。device の read は非ブロッキングのみ (ブロッキング read は Phase 8 の shell 前に追加)。
* I2C バスのロックはタッチドライバ内だけ (board_backlight 等は起動時のみ使用)。
* tick ISR は flash 上のコードを実行する。flash 書き込み (cache 無効) を行う段階で IRAM へ移す必要あり。
* `kprintf` の `%-Nd` 左寄せは `%s` のみ対応。
