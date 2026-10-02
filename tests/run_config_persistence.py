"""Run in a VS developer shell; extract actual HDR/latency INI code for disk round trips."""
import pathlib
import re
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
header = (repo / 'OptiScaler/Config.h').read_text(encoding='utf-8')
source = (repo / 'OptiScaler/Config.cpp').read_text(encoding='utf-8')

def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

fields = re.findall(r'^\s*(CustomOptional<[^\n]+>\s+(?:FfxivHDR\w*|LowLatencyInput|LowLatencyOutput|VulkanUseCopyForInputs|VulkanUseCopyForOutput|AnisotropyModifyComp|AnisotropyModifyMinMax|DlssNrVitFirstPassOnly)\s*\{[^\n]+?;)', header, re.M)
assert len(fields) == 16
load_start = source.index('            if (auto v = readUInt("LowLatency", "Input")')
load = source[load_start:source.index('            ForceHDR.set_from_config', load_start)]
for field in ['AnisotropyModifyComp', 'AnisotropyModifyMinMax', 'DlssNrVitFirstPassOnly']:
    start = source.index(field + '.set_from_config')
    load += source[start:source.index(';', start) + 1] + '\n'

save_start = source.index('    ini.SetValue("LowLatency", "Input",')
save = source[save_start:source.index('    ini.SetValue("DlssNr", "VitAllowUnverified",', save_start)]
for key in ['ModifyComparison', 'ModifyMinMax']:
    start = source.index('ini.SetValue("Anisotropy", "' + key + '",')
    save += source[start:source.index(';', start) + 1] + '\n'

start = source.index('ini.SetValue("DlssNr", "VitFirstPassOnly",')
save += source[start:source.index(';', start) + 1] + '\n'

tail_start = source.index('    auto pathWStr = absoluteFileName.wstring();', source.index('bool Config::SaveIni()'))
save += source[tail_start:source.index('\n}', tail_start)]
optional = header[header.index('enum HasDefaultValue'):header.index('constexpr inline int UnboundKey')]
enums = header[header.index('enum class LowLatencyInput'):header.index('\n};', header.index('enum class LowLatencyMode')) + 3]
helpers = '\n'.join(function(x) for x in ['static inline bool isFloat(', 'std::string GetBoolValue(', 'template <typename T> std::string GetIntValue(', 'std::string GetFloatValue('])
readers = '\n'.join(function(x) for x in ['std::optional<std::string> Config::readString(', 'std::optional<float> Config::readFloat(', 'std::optional<int> Config::readInt(', 'std::optional<uint32_t> Config::readUInt(', 'std::optional<bool> Config::readBool('])
code = r'''
#include <optional>
#include <string>
#include <filesystem>
#include <sstream>
#include <format>
#include <algorithm>
#include <vector>
#include <cassert>
#include <iostream>
#include <SimpleIni.h>
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
static CSimpleIniA ini;
constexpr int UnboundKey = -1;
''' + optional + enums + helpers + '\nclass Config { public:\n' + '\n'.join(fields) + r'''
std::filesystem::path absoluteFileName;
std::vector<std::string> _log;
Config* Instance() {return this;}
std::optional<std::string> readString(std::string, std::string, bool = false);
std::optional<float> readFloat(std::string, std::string);
std::optional<int> readInt(std::string, std::string);
std::optional<uint32_t> readUInt(std::string, std::string);
std::optional<bool> readBool(std::string, std::string);
bool Save() {
''' + save + '\n}\nbool Load() { ini.Reset(); if(ini.LoadFile(absoluteFileName.c_str())<0)return false;\n' + load + '\nreturn true;\n}\n};\n' + readers + r'''
int main(int argc,char**argv) {
 assert(argc==2);
 const std::filesystem::path path=argv[1];
 Config c; c.absoluteFileName=path;
 c.DlssNrVitFirstPassOnly=false;
 c.FfxivHDR=true;c.FfxivHDRPeak=1450.0f;c.FfxivHDRPaper=185.0f;
 c.FfxivHDRExpansion=0.73f;c.FfxivHDRContrast=1.23f;
 c.FfxivHDRSaturation=1.17f;c.FfxivHDRVibrance=-0.32f;
 c.FfxivHDRScreenshotKey=0x79;c.FfxivHDRScreenshotFormat=1;
 c.LowLatencyInput=LowLatencyInput::Reflex;c.LowLatencyOutput=LowLatencyMode::XeLL;
 c.VulkanUseCopyForInputs=true;c.VulkanUseCopyForOutput=true;
 c.AnisotropyModifyComp=false;c.AnisotropyModifyMinMax=false;
 assert(c.Save()); Config loaded;loaded.absoluteFileName=path;assert(loaded.Load());
''' + ''.join(f'assert(c.{name}.value_or_default()==loaded.{name}.value_or_default());\n' for name in re.findall(r'>\s+(\w+)\s*\{', '\n'.join(fields))) + r'''
 // Reset every HDR field independently, exactly as the menu does. Others must survive.
 Config defaults;
''' + ''.join(f'''{{auto before=loaded;loaded.{name}=std::nullopt;assert(loaded.Save());
 Config fresh;fresh.absoluteFileName=path;assert(fresh.Load());
 assert(fresh.{name}.value_or_default()==defaults.{name}.value_or_default());
''' + ''.join(f'assert(fresh.{other}.value_or_default()==before.{other}.value_or_default());\n' for other in re.findall(r'>\s+(\w+)\s*\{', '\n'.join(fields)) if other != name) + 'loaded=fresh;}\n' for name in re.findall(r'>\s+(FfxivHDR\w*)\s*\{', '\n'.join(fields))) + r'''
 // Restoring neutral values must also survive a second save, not retain old INI values.
 assert(loaded.Save());Config twice;twice.absoluteFileName=path;assert(twice.Load());
 assert(twice.FfxivHDRSaturation.value_or_default()==1.0f);
 assert(twice.FfxivHDRVibrance.value_or_default()==0.0f);
 assert(twice.FfxivHDRScreenshotKey.value_or_default()==UnboundKey);
 loaded.absoluteFileName=path/"missing"/"OptiScaler.ini";assert(!loaded.Save());
 std::cout<<"PASS: 16 settings round-trip; all nine independent HDR resets persist; failed writes report failure\n";
}
'''
simpleini = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else repo / 'external/simpleini'
with tempfile.TemporaryDirectory(prefix='optiscaler-persistence-') as temp:
    build = pathlib.Path(temp)
    (build / 'test.cpp').write_text(code, encoding='utf-8')
    subprocess.run(['cl', '/nologo', '/std:c++latest', '/EHsc', '/UNDEBUG', f'/I{simpleini}', str(build / 'test.cpp'), f'/Fo{build}/', f'/Fe{build}/test.exe'], check=True)
    subprocess.run([str(build / 'test.exe'), str(build / 'OptiScaler.ini')], check=True)
