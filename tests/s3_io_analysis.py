"""Independent range-overlap and trace-boundary oracles for the profiling runner."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'benchmarks'))
from s3_io_load import analyze, union_bytes

assert union_bytes([(4,12),(0,8),(4090,4100)]) == 22
assert union_bytes([]) == 0
assert union_bytes([(0,10),(2,3),(10,12)]) == 12


def line(op, source, size, offset=0, count=0):
    return (f'S3 io op={op} source={source} object_bytes={size} offset={offset} '
            f'requested={count} received={count} elapsed_us=100 accepted=1\n')

log=(line('HEAD',1,10000)+line('HEAD',2,16)+line('GET',1,10000,0,8)
     +line('GET',1,10000,4,8)+line('GET',1,10000,4090,10)+line('GET',2,16,0,16))
result=analyze(log,{10000:'image',16:'mask'},1)
image=result['groups'][1]
assert image['count']==3 and image['received_bytes']==26 and image['at_most_1k']==3
assert abs(image['elapsed_ms']-0.3)<1e-9 and image['median_requested_bytes']==8
assert result['within_source'][0]['unique_read_bytes']==22
assert result['within_source'][0]['repeated_read_bytes']==4
assert result['within_source'][0]['hypothetical_4k_pages']==2
assert result['within_source'][0]['hypothetical_4k_bytes']==8192
assert result['within_source'][1]['hypothetical_4k_bytes']==16
for bad in (log.replace('accepted=1','accepted=0',1),log.replace('object_bytes=16','object_bytes=17'),log.split('\n',1)[1]):
    try:
        analyze(bad,{10000:'image',16:'mask'},1)
    except RuntimeError:
        pass
    else:
        raise AssertionError('Invalid/partial trace accepted')
print('S3 trace grouping, overlaps, page model and boundary checks passed')
