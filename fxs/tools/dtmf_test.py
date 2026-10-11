#!/usr/bin/env python3
# Offline test suite for the DTMF detector (fxs/slicctl/dtmf.c): synthetic
# keypad audio under dial-tone echo, twist, frequency error, noise, short
# tones, plus non-digit audio that must yield nothing.
# Usage: python3 dtmf_test.py PATH/TO/host-built-slicctl   (exit status = failures)
import math, random, struct, subprocess, sys
R={'1':(697,1209),'2':(697,1336),'3':(697,1477),'A':(697,1633),'4':(770,1209),'5':(770,1336),'6':(770,1477),'B':(770,1633),
   '7':(852,1209),'8':(852,1336),'9':(852,1477),'C':(852,1633),'*':(941,1209),'0':(941,1336),'#':(941,1477),'D':(941,1633)}
def db(x): return 32767*10**(x/20)
def gen(digits, lvl=-10, on=0.1, off=0.1, twist=0, fdev=0.0, tone450=None, noise=None, seed=1):
    rnd=random.Random(seed); out=[]; t=0
    def add(n, f=None):
        nonlocal t
        for i in range(n):
            x=0.0
            if f:
                x+=db(lvl)*math.sin(2*math.pi*f[0]*(1+fdev)*t/8000)+db(lvl+twist)*math.sin(2*math.pi*f[1]*(1+fdev)*t/8000)
            if tone450 is not None: x+=db(tone450)*math.sin(2*math.pi*450*t/8000)
            if noise is not None: x+=rnd.gauss(0,db(noise))
            out.append(max(-32768,min(32767,int(x)))); t+=1
    add(int(0.3*8000))
    for d in digits:
        add(int(on*8000),R[d]); add(int(off*8000))
    add(int(0.3*8000))
    return struct.pack('<%dh'%len(out),*out)
def run(audio):
    r=subprocess.run([sys.argv[1],'dtmfdet'],input=audio,capture_output=True)
    return r.stdout.decode().strip()
D='0771234567*#ABCD'
cases=[('clean -10 dBFS',gen(D),D),
 ('dial-tone echo -20 dBFS',gen(D,tone450=-20),D),
 ('dial-tone echo -12 dBFS (as loud as keys)',gen(D,tone450=-12),D),
 ('quiet keys -35 dBFS',gen(D,lvl=-35),D),
 ('too quiet -55 dBFS',gen(D,lvl=-55),''),
 ('twist +6 dB',gen(D,twist=6),D),
 ('twist -6 dB',gen(D,twist=-6),D),
 ('freq +1.5%',gen(D,fdev=0.015),D),
 ('freq -1.5%',gen(D,fdev=-0.015),D),
 ('white noise -30 dBFS',gen(D,noise=-30),D),
 ('repeat 5 5 5, 60 ms gaps',gen('555',off=0.06),'555'),
 ('held key 1 s = one digit',gen('8',on=1.0),'8'),
 ('60 ms tones',gen(D,on=0.06,off=0.06),D),
 ('40 ms tones (ITU minimum)',gen(D,on=0.04,off=0.04),D),
 ('negative: 450 Hz only',gen('',tone450=-6),''),
 ('negative: noise -10 dBFS',gen('',noise=-10),''),
]
bad=0
for name,a,exp in cases:
    got=run(a); ok=got==exp; bad+=not ok
    print(f"{'PASS' if ok else 'FAIL'}  {name:42s} expected {exp!r:20s} got {got!r}")
# speech-like: sum of random harmonics, should give no digits
rnd=random.Random(7); out=[]
for blk in range(200):
    f0=rnd.uniform(100,250); amps=[rnd.uniform(0,1)/(k) for k in range(1,15)]
    for i in range(205):
        t=blk*205+i; out.append(int(4000*sum(a*math.sin(2*math.pi*f0*k*t/8000) for k,a in enumerate(amps,1))))
got=run(struct.pack('<%dh'%len(out),*[max(-32768,min(32767,x)) for x in out]))
print(f"{'PASS' if got=='' else 'FAIL'}  {'negative: 5 s harmonic speech-like':42s} expected ''  got {got!r}"); bad+=got!=''
sys.exit(bad)
