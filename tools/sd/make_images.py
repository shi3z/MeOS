"""Small deterministic FAT12/FAT16/FAT32 fixtures with fragmented files and nested directories.
Only creates files in the test's temporary directory; never opens a block device.
"""
import pathlib
import struct
import sys

out = pathlib.Path(sys.argv[1])

def make(bits, mbr=False):
    total, reserved, fatsz, roots = {12: (2000, 1, 6, 64), 16: (10000, 1, 40, 64), 32: (70000, 32, 550, 0)}[bits]
    base = 2048 if mbr else 0
    image = bytearray((base + total) * 512)
    b = memoryview(image)[base * 512:(base + 1) * 512]
    b[:3] = b'\xeb\x3c\x90'
    b[3:11] = b'MEOSRO  '
    struct.pack_into('<HBHBHHBHHHII', b, 11, 512, 1, reserved, 1, roots, total if total < 65536 else 0,
                     0xf8, fatsz if bits != 32 else 0, 63, 255, base, total if total >= 65536 else 0)
    if bits == 32:
        struct.pack_into('<IHHIHH', b, 36, fatsz, 0, 0, 2, 1, 6)
        b[66] = 0x29
        b[82:90] = b'FAT32   '
    else:
        b[38] = 0x29
        b[54:62] = b'FAT12   ' if bits == 12 else b'FAT16   '
    b[510:512] = b'\x55\xaa'
    if mbr:
        image[446 + 4] = 0x0c if bits == 32 else 0x06
        struct.pack_into('<II', image, 446 + 8, base, total)
        image[510:512] = b'\x55\xaa'
    fat = (base + reserved) * 512
    data = base + reserved + fatsz + (roots * 32 + 511) // 512
    def cluster(c):
        return (data + c - 2) * 512
    def setfat(c, v, target=image):
        if bits == 12:
            offset = fat + c + c // 2
            value = struct.unpack_from('<H', target, offset)[0]
            value = (value & 0x000f) | (v << 4) if c & 1 else (value & 0xf000) | v
            struct.pack_into('<H', target, offset, value)
            return
        struct.pack_into('<H' if bits == 16 else '<I', target, fat + c * (bits // 8), v)
    eoc = {12: 0xfff, 16: 0xffff, 32: 0x0fffffff}[bits]
    setfat(0, eoc - 7); setfat(1, eoc)
    first = 2 if bits != 32 else 3
    if bits == 32:
        setfat(2, eoc)
    chain = [first, first + 2, first + 1]
    payload = bytes(i % 251 for i in range(1300))
    for i, c in enumerate(chain):
        setfat(c, chain[i + 1] if i + 1 < len(chain) else eoc)
        chunk = payload[i * 512:(i + 1) * 512]
        image[cluster(c):cluster(c) + len(chunk)] = chunk
    directory, nested = first + 3, first + 4
    setfat(directory, eoc); setfat(nested, eoc)
    image[cluster(nested):cluster(nested) + 7] = b'nested\n'
    def entry(offset, name, attr, c, size):
        image[offset:offset + 11] = name
        image[offset + 11] = attr
        struct.pack_into('<H', image, offset + 20, c >> 16)
        struct.pack_into('<HI', image, offset + 26, c & 0xffff, size)
    root = (base + reserved + fatsz) * 512 if bits != 32 else cluster(2)
    entry(root, b'HELLO   TXT', 0x20, first, 1300)
    entry(root + 32, b'DIR        ', 0x10, directory, 0)
    entry(cluster(directory), b'.          ', 0x10, directory, 0)
    entry(cluster(directory) + 32, b'..         ', 0x10, 0, 0)
    entry(cluster(directory) + 64, b'NEST    TXT', 0x20, nested, 7)
    # Long-name records, including Unicode, spaces, surrogate pairs and maximum names.
    if bits == 32:
        root_chain=[2,100,101,102]
        for i,c in enumerate(root_chain):setfat(c,root_chain[i+1] if i+1<len(root_chain) else eoc)
    def slot(index):
        return cluster(root_chain[index//16])+index%16*32 if bits==32 else root+index*32
    def checksum(alias):
        value=0
        for b in alias:value=(((value&1)<<7)+(value>>1)+b)&255
        return value
    def lfn(index,text,alias):
        raw=text.encode('utf-16-le');units=list(struct.unpack('<'+'H'*(len(raw)//2),raw))
        if len(units)%13:units+=[0]+[0xffff]*((12-len(units)%13)%13)
        chunks=[units[i:i+13] for i in range(0,len(units),13)]
        for sequence in range(len(chunks),0,-1):
            off=slot(index);index+=1;image[off:off+32]=bytes(32)
            image[off]=sequence|(0x40 if sequence==len(chunks) else 0)
            image[off+11]=0x0f;image[off+13]=checksum(alias)
            for position,value in zip([1,3,5,7,9,14,16,18,20,22,24,28,30],chunks[sequence-1]):struct.pack_into('<H',image,off+position,value)
        entry(slot(index),alias,0x20,first,1300)
        return index+1
    index=2
    index=lfn(index,'長い日本語の録音ファイル名 2026年10月24日.mp3',b'RECORD~1MP3')
    index=lfn(index,'a'*251+'.mp3',b'ASCII_~1MP3')
    index=lfn(index,'録'*251+'.mp3',b'JAPANE~1MP3')
    index=lfn(index,'音楽🎵.mp3',b'MUSIC_~1MP3')
    name = f'fat{bits}'  + ('-mbr' if mbr else '')
    (out / (name + '.img')).write_bytes(image)
    # A directory cluster that links to itself and has no terminating entry.
    # The adapter's operation budget must reject the otherwise endless traversal.
    cyc = bytearray(image)
    for i in range(16):
        off = cluster(directory) + i * 32
        cyc[off:off + 11] = f'F{i:07d}TXT'.encode()
        cyc[off + 11] = 0x20
    setfat(directory, directory, cyc)
    (out / (name + '-cycle.img')).write_bytes(cyc)

make(12)
make(16)
make(32)
make(16, True)
make(32, True)
