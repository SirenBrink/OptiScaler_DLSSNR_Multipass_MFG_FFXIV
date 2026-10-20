"""Verify FFXIV native-quality hook guards against a supplied, unmodified executable.
Usage: python tests/verify_ffxiv_native_quality.py path/to/ffxiv_dx11.exe
No game process is opened and no executable bytes are modified.
"""
from pathlib import Path
import re
import struct
import sys
import hashlib

source = (Path(__file__).resolve().parents[1] / 'OptiScaler/upscalers/IFeature.cpp').read_text()
binary = Path(sys.argv[1]).read_bytes()
u16 = lambda offset: struct.unpack_from('<H', binary, offset)[0]
u32 = lambda offset: struct.unpack_from('<I', binary, offset)[0]
assert binary[:2] == b'MZ'
nt = u32(0x3c)
assert binary[nt:nt+4] == b'PE\0\0' and u16(nt+24) == 0x20b
sections = []
for i in range(u16(nt+6)):
    offset = nt + 24 + u16(nt+20) + i*40
    size, rva, raw_size, raw = struct.unpack_from('<IIII', binary, offset+8)
    sections.append((binary[offset:offset+8].rstrip(b'\0'), rva, raw_size, raw))
def read(rva, length):
    for _, start, size, raw in sections:
        if start <= rva and rva+length <= start+size:
            return binary[raw+rva-start:raw+rva-start+length]
    raise AssertionError(f'Unmapped RVA {rva:x}')
def constant(name):
    return int(re.search(r'constexpr uintptr_t '+name+r' = (0x[0-9a-f]+);', source).group(1), 16)
def guard(name):
    match = re.search(r'constexpr unsigned char '+name+r'\[\] = \{([^}]+)\}', source)
    return bytes(int(value,16) for value in re.findall(r'0x[0-9a-f]+', match.group(1)))
text_section = next(section for section in sections if section[0] == b'.text')
text = read(text_section[1], text_section[2])
for name, field in [('expected','kFfxivSettingsInstructionsRva'), ('queryBytes','kFfxivQuerySizeRva'), ('updateBytes','kFfxivRendererUpdateRva'), ('dispatchBytes','kFfxivResizeCallbacksRva')]:
    expected, rva = guard(name), constant(field)
    assert read(rva,len(expected)) == expected, f'{field}: instruction mismatch'
    if name != 'dispatchBytes':
        assert text.count(expected) == 1, f'{field}: guard not unique'
    print(f'PASS: {field} {rva:#x} matches executable')
settings = constant('kFfxivSettingsInstructionsRva')
assert settings+7+struct.unpack('<i',read(settings+3,4))[0] == constant('kFfxivSettingsPointerRva')
query = constant('kFfxivQuerySizeRva')
assert query+28+struct.unpack('<i',read(query+24,4))[0] == constant('kFfxivRenderManagerPointerRva')
pdata = next(section for section in sections if section[0] == b'.pdata')
pdata_bytes = read(pdata[1],pdata[2])
entries = [struct.unpack_from('<III', pdata_bytes,offset) for offset in range(0,pdata[2]-11,12)]
for name in ['kFfxivQuerySizeRva','kFfxivRendererUpdateRva','kFfxivResizeCallbacksRva']:
    assert any(start==constant(name) for start,end,unwind in entries), f'{name}: not a native function boundary'
start = constant('kFfxivRendererUpdateRva')
# MSVC splits this function into chained unwind ranges. Validate all of them,
# rather than truncating at the first runtime-function entry.
chunks = []
for begin,end,unwind in entries:
    if not begin or end <= begin or not unwind:
        continue
    root = begin
    for _ in range(10):
        info = read(unwind,4)
        if not info[0] & 0x20:
            break
        root,_,unwind = struct.unpack('<III', read(unwind+4+((info[2]+1)&~1)*2,12))
    if root == start:
        chunks.append((begin,read(begin,end-begin)))
