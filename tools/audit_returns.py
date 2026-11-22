from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
# Keep offsets, but mask comments and strings when locating statement expressions.
def masked(s):
 return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',lambda m:''.join('\n' if c=='\n' else ' ' for c in m.group()),s,flags=re.S)
def args(text):
 out=[];start=0;depth=0
 for i,c in enumerate(masked(text)):
  if c in '([{':depth+=1
  if c in ')]}':depth-=1
  if c==',' and depth==0:out.append(text[start:i].strip());start=i+1
 out.append(text[start:].strip());return out
posix={'close','unlink','shm_unlink','munmap','sem_post','sem_destroy','sigemptyset','sigaddset','fflush','printf','fprintf','puts','putchar','fputs','fclose','kill','sigaction','ftruncate','pipe','clock_gettime','pthread_kill'}
zero={'mac_store_close','pipe_link_destroy','mac_table_destroy','control_stop','async_logger_stop'}
files=list((root/'src').glob('*.c'))+list((root/'bonus').glob('*/*.c'))+list((root/'tests').glob('*.c'))
found=0
for p in files:
 s=p.read_text();mask=masked(s);edits=[]
 for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\(',mask):
  name=m.group(1)
  is_pthread=name.startswith('pthread_') and name not in {'pthread_self','pthread_equal'}
  if name not in posix|zero|{'snprintf'} and not is_pthread:continue
  start=m.start();prefix=mask[:start].rstrip();prev=prefix[-1:]
  if prev not in ('',';','{','}',')',':') and not re.search(r'\b(?:else|do)$',prefix):continue
  depth=1;j=m.end()
  while j<len(mask) and depth:
   depth+=(mask[j]=='(')-(mask[j]==')');j+=1
  if depth or not re.match(r'\s*;',mask[j:]):continue
  call=s[start:j]
  if name=='snprintf':
   parameters=args(s[m.end():j-1]);new=f'vsw_format({call}, {parameters[1]})'
  elif name=='kill':new='vsw_kill'+s[start+len(name):j]
  elif is_pthread:new=f'vsw_pthread({call}, "{name}")'
  elif name in zero:new=f'vsw_zero({call}, "{name}")'
  else:new=f'vsw_posix({call}, "{name}")'
  edits.append((start,j,new))
 if edits:
  for a,b,new in edits:
   print(f"UNCHECKED {p.relative_to(root)}:{s.count(chr(10),0,a)+1}: {s[a:b]}")
  found += len(edits)
if found:
 raise SystemExit(1)
print("PASS: no unchecked standalone calls in the audited function set")
print("Note: this lexical guard supplements manual review; it is not a complete C semantic proof.")
