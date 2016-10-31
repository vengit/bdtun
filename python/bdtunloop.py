import bdtun
import os
from ctypes import *

filename = "cucc.disk"

tun = bdtun.Bdtun("cucc")

if bdtun.Bdtun.create(tun.name, 4096, 102400000, 0) < 0:
    print "Could not create tunnel"
    #exit()

tun.open()

try:
    f = open(filename, "w+")

    if os.ftruncate(f.fileno(), 102400000):
        print "Could not ftruncate() file"
        exit()

    req = bdtun.TxReq()

    buf = create_string_buffer(1024*1024) # TODO: how big the largest BIO can be?

    while True:
        # Read the request metadate
        tun.read_request(req)

        # Seek to the correct position in our backing file
        f.seek(req.offset)

        # Check data direction and execute the transaction
        if req.isWrite():
            #tun.get_request_data(req, buf)
            #f.write(buf, req.size * 4096)
            pass
        if req.isRead():
            #buf2 = f.read(req.size * 4096)
            #tun.send_request_data(req, create_string_buffer(buf2))
            pass

        # Signal the completion to the kernel
        tun.complete_request(req)
except KeyboardInterrupt:
    # Close the tunnel and the backing file
    tun.close()
    f.close()
