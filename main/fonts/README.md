# Chinese Font

`lv_font_chinese_16.c` is generated from Noto Sans SC at 16 px and 2 bpp,
instanced at weight 600 for stronger strokes. It contains ASCII, all 6,763
GB2312 level 1/2 Hanzi, and expanded punctuation, fullwidth forms, arrows,
mathematical symbols, and geometric symbols. Emoji and other characters outside
these ranges still need explicit glyph coverage. The generated font data is
covered by the SIL Open Font License 1.1 in `LICENSE-OFL.txt`.

Install the font-generation dependency once, then regenerate on Windows:

```powershell
.\.venv-asr-fixed\Scripts\python.exe -m pip install -r .\tools\requirements-font.txt
.\tools\generate_chinese_font.ps1 -FontPath 'C:\Windows\Fonts\NotoSansSC-VF.ttf'
```
