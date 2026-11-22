"""Copy SkyrimSE.exe's image out of the running process into a PE file whose sections lie at
their RVAs, so the offline tools (retools.Image) read the decrypted code."""
import ctypes, ctypes.wintypes as w, struct, sys
k32 = ctypes.WinDLL("kernel32", use_last_error=True); psapi = ctypes.WinDLL("psapi")
pid, out = int(sys.argv[1]), sys.argv[2]
h = k32.OpenProcess(0x0410, False, pid)
mods = (ctypes.c_void_p * 1024)(); need = w.DWORD()
psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
base = mods[0]
def read(addr, n):
    buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got))
    return buf.raw[: got.value] if ok else None
hdr = bytearray(read(base, 0x1000))
pe = struct.unpack_from("<I", hdr, 0x3C)[0]
size = struct.unpack_from("<I", hdr, pe + 24 + 56)[0]
img = bytearray(size)
for page in range(0, size, 0x1000):
    data = read(base + page, 0x1000)
    if data: img[page : page + len(data)] = data
count = struct.unpack_from("<H", img, pe + 6)[0]; opt = struct.unpack_from("<H", img, pe + 20)[0]
for i in range(count):
    s = pe + 24 + opt + i * 40
    vsize, rva = struct.unpack_from("<II", img, s + 8)
    struct.pack_into("<II", img, s + 16, vsize, rva)  # SizeOfRawData, PointerToRawData
open(out, "wb").write(img)
print(hex(base), hex(size))
