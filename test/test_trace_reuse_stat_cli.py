"""End-to-end raw-hash/small-exact/large-sketch CLI regression (stdlib only)."""
import json
import math
import pathlib
import subprocess
import sys
import tempfile


def read_metrics(output):
    d={}
    for line in output.splitlines():
        if '=' in line:
            for token in line.split():
                if '=' in token:
                    k,v=token.split('=',1)
                    d[k]=v
    return d


def run(exe, p, *args, succeed=True):
    cmd=[exe,str(p),'--format','qwen-jsonl',*args]
    r=subprocess.run(cmd,check=False,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    if succeed != (r.returncode==0):
        raise AssertionError(f'{cmd} returned {r.returncode}: {r.stderr}\n{r.stdout}')
    return read_metrics(r.stdout),r


def emit(p,rows):
    p.write_text(''.join(json.dumps(row,separators=(',',':'))+'\n' for row in rows),encoding='utf8')


def check_equal(x,y):
    if x != y:
        raise AssertionError(f'expected {y!r}, got {x!r}')


def main():
    if len(sys.argv)!=2:
        raise SystemExit('expected path to traceReuseStat executable')
    exe=sys.argv[1]
    with tempfile.TemporaryDirectory(prefix='prefix-reuse-') as root:
        p=pathlib.Path(root)/'tiny.jsonl'
        # Raw hash IDs are deliberately far from dense (the old vector scheme
        # would try to allocate more than a trillion entries).
        ids=[2**51+7,2**51+11]
        emit(p,[
            {'timestamp':1.0,'session_id':'A','user_id':'u1','hash_ids':ids},
            {'timestamp':3.0,'session_id':'A','user_id':'u1','hash_ids':ids[:1]},
            {'timestamp':5.0,'session_id':'B','user_id':'u2','hash_ids':ids},
            {'timestamp':6.0,'session_id':'B','user_id':'u2','hash_ids':ids[:1]},
        ])
        for mode in ('raw','prefix'):
            data,r=run(exe,p,'--block-id',mode,'--gap-hist')
            check_equal(data['block_accesses'],'6')
            check_equal(data['distinct_blocks'],'2')
            check_equal(data['reuse_same_session'],'2')
            check_equal(data['reuse_cross_session'],'2')
            check_equal(data['sigma_cross'],'0.5000')
            check_equal(data['gap_quantile_method'],'exact')
            check_equal(data['cross_session_gap_p50_s'],'2.0')
            check_equal(data['cross_session_gap_mean_s'],'3.0')
            check_equal(data['reuse_cross_user'],'2')
            check_equal(data['gap_hist_le_4s'],'1')
            check_equal(data['gap_hist_le_8s'],'1')

        p=pathlib.Path(root)/'long.jsonl'
        # Reproduce 6,200 true cross-session gaps with bounded histogram data.
        emit(p,({'timestamp':float(i),'session_id':('A' if i%2 else 'B'),
                 'hash_ids':[ids[0]]} for i in range(6201)))
        data,_=run(exe,p,'--block-id','raw','--gap-hist')
        check_equal(data['reuse_cross_session'],'6200')
        check_equal(data['gap_quantile_method'],'bounded_linear_octave_64')
        lower,upper=map(float,data['cross_gap_p50_interval_s'].split(','))
        if not (lower <= 1.0 <= upper and upper <= 1.016*lower):
            raise AssertionError(f'bad verified quantile enclosure: {lower}, {upper}')

        p=pathlib.Path(root)/'subsecond.jsonl'
        emit(p,[{'timestamp':0.0,'session_id':'A','hash_ids':[ids[0]]},
                {'timestamp':0.1,'session_id':'B','hash_ids':[ids[0]]}])
        data,_=run(exe,p,'--block-id','raw','--gap-hist')
        check_equal(data['gap_hist_le_1s'],'1')

        p=pathlib.Path(root)/'nonmonotone.jsonl'
        emit(p,[{'timestamp':3.0,'session_id':'A','hash_ids':[ids[0]]},
                {'timestamp':2.0,'session_id':'B','hash_ids':[ids[0]]}])
        _,r=run(exe,p,'--block-id','raw',succeed=False)
        if 'nonmonotone' not in r.stderr:
            raise AssertionError('failed to distinguish nonmonotone input')
    print('traceReuseStat CLI: raw sparse IDs, exact small, bounded large, subsecond and invalid time passed')


if __name__=='__main__':
    main()
