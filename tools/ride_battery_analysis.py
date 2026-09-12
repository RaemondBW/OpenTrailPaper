#!/usr/bin/env python3
"""Analyze the captured Sep 11 ride. Excludes replayed historical NVS snapshots.

Fuel-gauge capacity differences integrate consumption better than sparse current
snapshots. Projection is nominal full-capacity runtime, not measured endurance.
"""
import json
import re
from pathlib import Path
from statistics import mean
ROOT=Path(__file__).resolve().parents[1]
source=ROOT/'investigations/results/ride-crashes-2026-09-11/daily.log'
lines=source.read_text().splitlines()
def seconds(t):
    h,m,s=map(int,t.split(':'));return h*3600+m*60+s
battery=[];windows=[];gps=[];phone=[];sensor=[];boots=[];boot=0;snapshot=False;last_rx=None
for index,line in enumerate(lines,1):
    if line.startswith('[diag] prior failure snapshot'): snapshot=True
    if 'sd prior RTC:' in line: snapshot=False
    if snapshot:continue
    match=re.match(r'\[(\d\d:\d\d:\d\d)\] (.*)',line)
    if not match:continue
    stamp,body=match.groups();t=seconds(stamp)
    if 'boot firmware' in body:boot+=1;boots.append(dict(line=index,time=stamp,event=body));last_rx=None
    if t>seconds('09:45:32'):continue  # stop at saved ride, before post-ride/USB testing
    m=re.match(r'battery: (\d+)% (\d+)mV (-?\d+)mA (\d+)/(\d+)mAh (\w+)',body)
    if m:
        soc,mv,ma,rc,fc=map(int,m.groups()[:5]);battery.append(dict(line=index,boot=boot,time=stamp,seconds=t,soc=soc,mv=mv,ma=ma,remaining_mAh=rc,full_mAh=fc))
    m=re.match(r'pm window: (\d+)ms calls=(\d+) rejected=(\d+) sleep_call_ms=(\d+) holders=(\S+) sd=(\d) host=(\d)',body)
    if m:windows.append(dict(zip(('ms','calls','rejected','sleep_ms','holders','sd','host'),[int(x) if i!=4 else x for i,x in enumerate(m.groups())]),boot=boot,time=stamp))
    m=re.match(r'gps sentence window: gga=(\d+) rmc=(\d+) bad=(\d+) truncated=(\d+) missing_gga=(\d+) missing_rmc=(\d+)',body)
    if m:last_rx=dict(zip(('gga','rmc','bad','truncated','missing_gga','missing_rmc'),map(int,m.groups())),boot=boot,line=index,time=stamp)
    m=re.match(r'gps RX window: (\d+)ms bytes=(\d+) good=(\d+) bad=(\d+) sleep_calls=(\d+)',body)
    if m and last_rx:
        last_rx.update(dict(zip(('ms','bytes','good','parser_bad','sleep_calls'),map(int,m.groups()))));gps.append(last_rx);last_rx=None
    if 'phone disconnected' in body:phone.append(dict(time=stamp,line=index,event=body))
    if 'sensor disconnected' in body:sensor.append(dict(time=stamp,line=index,event=body))

def interval(label,start,end):
    a=next(x for x in battery if x['time']==start);b=next(x for x in battery if x['time']==end)
    assert a['boot']==b['boot'] and a['ma']<0 and b['ma']<0
    elapsed=b['seconds']-a['seconds'];used=a['remaining_mAh']-b['remaining_mAh'];current=used*3600/elapsed
    selected=[x['ma'] for x in battery if x['boot']==a['boot'] and a['seconds']<=x['seconds']<=b['seconds']]
    # Both endpoints have integer-mAh resolution; ±1 mAh in their difference.
    return dict(label=label,start=a,end=b,elapsed_seconds=elapsed,used_mAh=used,mean_from_capacity_mA=current,
                nominal_runtime_hours=a['full_mAh']/current,
                quantization_only_runtime_range_hours=[a['full_mAh']/((used+1)*3600/elapsed),a['full_mAh']/((used-1)*3600/elapsed)],
                snapshot_count=len(selected),snapshot_mean_mA=-mean(selected))
sleep_gps=[x for x in gps if x['sleep_calls']>0]
report=dict(source=str(source.relative_to(ROOT)),cutoff='09:45:32 ride saved',battery_readings=battery,
    intervals=[interval('Phone and Assioma connected; includes hunt/display/stop activity','08:36:22','08:46:17'),
               interval('Neither phone nor sensor connected; stopped','08:53:43','08:58:40'),
               interval('Assioma connected, phone absent, after second crash','09:37:23','09:44:49')],
    pm=dict(windows=len(windows),observed_seconds=sum(x['ms'] for x in windows)/1000,
            successful_calls=sum(x['calls'] for x in windows),rejected=sum(x['rejected'] for x in windows),
            seconds_in_sleep_calls=sum(x['sleep_ms'] for x in windows)/1000,
            sleep_call_fraction=sum(x['sleep_ms'] for x in windows)/sum(x['ms'] for x in windows),
            sd_unmounted_windows=sum(x['sd']==0 for x in windows)),
    gps_during_sleep=dict(windows=len(sleep_gps),**{k:sum(x[k] for x in sleep_gps) for k in ('gga','rmc','bad','truncated','missing_gga','missing_rmc','bytes','good','parser_bad')}),
    phone_disconnects=phone,sensor_disconnects=sensor,boots=boots,
    limitations=['Two watchdog resets; missing final buffered intervals before resets.',
      'No same-route awake baseline or complete battery discharge.',
      'Full outing includes stops and deep sleep, so its start/end consumption is not a riding runtime estimate.',
      'Integer gauge readings and calibration/temperature affect estimates; intervals cover only quantization, not all uncertainty.',
      'sleep_call_fraction includes entry/exit overhead, not precise low-power residency.',
      'Phone-disconnect causality is not established; timeouts also occurred after USB held CPU sleep off.'])
out=ROOT/'investigations/results/ride-crashes-2026-09-11/battery-analysis.json'
out.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k not in ('battery_readings','phone_disconnects','boots')},indent=2))
print('Phone disconnects before ride save:',len(phone))
