"""Read guest memory of a running madden_host. usage: peek.py <pid> <g_base hex>  (then python code on stdin using R32/R8/RS)"""
import sys, struct
pid, base = int(sys.argv[1]), int(sys.argv[2], 16)
mem = open(f'/proc/{pid}/mem', 'rb')
def RB(a, n): mem.seek(base + a); return mem.read(n)
def R32(a): return struct.unpack('>I', RB(a, 4))[0]
def R8(a): return RB(a, 1)[0]
def RS(a, n=64): b = RB(a, n); return b.split(b'\0')[0].decode('latin1')
exec(sys.stdin.read())
