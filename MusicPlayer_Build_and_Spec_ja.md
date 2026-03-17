# Omoti MusicPlayer: Build手順と仕様書

このドキュメントは、`Omoti_musicplayer_rework.dll` を生成したときの実手順と、実装済み MusicPlayer の仕様をまとめたものです。

## 1. ビルド手順

### 1.1 前提ツール
- Visual Studio 2022 Build Tools (MSVC / `cl.exe`)
- CMake
- Ninja
- MinGW-w64 (`ld.exe` を assets 埋め込みで使用)

このリポジトリでは assets 埋め込み時に `ld.exe` を呼ぶため、`CMakeLists.txt` に `MINGW_LD` の絶対パス設定があります。

### 1.2 configure
`Developer Command Prompt` 相当を有効化した上で configure:

```bat
call C:\BuildTools\Common7\Tools\VsDevCmd.bat -arch=x64
cmake -S . -B out/build/x64-release-clean -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe -DCMAKE_CXX_FLAGS=/EHsc
```

### 1.3 build

```bat
call C:\BuildTools\Common7\Tools\VsDevCmd.bat -arch=x64
cmake --build out/build/x64-release-clean --config Release -j 8
```

### 1.4 生成物
- 本体DLL: `out/build/x64-release-clean/Omoti.dll`
- この作業での配布名: `e:\Omotiskid\Omoti_musicplayer_rework.dll`

---

## 2. MusicPlayer仕様

## 2.1 起動キー
- `O` キーで MusicPlayer メニューを開く

## 2.2 曲フォルダ
- 監視対象: `util::GetOmotiPath() / "Music"`
- 対応拡張子:
  - `.mp3`
  - `.wav`
  - `.ogg`
  - `.flac`
  - `.m4a`

## 2.3 メニュー機能
- 曲一覧表示（ファイル名 stem をタイトル表示）
- 検索ボックス（部分一致）
- 曲クリックで再生
- `Pause/Resume`
- `Stop`
- `Next`
- `Shuffle ON/OFF`
- `Refresh`（フォルダ再走査）
- `Copy Folder`（Musicフォルダパスをクリップボードへコピー）

## 2.4 再生ロジック
- 再生バックエンド: `mciSendStringW` (`winmm`)
- エイリアス: `Omoti_music_runtime`
- 曲終了時:
  - Shuffle OFF: 次の曲へ循環
  - Shuffle ON: ランダム選曲（同一曲連続回避）

## 2.5 ゲーム中のNow Playing GUI
- 画面右上に常時表示
- 表示内容:
  - `Music Player` 見出し
  - `Playing/Paused/Stopped: 曲名`

---

## 3. 関連ソース

- `src/client/misc/MusicPlayerRuntime.h`
- `src/client/misc/MusicPlayerRuntime.cpp`
- `src/client/screen/screens/MusicPlayerMenu.h`
- `src/client/screen/screens/MusicPlayerMenu.cpp`
- `src/client/screen/ScreenManager.h`（画面登録）
- `src/client/Omoti.h`, `src/client/Omoti.cpp`（旧メニューキー無効化）
- `src/client/screen/screens/HUDEditor.cpp`（キー無効化）

---

## 4. 注意点

- 既存DLLがプロセスに掴まれていると上書きコピーに失敗する。
- `M` キーの旧メニューは無効化済み（`menuKey` は実質 `0`）。
- 追加実装は `x64-release-clean` 構成を基準にしている。
