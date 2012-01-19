import bdtun
import os
from ctypes import *

libc = CDLL("libc.so.6")

filename = "cucc.disk"

tun = bdtun.Bdtun("cucc")

if bdtun.Bdtun.create(tun.name, 4096, 102400000, 0) < 0:
    print "Could not create tunnel"
    exit()

tun.open()

f = open(filename, "w+")

if os.ftruncate(f.fileno(), 102400000):
    print "Could not ftruncate() file"
    exit()

req = bdtun.TxReq()

while True:
    tun.read_request(req)
    f.seek(req.offset)
    if req.flags & 1:
        libc.write(c_int(f.fileno()), req.buf, c_size_t(req.size))
    else:
        libc.read(c_int(f.fileno()), req.buf, c_size_t(req.size))
    tun.complete_request(req)

f.close()
