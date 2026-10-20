from pathlib import Path
import subprocess,tempfile,re
r=Path(__file__).resolve().parents[1]
def function(text, signature):
 start=text.index(signature)
 masked=re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"',lambda m:' '*len(m[0]),text)
 begin=masked.index('{',start);depth=1;end=begin+1
 while depth:
  depth+=(masked[end]=='{')-(masked[end]=='}');end+=1
 return text[start:end]+'\n'
base=(r/'OptiScaler/framegen/IFGFeature.cpp').read_text();dx=(r/'OptiScaler/framegen/IFGFeature_Dx12.cpp').read_text()
common='\n'.join(function(base,x) for x in ['int IFGFeature::GetIndex()', 'UINT64 IFGFeature::StartNewFrame()', 'bool IFGFeature::IsActive()', 'bool IFGFeature::IsPaused()', 'bool IFGFeature::IsLowResMV()', 'void IFGFeature::SetFrameCount(', 'void IFGFeature::UpdateTarget()', 'void IFGFeature::ResetCounters()'])
common+=function(dx,'void IFGFeature_Dx12::NewFrame()')
providers=''
for provider in ['dlssg/DLSSG','xefg/XeFG']:
 name=provider.split('/')[1]+'_Dx12';src=(r/f'OptiScaler/framegen/{provider}_Dx12.cpp').read_text()
 for method in ['CreateContext','Activate','Deactivate','EvaluateState']:
  providers+=function(src,f'void {name}::{method}(')
 # Execute the real early dispatch gate without mocking the full graphics dispatch.
 dispatch=function(src,f'bool {name}::Dispatch()');gate=re.search(r'if \((!?_isActive|!IsActive\(\)) \|\| IsPaused\(\)\)\s*return false;',dispatch)
 assert gate, 'Dispatch active gate missing'
 providers+=f'bool {name}::DispatchGate(){{{gate[0]} return true;}}\n'
sl_input=function((r/'OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp').read_text(),'void Sl_Inputs_Dx12::CheckForFrame(')
providers+=sl_input
menu=(r/'OptiScaler/menu/menu_common.cpp').read_text()
menu_block=function(menu,'if (!correctMVs || state.realExclusiveFullscreen)')
providers+='void ApplyXeFGMenuWarnings(bool correctMVs,bool exclusive){auto* config=Config::Instance();auto& state=State::Instance();state.realExclusiveFullscreen=exclusive;'+menu_block+'}\n'
with tempfile.TemporaryDirectory(prefix='fg-startup-',dir=r/'tests') as temp:
 b=Path(temp);(b/'fg-startup-production.inl').write_text(common+providers)
 exe=b/'test.exe';subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',f'/I{b}',str(r/'tests/fg_startup.cpp'),f'/Fo{b}/',f'/Fe{exe}'],check=True);subprocess.run([str(exe)],check=True)
 # Negative control: old absolute warm-up target never expires after a frame ID restart.
 old=common+providers;start=old.index('void IFGFeature::SetFrameCount(');body=function(old,'void IFGFeature::SetFrameCount(');old=old.replace(body,'void IFGFeature::SetFrameCount(UINT64 frameId){_frameCount=frameId;}\n',1)
 (b/'fg-startup-production.inl').write_text(old)
 bad=b/'old.exe';subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',f'/I{b}',str(r/'tests/fg_startup.cpp'),f'/Fo{b}/old.obj',f'/Fe{bad}'],check=True)
 result=subprocess.run([str(bad),'rewind-only']);assert result.returncode==1, 'Frame-rewind negative control was not detected'
 print('PASS: negative control reproduces FG remaining paused after input frame IDs restart')

 # The same policy must not re-arm a warm-up while the user has FG disabled.
 old_recovery=function(dx,'void IFGFeature_Dx12::NewFrame()')
 fixed_block=old_recovery[old_recovery.index('    if (_waitingNewFrameData)'):old_recovery.index('    auto fIndex = GetIndex();')]
 broken=old_recovery.replace(fixed_block,'    if (_waitingNewFrameData){UpdateTarget();Activate();_waitingNewFrameData=false;}\n')
 (b/'fg-startup-production.inl').write_text((common+providers).replace(old_recovery,broken,1))
 subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',f'/I{b}',str(r/'tests/fg_startup.cpp'),f'/Fo{b}/recovery.obj',f'/Fe{b}/recovery.exe'],check=True)
 assert subprocess.run([str(b/'recovery.exe'),'disabled-recovery-only']).returncode==1
 print('PASS: negative control catches disabled FG re-arming a pending warm-up')
 (b/'fg-startup-production.inl').write_text((common+providers).replace('if (!_isActive || IsPaused())','if (!IsActive() || IsPaused())'))
 subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',f'/I{b}',str(r/'tests/fg_startup.cpp'),f'/Fo{b}/dispatch.obj',f'/Fe{b}/dispatch.exe'],check=True)
 assert subprocess.run([str(b/'dispatch.exe'),'pending-dispatch-only']).returncode==1
 print('PASS: negative control catches dispatching while only pending recovery is active')

 # Opening the menu used to erase the FG enable preference during warnings.
 bad_menu=menu_block.replace('config->FGXeFGDebugView.reset();','config->FGEnabled.reset();config->FGXeFGDebugView.reset();')
 (b/'fg-startup-production.inl').write_text((common+providers).replace(menu_block,bad_menu))
 subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',f'/I{b}',str(r/'tests/fg_startup.cpp'),f'/Fo{b}/menu.obj',f'/Fe{b}/menu.exe'],check=True)
 assert subprocess.run([str(b/'menu.exe'),'ui-preference-only']).returncode==1
 print('PASS: negative control catches the menu erasing the FG enable preference')

 (b/'fg-startup-production.inl').write_text((common+providers).replace('const bool restarted = _isFrameFinished','const bool restarted = false && _isFrameFinished'))
 subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',f'/I{b}',str(r/'tests/fg_startup.cpp'),f'/Fo{b}/input.obj',f'/Fe{b}/input.exe'],check=True)
 assert subprocess.run([str(b/'input.exe'),'input-epoch-only']).returncode==1
 print('PASS: negative control catches the Streamline input latch waiting for pre-restart frame IDs')
