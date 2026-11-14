#!/usr/bin/env python3
from pathlib import Path
import json
root=Path(__file__).resolve().parent
strings=json.loads((root/'strings.json').read_text())
lines=['#pragma once','namespace strings {']
for table,values in strings.items():
    for key,value in values.items():
        lines.append('inline constexpr const char *'+table+'_'+key+'='+json.dumps(value,ensure_ascii=False)+';')
lines.append('}')
(root/'src/strings.h').write_text('\n'.join(lines)+'\n')
