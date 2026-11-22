#!/usr/bin/env python3
"""Generate the assignment's three-page report; requires reportlab."""
from pathlib import Path
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, PageBreak, Flowable

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'docs/SUBMISSION_REPORT.pdf'
font_dir=Path('/usr/share/fonts/truetype/dejavu')
for name,filename in [('DVS','DejaVuSans.ttf'),('DVS-Bold','DejaVuSans-Bold.ttf'),('DVS-Oblique','DejaVuSans-Oblique.ttf')]:
    pdfmetrics.registerFont(TTFont(name,str(font_dir/filename)))
pdfmetrics.registerFontFamily('DVS',normal='DVS',bold='DVS-Bold',italic='DVS-Oblique',boldItalic='DVS-Bold')
styles=getSampleStyleSheet()
for style in styles.byName.values():
    if hasattr(style,'fontName'):
        style.fontName='DVS-Bold' if 'Bold' in style.fontName else 'DVS'

styles.add(ParagraphStyle(name='BodySmall',fontName='DVS',fontSize=9.5,leading=12.2,spaceAfter=6))
styles.add(ParagraphStyle(name='SectionSmall',fontName='DVS-Bold',fontSize=12,leading=15,spaceBefore=9,spaceAfter=5,textColor=colors.HexColor('#174b70')))
styles.add(ParagraphStyle(name='CellSmall',fontName='DVS',fontSize=8.1,leading=10.2))
W=A4[0]-84
story=[]
def p(text):story.append(Paragraph(text,styles['BodySmall']))
def h(text):story.append(Paragraph(text,styles['SectionSmall']))
def table(rows,widths):
    data=[[Paragraph(str(cell),styles['CellSmall']) for cell in row] for row in rows]
    item=Table(data,colWidths=widths,hAlign='LEFT',repeatRows=1)
    item.setStyle(TableStyle([('BACKGROUND',(0,0),(-1,0),colors.HexColor('#dce9f2')),
      ('VALIGN',(0,0),(-1,-1),'TOP'),('LEFTPADDING',(0,0),(-1,-1),6),('RIGHTPADDING',(0,0),(-1,-1),6),
      ('TOPPADDING',(0,0),(-1,-1),5),('BOTTOMPADDING',(0,0),(-1,-1),5),
      ('LINEBELOW',(0,0),(-1,0),0.7,colors.HexColor('#7899b0')),
      ('ROWBACKGROUNDS',(0,1),(-1,-1),[colors.white,colors.HexColor('#f3f6f9')])]))
    story.append(item);story.append(Spacer(1,6))
class Diagram(Flowable):
    def __init__(self):super().__init__();self.width=W;self.height=163
    def draw(self):
        c=self.canv
        def box(x,y,w,h,lines):
            c.setFillColor(colors.HexColor('#edf4fa'));c.setStrokeColor(colors.HexColor('#658ba8'))
            c.roundRect(x,y,w,h,5,stroke=1,fill=1)
            c.setFillColor(colors.HexColor('#173e5c'));c.setFont('DVS',7.5)
            for i,line in enumerate(lines):c.drawCentredString(x+w/2,y+h-12-i*11,line)
        def arrow(x,y,xx,yy):
            c.setStrokeColor(colors.HexColor('#547a96'));c.line(x,y,xx,yy)
            import math
            length=math.hypot(xx-x,yy-y)
            ux,uy=(xx-x)/length,(yy-y)/length
            c.line(xx,yy,xx-4*ux+2*uy,yy-4*uy-2*ux)
            c.line(xx,yy,xx-4*ux-2*uy,yy-4*uy+2*ux)
        box(0,116,W,40,['Main thread: TCP accept, semaphore admission, lifecycle and coordinated cleanup'])
        widths=[112,122,118,114];gap=(W-sum(widths))/3
        x=0;xs=[]
        for width,label in zip(widths,[['Session receiver / port','read, validate, learn'],['MAC table','striped locks + checkpoint'],['Priority queues','bounded WRR + aging'],['Sender / port','write + release frame']]):
            box(x,55,width,43,label);xs.append((x,width));x+=width+gap
        for i in range(3):arrow(xs[i][0]+xs[i][1],76,xs[i+1][0],76)
        arrow(55,116,55,98)
        box(0,0,115,36,['Refill worker / port','tokens + wake sender'])
        box(125,0,W-125,36,['Shared workers: admin (policy), aging (expire), signal (sigwait),','logger (drain records and write timestamps)'])
        c.setFont('DVS-Oblique',7.2);c.setFillColor(colors.HexColor('#415c70'))
        c.line(57,36,57,46);c.line(57,46,W-57,46);arrow(W-57,46,W-57,55)

