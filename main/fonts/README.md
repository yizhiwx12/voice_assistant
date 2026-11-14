# 中文字库来源与许可

`lv_font_chinese_16.c` 由 **Noto Sans SC** 可变字体生成，字号 16 像素、2 bpp，使用字重 600 实例以增强笔画。覆盖 ASCII、GB2312 一级/二级全部 6,763 个汉字，以及扩展标点、全角字符、箭头、数学和几何符号。表情及范围外字符仍需另行添加字形。

源字体来自 [Google Fonts 的 Noto Sans SC](https://github.com/google/fonts/tree/main/ofl/notosanssc)。本机生成流程使用 `NotoSansSC-VF.ttf`，其名称表记录版本 `2.04;241114210130;non-release`，版权为：

> © 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'.

本次核对的源 TTF SHA-256 为：

```text
763146584cf0710223441356b4395e279021b0806c196614377a7a0174ae074a
```

转换后的字库继续遵循 **SIL Open Font License 1.1**，完整版权及许可原文见 [LICENSE-OFL.txt](LICENSE-OFL.txt)，与[上游 OFL](https://github.com/google/fonts/blob/main/ofl/notosanssc/OFL.txt)对应。分发本目录字体资源时须一同保留该文件和来源说明；不要将字体资源改为项目自有代码的 MIT 许可。更换源字体版本时，应重新核对该版本的版权、许可与保留名称要求。

在 Windows 上安装生成依赖并重新生成字库：

```powershell
.\.venv-asr-fixed\Scripts\python.exe -m pip install -r .\tools\requirements-font.txt
.\tools\generate_chinese_font.ps1 -FontPath 'C:\Windows\Fonts\NotoSansSC-VF.ttf'
```
