# NurseOCReport

面向护理科室月度工作量统计表与人员系数表的离线识别与核算工具，支持 Windows 7 SP1 及以上桌面端（x64 / x86 单文件独立版）与 Android 7.0 及以上移动端（Miuix 界面）。

## v0.2.0 核心特性

- **Windows 单文件独立运行与 High-DPI**：内置压缩的 PP-OCRv5 模型资源，首次启动自动释放并校验缓存至 `%LOCALAPPDATA%\NurseOCReport\cache\`，单 `.exe` 即可运行；支持 Per-Monitor v2 高分屏缩放与跨屏 DPI 动态适配。
- **Android Miuix 界面**：从 Material3 全面迁移至 Miuix（`miuix-android 0.8.8`），单行工整的人员/权重卡片与弹层编辑，消除按钮与文本挤压。
- **双模名单导入**：支持直接粘贴或输入人员姓名文本（按空格、`、`、`:`、`：`、`，`、`,` 自动切分，并自动合并分隔的单字名），以及导入 `.txt`、`.csv`、`.docx`、`.xlsx` 名册文件校准姓名。
- **高精度数字与表头识别**：采用 `MNIST-12` + `PP-OCRv5` CTC 融合识别，完整支持 `0-9` 与 `.5` 小数手写体并将单表待确认项控制在 20 处以内；支持表头与周次行自动过滤及年月自动识别。
- **高效复核与规范导出**：支持选格自动局部放大、快捷填充后自动跳转下一处待确认格、Windows 与 Android 端工作量行增删（`新增一行`/`删除当前行`），以及保留 `0.00` 两位小数的多工作表 `.xlsx` 导出。
- **班次权重与系数开关**：支持各班次权重、职称系数与职务系数独立启用/停用并即时重算；默认人员名单完全空白，保障隐私安全。

## 本地构建说明

### 准备依赖与模型

```bash
python scripts/fetch_deps.py
```

该脚本会下载 C++ 依赖、PP-OCRv5 模型并生成 `deps/ppocr/mnist12_weights.h`。如需构建 Android 安装包，可附加参数复制模型资源：

```bash
python scripts/fetch_deps.py --copy-android-assets
```

### 构建核心测试与 Windows 单文件桌面端

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Windows 目标会在构建时通过 `scripts/pack_models_cpp.py` 将模型打包进 `NurseOCReport.exe` 资源段，执行 `NurseOCReport.exe --self-test` 可验证嵌入式模型解包与离线自检。

### 构建 Android 安装包

```bash
cd android
./gradlew assembleDebug
```

## 隐私与安全检查

```bash
python scripts/check_privacy.py
```
