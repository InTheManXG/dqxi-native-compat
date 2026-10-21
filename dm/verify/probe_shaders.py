# DQXI 着色器诊断构建器：只处理真实捕获，复用仓内纯反射函数。
# DQXI diagnostic builder: process captures with repository reflection helpers.
# 用法 / Usage: python probe_shaders.py CAPTURE OUTPUT DOTNET XLATE_DLL SPIRV_CROSS
import ast, hashlib, json, re, struct, subprocess, sys
from pathlib import Path
from probe_geometry import layered_vertex
if len(sys.argv) != 6:
 raise SystemExit('usage: probe_shaders.py CAPTURE OUTPUT DOTNET XLATE_DLL SPIRV_CROSS')
capture,root,dotnet,xlate,cross=map(lambda p:Path(p).resolve(),sys.argv[1:])
repo=Path(__file__).resolve().parents[2]
translated=root/'shader-translated'; translated.mkdir(parents=True,exist_ok=True)
chain=re.search(r'"([a-f0-9]+)"',(repo/'dm/gfx/nvn/dmsh_chain.h').read_text()).group(1)
cache=root/'shader-cache'/('b-'+chain);cache.mkdir(parents=True,exist_ok=True)
# 仅载入纯反射函数，不执行暗黑数据加载。
# Load pure reflection functions without executing D2R data loading.
source=ast.parse((repo/'dm/gfx/tools/dmsh_build.py').read_text())
names={'decode_tex','reflect','raster_facts','drv_reads','unfillable'}
functions=ast.Module(body=[n for n in source.body if isinstance(n,ast.FunctionDef) and n.name in names],type_ignores=[])
ns={'re':re,'TEX_BASE':0x20,'SAMP_BASE':0x568,'IMG_BASE':0x120,'DRV_FILL':{0:[(0x110,0x210),(0x310,0x410),(0x510,0x610)],2:[(0x168,0x568),(0x568,0x968)]}}
exec(compile(functions,'repository-reflection','exec'),ns)
jobs=[]
for ctlfile in sorted(capture.glob('*.control')):
 ctl=ctlfile.read_bytes();code=ctlfile.with_suffix('.code').read_bytes();key=hashlib.sha256(ctl).hexdigest()[:16]
 assert key==ctlfile.stem and len(ctl)==0x880 and struct.unpack_from('<I',ctl)[0]==0x98761234
 c1start,total=struct.unpack_from('<II',ctl,0x700);assert total==len(code) and 0x80<=c1start<=total
 # 计算程序没有图形 SPH，阶段必须从控制段读取，不能把机器码误当头部。
 stageid=struct.unpack_from('<I',ctl,0x714)[0]
 if stageid not in (0,1,5):
  if stageid == 2 and key == '330e9a32eb4f07b5':
   print('LAYERED passthrough geometry',key);continue
  print('UNSUPPORTED stage',key,stageid);continue
 stage={0:'vs',1:'ps',5:'cs'}[stageid]
 if stage != 'cs':
  assert struct.unpack_from('<I',code)[0]==0x12345678
  assert (struct.unpack_from('<I',code,0x30)[0]>>10)&15=={'vs':1,'ps':5}[stage]
 path=translated/(key+'.bin');path.write_bytes(code[0 if stage=='cs' else 0x30:c1start])
 factsfile=ctlfile.with_suffix('.resources.json')
 facts=json.loads(factsfile.read_text()) if factsfile.exists() else {}
 # 整数骨骼索引必须保持整数输入；浮点转换后再按位解释会生成越界索引。
 attrs=facts.get('attr',{})
 assert isinstance(attrs,dict) and all(str(i).isdigit() and 0<=int(i)<32 and t in {'float','uint','sint'} for i,t in attrs.items())
 job={'key':key,'stage':stage,'code':str(path),'c1':code[c1start:total].hex(),'tex':facts.get('tex',{}),'img':facts.get('img',{}),'tex_base':0x20,'attr':attrs,'fout':{},'debug':True,'reduced':True}
 if stage=='cs':
  local=list(struct.unpack_from('<III',ctl,0x748)); shared=struct.unpack_from('<I',ctl,0x754)[0]
  assert all(0<n<=1024 for n in local) and local[0]*local[1]*local[2]<=1024 and shared<=32768
  job.update(local=local,shared=shared)
 jobs.append(job)
jobfile=translated/'jobs.json';jobfile.write_text(json.dumps(jobs,indent=2))
subprocess.run([str(dotnet),str(xlate),str(jobfile),str(translated)],check=True)
rows=[]
for job in jobs:
 key=job['key'];msl=translated/(key+'.metal')
 subprocess.run([str(cross),'--msl','--msl-version','20300','--msl-argument-buffers','--msl-texture-buffer-native','--msl-pad-fragment-output',str(translated/(key+'.spv')),'--output',str(msl)],check=True)
 src=msl.read_text();res=json.loads((translated/(key+'.res.json')).read_text())
 entry=re.search(r'(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(',src);assert entry
 dr=ns['drv_reads'](src);bindings=ns['reflect'](key,job['stage'],src,res)
 unknown=[b for b in bindings if b['kind']=='?' or (b['kind'] in {'tex','samp','samp_tex','cbuf','sbuf','tbuf','img','ubo_ptr'} and b.get('reg') is None)]
 print(key,job['stage'],'queries',res['notes'],'bindings',bindings,'unfillable',ns['unfillable'](dr))
 if unknown or ns['unfillable'](dr) or res['notes']:raise RuntimeError('unresolved shader bindings')
 refl={'key':key+'_b','route':'B','chain':chain,'dm_ctl':key,'stage':job['stage'],'entry':entry[2],'c1':job['c1'],'threadgroup_size':job.get('local'),'bindings':bindings,'drv_reads':dr,'drv_unfillable':[], 'notes':res['notes'],
 'vertex_inputs':[{'location':int(m[2]),'msl_type':m[1]} for m in re.finditer(r'(\w+)\s+\w+\s*\[\[attribute\((\d+)\)\]\]',src)],'raster':ns['raster_facts'](job['stage'],src)}
 (cache/(key+'_b.metallib')).write_text(src);(cache/(key+'_b.json')).write_text(json.dumps(refl,indent=2))
 # 已核对几何程序逐顶点透传、由 location 1.x 指定层；变体只增加等价的 Metal 层输出。
 if key == 'bff1437a6efc2803' and (capture/'330e9a32eb4f07b5.control').exists():
  (cache/(key+'_l.metallib')).write_text(layered_vertex(src))
  (cache/(key+'_l.json')).write_text(json.dumps(dict(refl,key=key+'_l'),indent=2))
 rows.append('capture\t0\t'+{'vs':'顶点','ps':'像素','cs':'计算'}[job['stage']]+'\t'+key)
(root/'shader-map.tsv').write_text('\n'.join(rows)+'\n')