def footer(canvas,doc):
    canvas.setStrokeColor(colors.HexColor('#b6cad8'));canvas.line(42,34,A4[0]-42,34)
    canvas.setFont('DVS',8);canvas.setFillColor(colors.HexColor('#45627a'))
    canvas.drawString(42,22,'Virtual Switch | Operating Systems, Fall 2026')
    canvas.drawRightString(A4[0]-42,22,f'{doc.page} / 3')

story.append(Paragraph('Virtual Switch',styles['Title']))
p('<b>Implementation report - Operating Systems, Fall 2026</b><br/>C11 / Linux / POSIX threads and IPC | 22 November 2026')
h('1. Design and thread diagram')
p('TCP clients occupy logical ports. The switch learns (VLAN, MAC) locations and forwards known unicast or VLAN-scoped floods. The management process uses a UNIX socket and process-shared counters. Optional parent processes launch a pipe-linked pair or restart one crashed switch.')
story.append(Diagram());story.append(Spacer(1,8))
table([['Worker','Responsibility'],['Main','Accept and timed semaphore admission; coordinate joins/cleanup.'],['Per active port','Session runs receiver; one sender drains output; one timer refills tokens.'],['Shared workers','Admin handles commands; aging expires MACs; signal worker uses sigwait; logger drains records.']], [115,W-115])
h('2. Data path and ownership')
p('The fixed 17-byte header carries payload length, source/destination MAC, VLAN and priority; multi-byte fields use network order. Complete-transfer loops handle partial I/O and EINTR. Invalid or truncated frames fail safely; ignored SIGPIPE lets failed writes return errors.')
p('A received SharedFrame starts with one atomic reference. Accepted queue pushes retain one; rejected pushes retain none. The receiver releases its reference after forwarding. Senders and queue cleanup release theirs. Payload is shared inside one process; only serialized bytes cross pipe/process boundaries.')
p('Each port has three FIFO queues of eight frames, a 5:3:1 selection schedule and 100 ms overdue-head aging. Full queues reject/count deliveries instead of waiting for space. Per-port sender isolation prevents slow-client network writes from blocking other senders. A 1000 frames/s token bucket permits burst 10; timer/consumer waits use timed condition variables.')
story.append(PageBreak())
h('3. Locks and the data each protects')
table([['Lock','Protected data / role'],
['admission_lock','Free-slot/permit transitions, enabled state and session reuse/join serialization.'],
['ports_lock','Active port policy and forwarding decisions; slot/socket lifetime coordination.'],
['Port.transmit_lock','In-flight write versus policy purge; multiple affected ports lock ascending.'],
['FrameQueue.mutex','Three rings, counts, head/timestamps, schedule cursor and close predicate.'],
['RateLimit.mutex','Token credit, refill timestamp and close/wait predicates.'],
['MacTable.capacity_lock','Structural mutations, entry count, eviction and checkpoint serialization.'],
['MacBucket.lock[16]','Bucket chains, port/timestamp fields and entry recency.'],
['SharedStats.mutex','Cross-process per-port counters; PTHREAD_PROCESS_SHARED.'],
['SharedMacStore.mutex','Two recovery images and commit operation; robust/process-shared.'],
['MacAging.mutex','Stop flag, reloadable age and aging-worker wake predicate.'],
['AsyncLogger.mutex','Bounded record ring and close predicate; producer trylock.'],
['Benchmark locks / gate','Global mutex, striped mutexes or rwlock protect the benchmark table; gate mutex protects ready/start predicates.']], [135,W-135])
p('Shared-frame references, LRU sequence, logger-drop counters and selected stop/commit fields use atomics. The supervisor is single-threaded; the host uses poll and an async-safe signal flag rather than worker mutexes.')
h('4. Lock order and blocking boundaries')
p('The order is a partial order: admission -> ports for lifecycle; affected transmit locks ascending -> ports for policy; ports where used -> MAC capacity -> bucket locks ascending -> recovery-store mutex. Queue, limiter and statistics operations are leaves and never acquire upper switch locks. Lookup releases its bucket before requesting a capacity/all-bucket checkpoint. MAC move logging runs after table unlock.')
p('Sender token/availability waits precede transmit_lock. Socket writes hold only that port\'s transmit lock, not queue, ports or MAC locks. Session joins release ports_lock first. Logger producers do not wait for a busy logger mutex; its writer performs file I/O outside that mutex. Aging releases its control mutex while sweeping. These rules remove the identified lock-order cycles; tests do not prove every possible schedule.')
h('5. Real bug and correction')
p('Earlier aging code freed expired nodes but did not decrement entry_count. An empty table could still appear full, causing replacement/learning to fail. The regression fills the table, makes timestamps stale, expires all entries, checks entry_count == 0, then learns again. Reproducing the earlier omission in an isolated generated copy fails that assertion. The fixed code decrements under the capacity lock and the production regression passes. The fault-injected copy is not shipped as production source.')
story.append(PageBreak())
h('6. Completed advanced and bonus items')
table([['Items','Implementation'],['Tier 1: 1-5','Wire protocol, bounded queues/senders, atomic shared frames, striped MAC aging/move/LRU/security, timed admission semaphore.'],['Tier 2: 6-9','WRR/aging, token bucket, access VLAN + sole trunk, ingress and queued-egress mirroring with per-sniffer deduplication.'],['Tier 2: 10-13','Live shutdown/up/VLAN/clear, UNIX swctl, shared stats/watch, sigwait worker with INT/HUP/USR1, bounded timestamped logger.'],['Bonus: pipe link','Two anonymous data pipes, child descriptor ownership and cancellable per-port trunk transport. Remote MACs learn behind trunk.'],['Bonus: supervisor','fork/exec/waitpid restart; robust two-slot shared MAC recovery image; source relearning validates stale slot hints.'],['Bonus: benchmark','Matching multithreaded workloads compare global/striped/rwlock; native CSV, metadata, graphs and generated report retained.']], [100,W-100])
h('7. Recovery and management details')
p('Learning/move/eviction, lookup LRU, aging, validated disconnect removal and clear synchronously publish when supervised. A robust process-shared mutex detects a dead writer; copying the inactive slot and atomically selecting it preserves either the old or complete new image. Restore rebuilds local nodes with original aging/LRU metadata. Recovered destinations remain unknown until source learning validates them; an unrelated reconnect cannot inherit known-unicast identity. Administrative policy and TCP sessions are not persisted.')
p('Shutdown disables forwarding, closes queues/limiter waits, interrupts I/O, joins users and releases references before destroying resources. Shared counters/watch and logs are separate from MAC recovery. SIGHUP reloads one MAC-age integer; SIGUSR1 dumps a snapshot. Ingress/egress mirror copies never recurse; egress means queue acceptance, not successful physical delivery. Logger contention may drop counted records.')
h('8. Evidence, benchmark and limits')
p('Before this compliance update, development-machine functional, Memcheck and TSan checks passed ordinary forwarding/isolation, live administration, pipe forwarding and real supervisor crash/reconnect scenarios. Current local checks pass the strict Makefile build, full C unit suite, automatic signal test and send/receive host peer tests; host TSan also passes. Updated full network/tool suites must be rerun on Ubuntu, because the delivery environment blocks the UNIX listener. See docs/VERIFICATION.md for scoped evidence.')
table([['Eight threads: native median Mops/s','Global','Striped','RW lock'],['80% reads','2.448','9.816','1.732'],['50% reads','2.132','8.660','1.733']], [W-168,56,56,56])
p('These measured table-operation results use five million operations/thread and three repeats. The full graph/report is in results/benchmark/. They exclude networking, live lookup LRU mutation and supervised checkpoints; they are not packet-throughput results. Default course builds use -O0; separate native benchmark builds use -O2.')
p('Limits: no raw Ethernet/TAP or spanning tree; supervisor manages one standalone switch, not the pipe pair. Recovery survives worker crashes while the parent lives, not power loss. Global ports/capacity coordination and synchronous full-image checkpoints can serialize work. Successful tests are evidence for their scenarios, not a guarantee about every hidden test or schedule.')
OUT.parent.mkdir(exist_ok=True)
doc=SimpleDocTemplate(str(OUT),pagesize=A4,rightMargin=42,leftMargin=42,topMargin=36,bottomMargin=44,
                     title='Virtual Switch - Implementation Report',author='Virtual Switch Project')
doc.build(story,onFirstPage=footer,onLaterPages=footer)
print(OUT)
