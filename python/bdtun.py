from ctypes import *
import os

class Info(Structure):
    _fields_ = [
        ("bd_size", c_uint64),
        ("bd_block_size", c_uint64),
        ("bd_major", c_int),
        ("bd_minor", c_int),
        ("ch_major", c_int),
        ("ch_minor", c_int),
        ("capabilities", c_int)
    ]

class _ctrlreq_create(Structure):
    _fields_ = [
        ("blocksize", c_uint64),
        ("size", c_uint64),
        ("capabilities", c_int),
        ("name", c_char * 32)
    ]

class _ctrlreq_info(Structure):
    _fields_ = [
        ("name", c_char * 32)
    ]

class _ctrlreq_list(Structure):
    _fields_ = [
        ("offset", c_size_t),
        ("maxdevices", c_size_t)
    ]

class _ctrlreq_resize(Structure):
    _fields_ = [
        ("name", c_char * 32)
    ]

class _ctrlreq_remove(Structure):
    _fields_ = [
        ("name", c_char * 32)
    ]

class CtrlReq(Union):
    _fields_ = [
        ("create", _ctrlreq_create),
        ("info", _ctrlreq_info),
        ("list", _ctrlreq_list),
        ("resize", _ctrlreq_resize),
        ("remove", _ctrlreq_remove)
    ]

class TxReq(Structure):
    _fields_ = [
        ("id", c_void_p),
        ("flags", c_ulong),
        ("offset", c_ulong),
        ("size", c_ulong)
    ]

    def isRead(self):
        return self.flags & 1 == 0;

    def isWrite(self):
        return self.flags & 1 == 1;


class Bdtun:
    def __init__(self, name):
        self.name = name
        self.dev = None
        lib = CDLL("libbdtun.so")

        f = lib.bdtun_complete_request
        f.argtypes = [c_int, POINTER(TxReq)]
        f.restype = c_int
        Bdtun._f_bdtun_complete_request = f

        f = lib.bdtun_create
        f.argtypes = [c_int, c_char_p, c_uint64, c_uint64, c_int]
        f.restype = c_int
        Bdtun._f_bdtun_create = f

        f = lib.bdtun_fail_request
        f.argtypes = [c_int, POINTER(TxReq)]
        f.restype = c_int
        Bdtun._f_bdtun_fail_request = f

        f = lib.bdtun_get_request_data
        f.argtypes = [c_int, POINTER(TxReq), c_void_p]
        f.restype = c_int
        Bdtun._f_bdtun_get_request_data = f

        f = lib.bdtun_info
        f.argtypes = [c_int, c_char_p, POINTER(Info)]
        f.restype = c_int
        Bdtun._f_bdtun_info = f

        f = lib.bdtun_list
        f.argtypes = [c_int, c_size_t, c_size_t, POINTER(POINTER(POINTER(c_char)))]
        f.restype = c_int
        Bdtun._f_bdtun_list = f
        
        f = lib.bdtun_read_request
        f.argtypes = [c_int, POINTER(TxReq)]
        f.restype = c_int
        Bdtun._f_bdtun_read_request = f

        f = lib.bdtun_remove
        f.argtypes = [c_int, c_char_p]
        f.restype = c_int
        Bdtun._f_bdtun_remove = f

        f = lib.bdtun_resize
        f.argtypes = [c_int, c_char_p, c_uint64]
        f.restype = c_int
        Bdtun._f_bdtun_resize = f

        f = lib.bdtun_send_request_data
        f.argtypes = [c_int, POINTER(TxReq), c_void_p]
        f.restype = c_int
        Bdtun._f_bdtun_send_request_data = f

        f = lib.bdtun_set_request
        f.argtypes = [c_int, POINTER(TxReq)]
        f.restype = c_int
        Bdtun._f_bdtun_set_request = f

    def open(self):
        self.dev = open("/dev/%s_tun" % self.name, 'w+')
        self.devno = self.dev.fileno()

    def close(self):
        self.dev.close()

    def set_request(self, txreq):
        return Bdtun._f_bdtun_set_request(c_int(self.devno), byref(txreq))

    def read_request(self, txreq):
        return Bdtun._f_bdtun_read_request(c_int(self.devno), byref(txreq))

    def complete_request(self, txreq):
        return Bdtun._f_bdtun_complete_request(c_int(self.devno), byref(txreq))

    def get_request_data(self, txreq, buf):
        return Bdtun._f_bdtun_get_request_data(c_int(self.devno), byref(txreq), byref(buf))

    def send_request_data(self, txreq, buf):
        return Bdtun._f_bdtun_send_request_data(c_int(self.devno), byref(txreq), byref(buf))

    @staticmethod
    def _getctrldev():
        return open("/dev/bdtun", "w+")

    @staticmethod
    def create(name, blocksize, size, capabilities):
        f = Bdtun._getctrldev()
        ret = Bdtun._f_bdtun_create(
                c_int(f.fileno()), c_char_p(name), c_uint64(blocksize),
            c_uint64(size), c_int(capabilities))
        f.close()
        return ret

    @staticmethod
    def resize(name, size):
        f = Bdtun._getctrldev()
        self.size = size
        ret = Bdtun._f_bdtun_resize(c_int(f.fileno()), c_char_p(name), c_uint64(size))
        f.close()
        return ret

    @staticmethod
    def remove(name):
        f = Bdtun._getctrldev()
        ret = Bdtun._f_bdtun_remove(c_int(f.fileno()), c_char_p(name))
        f.close()
        return ret

    @staticmethod
    def info(name, info):
        f = Bdtun._getctrldev()
        ret = Bdtun._f_bdtun_info(c_int(f.fileno()), c_char_p(name), ref(info))
        f.close()
        return ret

    @staticmethod
    def list(buf):
        f = Bdtun._getctrldev()
        ret = self._f_bdtun_list(c_int(f.fileno()), ref(buf))
        f.close()
        return ret
