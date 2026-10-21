// 音频模块只依赖单调时间契约，不链接 CPU 执行器。
// Audio uses only the monotonic clock contract, never a guest CPU executor.
namespace Ryujinx.Cpu { public interface ITickSource { double ElapsedSeconds { get; } } }
namespace NativeAudio {
using System;
using System.Buffers;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using Ryujinx.Audio.Common;
using Ryujinx.Audio.Integration;
using Ryujinx.Audio.Renderer.Parameter;
using Ryujinx.Audio.Renderer.Server;
using Ryujinx.Memory;
using Ryujinx.Memory.Range;

public static unsafe class Bridge {
    internal static delegate* unmanaged<ulong, ulong, int, int> Validate;
    internal static delegate* unmanaged<int, void> Notify;
    internal static delegate* unmanaged<short*, int, int, float, int> Play;
    static AudioRendererManager manager;
    static Session outputSession;
    [UnmanagedCallersOnly(EntryPoint="dq_audio_consumed")]
    public static void Consumed(int frames) {
        var session=Volatile.Read(ref outputSession);
        if(session!=null){Interlocked.Add(ref session.played,(ulong)frames);session.driver.GetUpdateRequiredEvent().Set();}
    }
    static AudioRenderSystem renderer;
    static readonly DirectMemory memory = new();
    static uint Error(Exception e) { Console.Error.WriteLine("[native-audio] " + e); return 0x10499; }
    [UnmanagedCallersOnly(EntryPoint="dq_audio_init")]
    public static int Init(delegate* unmanaged<ulong, ulong, int, int> validate, delegate* unmanaged<int, void> notify, delegate* unmanaged<short*, int, int, float, int> play) {
        Validate=validate; Notify=notify; Play=play;
        Ryujinx.Common.Logging.Logger.Info?.Print(Ryujinx.Common.Logging.LogClass.AudioRenderer,"Native audio bridge initialized");
        Ryujinx.Common.Logging.Logger.Flush();
        manager=new AudioRendererManager(new Clock());
        var events=new IWritableEvent[Ryujinx.Audio.Constants.AudioRendererSessionCountMax];
        for(int i=0;i<events.Length;i++) events[i]=new Event();
        manager.Initialize(events, new Driver()); return 0;
    }
    [UnmanagedCallersOnly(EntryPoint="dq_audio_size")]
    public static ulong Size(byte* configuration) {
        try { var c=Unsafe.ReadUnaligned<AudioRendererConfiguration>(configuration); return AudioRendererManager.GetWorkBufferSize(ref c); }
        catch(Exception e) { Error(e); return 0; }
    }
    [UnmanagedCallersOnly(EntryPoint="dq_audio_open")]
    public static uint Open(byte* configuration, ulong address, ulong size) {
        try { var c=Unsafe.ReadUnaligned<AudioRendererConfiguration>(configuration); return (uint)manager.OpenAudioRenderer(out renderer,memory,ref c,1,address,size,1); }
        catch(Exception e) { return Error(e); }
    }
    [UnmanagedCallersOnly(EntryPoint="dq_audio_update")]
    public static uint Update(byte* input, int ni, byte* output, int no, byte* perf, int np) {
        try {
            if(ni<64 || no<64 || np<0 || ni>1048576 || no>1048576 || np>1048576) return 0x10499;
            return (uint)renderer.Update(new NativeMemoryManager<byte>(output,no).Memory,
                np==0 ? Memory<byte>.Empty : new NativeMemoryManager<byte>(perf,np).Memory,
                new ReadOnlySequence<byte>(new NativeMemoryManager<byte>(input,ni).Memory));
        } catch(Exception e) { return Error(e); }
    }
    [UnmanagedCallersOnly(EntryPoint="dq_audio_control")]
    public static uint Control(int command, uint value, uint* result) {
        try {
            switch(command) {
                case 0:*result=renderer.GetSampleRate();break;
                case 1:*result=renderer.GetSampleCount();break;
                case 2:*result=renderer.GetMixBufferCount();break;
                case 3:*result=renderer.IsActive()?0u:1u;break;
                case 5:renderer.Start();break;
                case 6:renderer.Stop();break;
                case 8:if(value>100)return 0x10499;renderer.SetRenderingTimeLimitPercent(value);break;
                case 9:*result=renderer.GetRenderingTimeLimit();break;
                default:return 0x10499;
            }return 0;
        }catch(Exception e){return Error(e);}
    }
    sealed class Clock:Ryujinx.Cpu.ITickSource { readonly Stopwatch watch=Stopwatch.StartNew();public double ElapsedSeconds=>watch.Elapsed.TotalSeconds; }
    sealed class Event:IWritableEvent {public void Signal()=>Notify(1);public void Clear()=>Notify(0);}
    internal sealed class Driver:IHardwareDeviceDriver {
        readonly ManualResetEvent update=new(false),pause=new(true);
        public static bool IsSupported=>true; public float Volume{get;set;}=1;
        public IHardwareDeviceSession OpenDeviceSession(IHardwareDeviceDriver.Direction direction,IVirtualMemoryManager mm,SampleFormat fmt,uint rate,uint channels){
            if(direction!=IHardwareDeviceDriver.Direction.Output || fmt!=SampleFormat.PcmInt16 || rate!=48000 || channels!=2)throw new NotSupportedException("audio format");
            return new Session(this,mm,fmt,rate,channels);
        }
        public ManualResetEvent GetUpdateRequiredEvent()=>update;
        public ManualResetEvent GetPauseEvent()=>pause;
        public bool SupportsDirection(IHardwareDeviceDriver.Direction d)=>d==IHardwareDeviceDriver.Direction.Output;
        public bool SupportsSampleRate(uint r)=>r==48000;
        public bool SupportsSampleFormat(SampleFormat f)=>f==SampleFormat.PcmInt16;
        public bool SupportsChannelCount(uint c)=>c==2;
        public void Dispose(){pause.Dispose();}
    }
    sealed class Session:Ryujinx.Audio.Backends.Common.HardwareDeviceSessionOutputBase {
        internal readonly Driver driver; internal ulong played; ulong queued;float volume=1;bool running;
        public Session(Driver d,IVirtualMemoryManager m,SampleFormat f,uint r,uint c):base(m,f,r,c){driver=d;}
        public override void QueueBuffer(AudioBuffer b){
            if(!running)throw new InvalidOperationException("output stopped");
            if(b.Data==null || b.DataSize>(ulong)b.Data.Length || b.DataSize%4!=0)throw new ArgumentException("PCM bounds");
            // 原生层复制后可接下一段；真实消费回调才推进played，避免单缓冲持续欠载。
            if(Interlocked.CompareExchange(ref outputSession,this,null) is Session active && active!=this)
                throw new NotSupportedException("one active renderer output");
            queued+=b.DataSize/4; b.PlayedTimestamp=queued;
            fixed(byte* p=b.Data)if(Play((short*)p,checked((int)b.DataSize/4),2,volume*driver.Volume)!=0)throw new InvalidOperationException("CoreAudio output failed");
        }
        public override ulong GetPlayedSampleCount()=>Interlocked.Read(ref played);
        public override float GetVolume()=>volume;
        public override void SetVolume(float v)=>volume=v;
        public override void Start()=>running=true;
        public override void Stop()=>running=false;
        public override bool WasBufferFullyConsumed(AudioBuffer b)=>b.PlayedTimestamp!=0 && GetPlayedSampleCount()>=b.PlayedTimestamp;
        public override void PrepareToClose()=>Stop();
        public override void Dispose()=>Stop();
    }
    sealed class DirectMemory:IVirtualMemoryManager {
        public bool UsesPrivateAllocations=>false;
        static void Check(ulong a,ulong n,bool write=false){if(n>int.MaxValue || a>ulong.MaxValue-n || Validate(a,n,write?1:0)==0)throw new ArgumentOutOfRangeException("guest audio memory");}
        static Memory<byte> View(ulong a,int n,bool write=false){Check(a,(ulong)n,write);return new NativeMemoryManager<byte>((byte*)a,n).Memory;}
        public T Read<T>(ulong a)where T:unmanaged{Check(a,(ulong)sizeof(T));return Unsafe.ReadUnaligned<T>((void*)a);}
        public void Read(ulong a,Span<byte> d)=>GetSpan(a,d.Length).CopyTo(d);
        public void Write<T>(ulong a,T v)where T:unmanaged{Check(a,(ulong)sizeof(T),true);Unsafe.WriteUnaligned((void*)a,v);}
        public void Write(ulong a,ReadOnlySpan<byte> d)=>d.CopyTo(View(a,d.Length,true).Span);
        public bool WriteWithRedundancyCheck(ulong a,ReadOnlySpan<byte>d){bool changed=!d.SequenceEqual(GetSpan(a,d.Length));if(changed)Write(a,d);return changed;}
        public ReadOnlySequence<byte> GetReadOnlySequence(ulong a,int n,bool tracked=false)=>new(View(a,n));
        public ReadOnlySpan<byte> GetSpan(ulong a,int n,bool tracked=false)=>View(a,n).Span;
        public WritableRegion GetWritableRegion(ulong a,int n,bool tracked=false)=>new(null,a,View(a,n,true));
        public ref T GetRef<T>(ulong a)where T:unmanaged{Check(a,(ulong)sizeof(T),true);return ref Unsafe.AsRef<T>((void*)a);}
        public IEnumerable<HostMemoryRange> GetHostRegions(ulong a,ulong n){Check(a,n);return new[]{new HostMemoryRange((nuint)a,n)};}
        public IEnumerable<MemoryRange> GetPhysicalRegions(ulong a,ulong n){Check(a,n);return new[]{new MemoryRange(a,n)};}
        public bool IsMapped(ulong a)=>Validate(a,1,0)!=0;
        public bool IsRangeMapped(ulong a,ulong n)=>n<=int.MaxValue && a<=ulong.MaxValue-n && Validate(a,n,0)!=0;
        public void SignalMemoryTracking(ulong a,ulong n,bool write,bool precise=false,int? exemptId=null)=>Check(a,n,write);
        public void Map(ulong a,ulong p,ulong n,MemoryMapFlags f)=>throw new NotSupportedException();
        public void MapForeign(ulong a,nuint p,ulong n)=>throw new NotSupportedException();
        public void Unmap(ulong a,ulong n)=>throw new NotSupportedException();
        public void Reprotect(ulong a,ulong n,MemoryPermission p)=>throw new NotSupportedException();
        public void TrackingReprotect(ulong a,ulong n,MemoryPermission p,bool guest)=>throw new NotSupportedException();
    }
}
}
