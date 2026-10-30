from pathlib import Path
import sys
p=Path('src/app/UiStyle.h')
s=Path('popup-original.h').read_text()
variant=sys.argv[1]
if variant=='early-row':
 s=s.replace('        UiLayoutComboEdit(control,*style);','        SendMessageW(control, CB_SETITEMHEIGHT, 0, MulDiv(28, style->dpi, 96));\n        UiLayoutComboEdit(control,*style);')
elif variant in ('attach-only','attach-row'):
 start=s.index('inline void UiPrepareComboPopup(')
 end=s.index('    HDC dc = GetDC(combo);',start)
 attach=s[start:end].replace('inline void UiPrepareComboPopup(', 'inline void UiAttachComboPopup(')+'}\n'
 s=s[:start]+attach+s[start:]
 row='        SendMessageW(control, CB_SETITEMHEIGHT, 0, MulDiv(28, style->dpi, 96));\n' if variant=='attach-row' else ''
 s=s.replace('        UiLayoutComboEdit(control,*style);','        UiAttachComboPopup(control, style);\n'+row+'        UiLayoutComboEdit(control,*style);')
elif variant=='eager-control':
 s=s.replace('        UiLayoutComboEdit(control,*style);','        UiPrepareComboPopup(control, style);\n        UiLayoutComboEdit(control,*style);')
elif variant!='lazy': raise RuntimeError(variant)
p.write_text(s)
