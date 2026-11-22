# NurseOCReport

面向护理科室月度工作量统计表与人员系数表的离线识别与核算工具，支持 Windows 7 SP1 及以上桌面端与 Android 7.0 及以上移动端。

## 功能概览

- 离线表格式 OCR 识别：基于 PP-OCRv5 ncnn 引擎与网格透视矫正，识别手写数字、涂改嫌疑项、空白格与人名列。
- 班次权重与系数开关：支持对每个班次权重、职称系数与职务系数单独启用或关闭，切换后即时重算。
- 人员系数管理：默认人员名单为空，支持拍照识别系数表、表格内直接编辑，以及通过名册文件校准姓名。
- 交互复核与表格导出：支持缩放查图、局部放大、待确认项跳转、四角边框微调与一键导出多工作表 `.xlsx` 文件。

## 本地构建说明

### 准备依赖与模型

```bash
python scripts/fetch_deps.py
```

如需构建 Android 安装包，可附加参数复制模型资源：

```bash
python scripts/fetch_deps.py --copy-android-assets
```

### 构建核心测试与 Windows 桌面端

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### 构建 Android 安装包

```bash
cd android
./gradlew assembleDebug
```

## 隐私与安全检查

```bash
python scripts/check_privacy.py
```
