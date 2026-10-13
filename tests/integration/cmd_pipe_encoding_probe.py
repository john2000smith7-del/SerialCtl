"""Win32 redirected-CMD encoding diagnosis, separate from production GUI assertions."""
import base64
import ctypes
import json
import pathlib
import platform
import queue
import subprocess
import sys
import threading

report={'system':platform.platform(),'ACP':ctypes.windll.kernel32.GetACP(),'OEMCP':ctypes.windll.kernel32.GetOEMCP(),'cases':[]}
for page in (437,936,65001):
    info=subprocess.STARTUPINFO();info.dwFlags=subprocess.STARTF_USESHOWWINDOW;info.wShowWindow=0
    process=subprocess.Popen(['cmd.exe','/D','/Q','/K'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                             creationflags=subprocess.CREATE_NEW_CONSOLE,startupinfo=info)
    lines=queue.Queue()
    def receive():
        while True:
            line=process.stdout.readline()
            if not line:break
            lines.put(line)
    reader=threading.Thread(target=receive,daemon=True);reader.start()
    try:
        process.stdin.write(('chcp %d\r\necho SERIALCTL_PAGE_READY\r\n'%page).encode('ascii'));process.stdin.flush()
        while b'SERIALCTL_PAGE_READY' not in lines.get(timeout=5):pass
        text='SERIALCTL_PIPE_'+('ASCII' if page==437 else '中文')
        payload=('echo '+text+'\r\necho SERIALCTL_PAGE_DONE\r\n').encode('cp%d'%page)
        process.stdin.write(payload);process.stdin.flush()
        output=bytearray()
        while b'SERIALCTL_PAGE_DONE' not in output:output.extend(lines.get(timeout=5))
        case={'page':page,'input_base64':base64.b64encode(payload).decode(),'output_base64':base64.b64encode(output).decode(),
              'decoded':bytes(output).decode('cp%d'%page,errors='replace'),'matches':text.encode('cp%d'%page) in output}
        report['cases'].append(case)
        print(json.dumps(case,ensure_ascii=True))
    finally:
        process.terminate();process.wait(timeout=5);reader.join(timeout=5)
pathlib.Path(sys.argv[1]).write_text(json.dumps(report,ensure_ascii=True,indent=2),encoding='utf-8')
