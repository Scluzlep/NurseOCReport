# NurseOCReport

用于护理科室月度工作量统计表与人员系数表的拍照识别和自动算分，支持 Windows 电脑与安卓手机，全程无需联网，下载后点开即用。

## 主要功能

- 拍照识别纸质表：自动摆正歪斜照片，自动识别表头年月，支持手写整数、小数、空白格与姓名识别。
- 权重与系数自动算分：每个班次分值、职称系数、职务系数均可单独开关，修改后立即重算总分。
- 人员名单管理：初始名单为空，可拍照识别系数表，也可直接粘贴人名或导入文档自动核对姓名。
- 快速核对与导出：点选格子自动放大原图，点按数字按钮后自动跳到下一处待确认项，支持整行增删，一键导出保留两位小数的 Excel 表格。

## 使用步骤

1. 在页面右侧 Releases 处下载电脑版程序或手机安装包并打开。
2. 先核对班次权重与人员系数，首次设置后会自动保存，下月无需重填。
3. 选择或拍摄当月工作量统计表照片，核对标黄格子后点击导出表格即可。

## 源码编译

```bash
python scripts/fetch_deps.py
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

安卓安装包编译：

```bash
python scripts/fetch_deps.py --copy-android-assets
cd android
./gradlew assembleDebug
```
