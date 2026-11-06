param([string]$HostAddr = "10.0.2.2", [int]$Port = 8791)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Net.Sockets;
using System.IO;
using System.Threading;

public class LoopStream {
  [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class EnumComObject { }
  enum EDataFlow { eRender = 0, eCapture = 1, eAll = 2 }
  enum ERole { eConsole = 0, eMultimedia = 1, eCommunications = 2 }

  [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IMMDeviceEnumerator {
    int EnumAudioEndpoints(EDataFlow flow, int mask, out IntPtr col);
    int GetDefaultAudioEndpoint(EDataFlow flow, ERole role, out IMMDevice dev);
  }
  [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IMMDevice {
    int Activate(ref Guid iid, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o);
  }
  [ComImport, Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioClient {
    int Initialize(int share, int flags, long buf, long per, IntPtr fmt, IntPtr guid);
    int GetBufferSize(out int frames);
    int GetStreamLatency(out long lat);
    int GetCurrentPadding(out int pad);
    int IsFormatSupported(int share, IntPtr fmt, out IntPtr closest);
    int GetMixFormat(out IntPtr fmt);
    int GetDevicePeriod(out long def, out long min);
    int Start();
    int Stop();
    int Reset();
    int SetEventHandle(IntPtr h);
    int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object o);
  }
  [ComImport, Guid("C8ADBD64-E71E-48a0-A4DE-185C395CD317"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioCaptureClient {
    int GetBuffer(out IntPtr data, out int frames, out int flags, out long devpos, out long qpcpos);
    int ReleaseBuffer(int frames);
    int GetNextPacketSize(out int frames);
  }

  static byte[] Header(int rate, int ch) {
    var h = new byte[16];
    h[0]=(byte)'P'; h[1]=(byte)'C'; h[2]=(byte)'M'; h[3]=(byte)'1';
    BitConverter.GetBytes(rate).CopyTo(h,4);
    BitConverter.GetBytes(ch).CopyTo(h,8);
    BitConverter.GetBytes(16).CopyTo(h,12);
    return h;
  }

  public static string Run(string host, int port) {
    var en = (IMMDeviceEnumerator)(new EnumComObject());
    IMMDevice dev;
    int hr = en.GetDefaultAudioEndpoint(EDataFlow.eRender, ERole.eMultimedia, out dev);
    if (hr != 0) return "ERR endpoint " + hr;
    Guid iid = typeof(IAudioClient).GUID;
    object o;
    hr = dev.Activate(ref iid, 1, IntPtr.Zero, out o);
    if (hr != 0) return "ERR activate " + hr;
    var ac = (IAudioClient)o;
    IntPtr fmt;
    if (ac.GetMixFormat(out fmt) != 0) return "ERR mixformat";
    short tag = Marshal.ReadInt16(fmt, 0);
    short ch = Marshal.ReadInt16(fmt, 2);
    int rate = Marshal.ReadInt32(fmt, 4);
    short bits = Marshal.ReadInt16(fmt, 14);
    int sub = tag;
    if (tag == -2) sub = Marshal.ReadInt32(fmt, 24);
    if (ac.Initialize(0, 0x20000, 2000000, 0, fmt, IntPtr.Zero) != 0) return "ERR init";
    Guid capIid = typeof(IAudioCaptureClient).GUID;
    object co;
    if (ac.GetService(ref capIid, out co) != 0) return "ERR service";
    var cap = (IAudioCaptureClient)co;
    if (ac.Start() != 0) return "ERR start";

    var tcp = new TcpClient();
    tcp.NoDelay = true;
    tcp.Connect(host, port);
    var ns = tcp.GetStream();
    ns.Write(Header(rate, ch), 0, 16);
    ns.Flush();

    var sil = new byte[(rate / 100) * ch * 2];   // 10ms silence
    var sbuf = new byte[65536];
    long sent = 0;
    int idle = 0;
    while (true) {
      int pkt;
      if (cap.GetNextPacketSize(out pkt) != 0) break;
      if (pkt == 0) {
        Thread.Sleep(4);
        if (++idle > 12) {                        // ~50ms with no data: keep the pipe fed
          try { ns.Write(sil, 0, sil.Length); } catch { break; }
          idle = 0;
        }
        continue;
      }
      idle = 0;
      IntPtr data; int frames, flags; long a1, a2;
      if (cap.GetBuffer(out data, out frames, out flags, out a1, out a2) != 0) break;
      if (frames > 0) {
        int n = frames * ch;
        int need = n * 2;
        if (need > sbuf.Length) sbuf = new byte[need];
        if (sub == 3) {
          for (int i = 0; i < n; i++) {
            float v = Marshal.PtrToStructure<float>(data + i * 4);
            int s = (int)(v * 32767f);
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            sbuf[i*2] = (byte)(s & 0xff);
            sbuf[i*2+1] = (byte)((s >> 8) & 0xff);
          }
        } else {
          Marshal.Copy(data, sbuf, 0, need);
        }
        try { ns.Write(sbuf, 0, need); sent += need; } catch { break; }
      }
      cap.ReleaseBuffer(frames);
    }
    ac.Stop();
    try { ns.Close(); tcp.Close(); } catch {}
    return "DONE sent=" + sent;
  }
}
'@
$log = "C:\wa.log"
"[$(Get-Date -Format o)] start $HostAddr`:$Port" | Set-Content -Path $log -Encoding ascii
while ($true) {
  try {
    $r = [LoopStream]::Run($HostAddr, $Port)
    "[$(Get-Date -Format o)] $r" | Add-Content -Path $log -Encoding ascii
  } catch {
    "[$(Get-Date -Format o)] EX $($_.Exception.Message)" | Add-Content -Path $log -Encoding ascii
  }
  Start-Sleep -Milliseconds 800
}