assert chunks
assert any(code[i]==0xe8 and address+i+5+struct.unpack_from('<i',code,i+1)[0]==constant('kFfxivResizeCallbacksRva') for address,code in chunks for i in range(len(code)-4)), 'Renderer does not call selected resize dispatcher'
device = constant('kFfxivDevicePointerRva')
assert any(code[i:i+3] in (b'\x48\x8b\x05',b'\x4c\x8b\x35') and address+i+7+struct.unpack_from('<i',code,i+3)[0]==device for address,code in chunks for i in range(len(code)-6)), 'Renderer does not reference selected device global'
print('PASS: native function boundaries, RIP-relative globals, renderer device and resize-call relationship')
print('SHA256:',hashlib.sha256(binary).hexdigest())

preview = (Path(__file__).resolve().parents[1] / 'OptiScaler/misc/FfxivCharaViewTrace.h').read_text()
shape_source = re.search(r'constexpr unsigned char shape\[\]\{([^}]+)\}', preview).group(1)
shape = bytes(int(token, 0) for token in shape_source.split(','))
pattern = b''.join(b'.' if 3 <= i <= 6 else re.escape(bytes([value])) for i,value in enumerate(shape))
matches = list(re.finditer(pattern, text, re.S))
assert len(matches) == 1, 'CharaView accessor is missing or ambiguous'
accessor = text_section[1] + matches[0].start()
manager = accessor + 7 + struct.unpack('<i', read(accessor + 3, 4))[0]
# Globals may be in a zero-initialized tail, beyond SizeOfRawData on disk.
assert any(name in (b'.data', b'.rdata') and start <= manager and manager + 8 <= start + max(size, u32(nt + 24 + u16(nt+20) + i*40 + 8)) for i,(name,start,size,raw) in enumerate(sections)), 'CharaView manager is outside data sections'
print(f'PASS: unique CharaView accessor {accessor:#x}, manager global {manager:#x}')

# Jitter is a second native hook family and must be audited after every patch.
jitter = (Path(__file__).resolve().parents[1] / 'OptiScaler/misc/FfxivJitter.cpp').read_text()
jitter_addresses = {}
for name, slot in [('getter','a'),('call','b'),('camera','c'),('rendererLoad','d'),('generator','e'),('generatorCall','f')]:
    match = re.search(r'constexpr unsigned char '+name+r'\[\]\{([^}]+)\}', jitter)
    expected = bytes(int(token,0) for token in match.group(1).split(','))
    address = int(re.search(r'Read\(base\+(0x[0-9a-f]+),'+slot+r',sizeof\('+slot+r'\)\)',jitter).group(1),16)
    assert read(address,len(expected)) == expected, f'Jitter {name}: instruction mismatch'
    jitter_addresses[name] = address
    print(f'PASS: jitter {name} {address:#x} matches executable')
# The eight-byte leaf getter has no unwind record; its exact body and caller are checked above.
for name in ['camera','generator']:
    assert any(begin==jitter_addresses[name] for begin,end,unwind in entries), f'Jitter {name}: not a function boundary'
def relative(address, operand=1, length=5):
    return address+length+struct.unpack('<i',read(address+operand,4))[0]
assert relative(jitter_addresses['call']) == jitter_addresses['getter'], 'NGX packet does not call camera getter'
assert relative(jitter_addresses['generatorCall']) == jitter_addresses['generator'], 'Camera does not call jitter generator'
return_guards = [int(value,16) for value in re.findall(r'_ReturnAddress\(\)\)!=(?:executable)\+(0x[0-9a-f]+)',jitter)]
assert set(return_guards)=={jitter_addresses['call']+5,jitter_addresses['generatorCall']+5}, 'Jitter return-address filters are stale'
settings_read = int(re.search(r'Read\(executable\+(0x[0-9a-f]+),&settings',jitter).group(1),16)
renderer_read = int(re.search(r'Read\(executable\+(0x[0-9a-f]+),&renderer',jitter).group(1),16)
device_read = int(re.search(r'Read\(executable\+(0x[0-9a-f]+),&device',jitter).group(1),16)
assert settings_read == constant('kFfxivSettingsPointerRva')
assert device_read == constant('kFfxivDevicePointerRva')
assert relative(jitter_addresses['rendererLoad'],3,7)==renderer_read==manager
assert relative(jitter_addresses['generatorCall']+5,3,7)==renderer_read
print('PASS: native jitter function boundaries, caller filters and shared renderer/settings/device globals')
