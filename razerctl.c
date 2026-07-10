// SPDX-License-Identifier: GPL-2.0-only
// razerctl - CLIENT for the Razer Blade 16 (1532:02b7) control daemon (razerctld).
// All hidraw/EC access (fan, perf mode, keyboard backlight, battery charge limit,
// the temp-driven fan curve) lives in razerctld now, which owns the device and
// persists+enforces that state across boots. This binary is a thin RPC client over
// a local UNIX socket (razer_ipc.h) -- no root needed for any of that. The dGPU
// undervolt page is the one exception: it talks to NvAPI/NVML directly (unrelated
// to the EC/hidraw), stays in this client, and still needs root to WRITE (reads work
// unprivileged). See razerctld.c for the actual hidraw protocol.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dlfcn.h>              // dlopen libnvidia-api.so / libnvidia-ml.so (dGPU undervolt page)
#include <stdint.h>
#include <dirent.h>             // dGPU PCI auto-discovery (address can change across boots)
#include <sys/stat.h>          // mkdir for the undervolt profile
#include "razer_ipc.h"
// ---- RPC to razerctld: one line request -> one line "OK ..."/"ERR ..." response ----
static int rpc(const char*cmd,char*resp,int rn){
    int s=socket(AF_UNIX,SOCK_STREAM,0); if(s<0) return -1;
    struct sockaddr_un a; memset(&a,0,sizeof a); a.sun_family=AF_UNIX;
    strncpy(a.sun_path,RAZERCTLD_SOCK,sizeof(a.sun_path)-1);
    if(connect(s,(struct sockaddr*)&a,sizeof a)<0){ close(s); return -1; }
    char line[256]; int ln=snprintf(line,sizeof line,"%s\n",cmd);
    if(write(s,line,ln)!=ln){ close(s); return -1; }
    int n=read(s,resp,rn-1); close(s);
    if(n<=0) return -1;
    resp[n]=0; char*nl=strpbrk(resp,"\r\n"); if(nl)*nl=0;
    return strncmp(resp,"OK",2)==0?0:-1;
}
static int rpc_ok(const char*cmd){ char r[128]; return rpc(cmd,r,sizeof r); }
// ---- reads (proxied to razerctld; `fd` kept as a parameter for source compat with
// the TUI/CLI call sites below, but it's unused -- there's no local hidraw fd anymore) ----
static int get_pmode(int fd){ (void)fd; char r[64]; if(rpc("STATUS",r,sizeof r)!=0) return -1;
    int m; return sscanf(r,"OK mode=%d",&m)==1?m:-1; }
static int get_fan_setpoint(int fd){ (void)fd; char r[64]; if(rpc("SETPOINT",r,sizeof r)!=0) return -1;
    int sp; return sscanf(r,"OK %d",&sp)==1?sp:-1; }
static int get_tach(int fd,int zone){ (void)fd; char r[64]; if(rpc("TACH",r,sizeof r)!=0) return -1;
    int f1,f2; if(sscanf(r,"OK %d %d",&f1,&f2)!=2) return -1; return zone==1?f1:f2; }
// ---- writes (each one round-trips to razerctld, which persists it to disk too) ----
static int set_fan(int fd,int rpm){ (void)fd; char c[48]; snprintf(c,sizeof c,"FAN MANUAL %d",rpm); return rpc_ok(c); }
static int set_fan_auto(int fd){ (void)fd; return rpc_ok("FAN AUTO"); }
static int set_fan_curve(int fd){ (void)fd; return rpc_ok("FAN CURVE"); }
static int set_pmode(int fd,int mode,int force_manual){ (void)fd; if(mode<0||mode>2) return -1;
    char c[48]; snprintf(c,sizeof c,"MODE %d %d",mode,force_manual?1:0); return rpc_ok(c); }
// ---- battery charge limit (persisted by razerctld) ----
static int get_charge_limit(int fd){ (void)fd; char r[64]; if(rpc("BATTERYRAW",r,sizeof r)!=0) return -1;
    int raw; return sscanf(r,"OK %d",&raw)==1?raw:-1; }
static int set_charge_limit(int fd,int raw){ (void)fd; char c[48];
    if(raw==0x41) snprintf(c,sizeof c,"BATTERY OFF"); else snprintf(c,sizeof c,"BATTERY %d",raw&0x7f);
    return rpc_ok(c); }
// Keyboard backlight presets (index 0 = off; cycled by TUI 'k', named by CLI `kbd`).
// Defined before kbd_color() below since it needs to map an (r,g,b) triplet back to
// razerctld's KBD index (the daemon owns the same table; this just has to agree with it).
static const struct { const char*name; int r,g,b; } KBD[] = {
    {"off",-1,-1,-1}, {"white",255,255,255}, {"red",255,0,0}, {"purple",128,0,128}, {"green",0,255,0}
};
#define NKBD ((int)(sizeof KBD / sizeof KBD[0]))
static int kbd_off(int fd){ (void)fd; return rpc_ok("KBD 0"); }
static int kbd_color(int fd,int r,int g,int b){ (void)fd;
    for(int i=0;i<NKBD;i++) if(KBD[i].r==r&&KBD[i].g==g&&KBD[i].b==b){ char c[16]; snprintf(c,sizeof c,"KBD %d",i); return rpc_ok(c); }
    return -1;
}
static const char* modename(int m){return m==0?"Balanced":m==1?"Gaming":m==2?"Creator":"?";}
// (Removed: reclaim_dgpu / KWin restart. We now reboot into an iGPU-only BIOS
// state to park the dGPU, so restarting the compositor to "drop" the dGPU made
// no sense in dGPU-only MUX mode -- deprecated 2026-06-21.)
// ---------- TUI ----------
static struct termios orig;
static char obuf[1<<15];
static void raw_on(){ tcgetattr(0,&orig); struct termios t=orig; t.c_lflag&=~(ICANON|ECHO);
    t.c_cc[VMIN]=1; t.c_cc[VTIME]=0; tcsetattr(0,TCSANOW,&t);
    setvbuf(stdout,obuf,_IOFBF,sizeof obuf);   // whole frame -> one write() (no per-line flush flicker)
    printf("\033[2J\033[?25l"); }              // one full clear on entry; frames then redraw in place
static void raw_off(){ tcsetattr(0,TCSANOW,&orig); printf("\033[?25h\033[0m\n"); }
static void rds(const char*p,char*o,int n){ o[0]=0; FILE*f=fopen(p,"r"); if(!f)return;
    if(fgets(o,n,f)){ int l=strlen(o); if(l&&o[l-1]==0x0a)o[l-1]=0; } fclose(f); }
// Discover the dGPU PCI address at runtime (it can shift across boots, e.g. with
// MUX-mode changes) instead of hardcoding 0000:01:00.0. Scan for the NVIDIA
// (0x10de) VGA/3D controller; cache the result. Falls back to the usual slot.
static const char* dgpu_pci(void){
    static char addr[20]=""; if(addr[0]) return addr;
    DIR*d=opendir("/sys/bus/pci/devices");
    if(d){ struct dirent*e; char p[300],v[16],c[16];
        while((e=readdir(d))){ if(e->d_name[0]=='.')continue;
            snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/vendor",e->d_name); rds(p,v,sizeof v);
            if(strncmp(v,"0x10de",6))continue;
            snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/class",e->d_name); rds(p,c,sizeof c);
            if(!strncmp(c,"0x0300",6)||!strncmp(c,"0x0302",6)){ strncpy(addr,e->d_name,sizeof addr-1); break; } }
        closedir(d); }
    if(!addr[0]) strcpy(addr,"0000:01:00.0");
    return addr;
}
static void dgpu_path(char*buf,int n,const char*suffix){ snprintf(buf,n,"/sys/bus/pci/devices/%s/%s",dgpu_pci(),suffix); }
static long rdl(const char*p){FILE*f=fopen(p,"r");if(!f)return -1;long v=-1;if(fscanf(f,"%ld",&v)!=1)v=-1;fclose(f);return v;}
// NOTE: current_now/voltage_now are UNSIGNED magnitudes on this hardware -- they
// do NOT flip sign when discharging (verified empirically). AC0/online only means
// "a charger is physically connected", not "the battery is actually charging".
//
// The EC's OWN BAT0/status field (ACPI _BST "Charging"/"Discharging") is ALSO not
// trustworthy on this hardware -- verified empirically 2026-07-02: status reported
// "Charging" for 86+ straight seconds while charge_now was CONTINUOUSLY DROPPING
// (real discharge, ~70mAh lost). Windows shows the identical lag (same EC firmware,
// same _BST method) -- this is an EC/firmware bug, not a Linux driver issue.
//
// So direction is derived here from the actual charge_now DELTA across polls (the
// only ground truth that can't lie), not from the EC's advertised status string.
// NOISE_UAH filters ADC jitter at rest from a real charge/discharge trend.
#define CHARGE_NOISE_UAH 300
static double batt_watts(int*ac, char*status, int statuslen){
    long c=rdl("/sys/class/power_supply/BAT0/current_now");
    long v=rdl("/sys/class/power_supply/BAT0/voltage_now");
    *ac=(int)rdl("/sys/class/power_supply/AC0/online");

    static long prev_charge=-1;
    long charge=rdl("/sys/class/power_supply/BAT0/charge_now");
    if(charge>=0){
        if(prev_charge>=0){
            long delta=charge-prev_charge;
            if(delta>CHARGE_NOISE_UAH) snprintf(status,statuslen,"Charging");
            else if(delta<-CHARGE_NOISE_UAH) snprintf(status,statuslen,"Discharging");
            else snprintf(status,statuslen,"Steady");
        } else snprintf(status,statuslen,"...");   // first poll: no baseline yet
        prev_charge=charge;
    } else snprintf(status,statuslen,"?");         // charge_now unreadable

    return (c>0&&v>0)?(c/1e6)*(v/1e6):0.0;
}
static double mono(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static long long rdll(const char*p){FILE*f=fopen(p,"r");if(!f)return -1;long long v=-1;if(fscanf(f,"%lld",&v)!=1)v=-1;fclose(f);return v;}
static double rapl_w(const char*path,long long*prev,double*pts){
    long long e=rdll(path); double t=mono();
    if(e<0) return -1;
    if(*prev<0){*prev=e;*pts=t;return 0;}
    double dt=t-*pts; long long de=e-*prev;
    if(de<0) de+=262143328850LL;  /* wrap = max_energy_range_uj for this RAPL domain (pkg & core share it on this CPU) */
    *prev=e;*pts=t; return dt>0?(de/1e6)/dt:0;
}
static int pkg_temp(void){
    static char path[160]; static int done=0;
    if(!done){ done=1; path[0]=0;
        for(int h=0;h<24&&!path[0];h++){ char np[96]; snprintf(np,sizeof np,"/sys/class/hwmon/hwmon%d/name",h);
            char nm[32]; rds(np,nm,sizeof nm); if(strcmp(nm,"coretemp")) continue;
            for(int t=1;t<48;t++){ char lp[160]; snprintf(lp,sizeof lp,"/sys/class/hwmon/hwmon%d/temp%d_label",h,t);
                char lab[40]; rds(lp,lab,sizeof lab); if(strstr(lab,"Package")){ snprintf(path,sizeof path,"/sys/class/hwmon/hwmon%d/temp%d_input",h,t); break; } } } }
    if(!path[0])return -1; long long v=rdll(path); return v<0?-1:(int)(v/1000);
}
// (The temp-driven fan curve -- CPU/GPU EMA smoothing, curve_rpm/curve_step, FAN_STEP/
// FAN_LOOP_S -- now lives entirely in razerctld.c. It's an ongoing control loop that has
// to keep running whether or not this client is attached, so it makes no sense client-side
// anymore; `razerctl fan curve` / the TUI's Curve setting just tell the daemon "Curve" once.)
static double cpu_busy(void){ static long long pt=-1,pi=-1;
    FILE*f=fopen("/proc/stat","r"); if(!f)return -1; char c[8]; long long u,n,sy,idle,io,ir,si,st;
    if(fscanf(f,"%7s %lld %lld %lld %lld %lld %lld %lld %lld",c,&u,&n,&sy,&idle,&io,&ir,&si,&st)!=9){fclose(f);return -1;} fclose(f);
    long long tot=u+n+sy+idle+io+ir+si+st, idl=idle+io;
    if(pt<0){pt=tot;pi=idl;return 0;}
    long long dt=tot-pt,di=idl-pi; pt=tot;pi=idl; return dt>0?100.0*(dt-di)/dt:0;
}
static double deep_res(int ncpu){ static long long pv=-1; static double pts=-1;
    long long sum=0; for(int i=0;i<ncpu;i++){ char p[128]; snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d/cpuidle/state3/time",i); long long v=rdll(p); if(v>0)sum+=v; }
    double t=mono(); if(pv<0){pv=sum;pts=t;return 0;} double dt=t-pts; long long d=sum-pv; pv=sum;pts=t;
    return dt>0?100.0*(d/1e6)/(dt*ncpu):0;
}
// Live CPU frequency: mean across all online cores + the single peak core (scaling_cur_freq, kHz->MHz).
static void cpu_freq(int ncpu,int*mean,int*max){
    long sum=0,mx=0; int cnt=0;
    for(int i=0;i<ncpu;i++){ char p[128]; snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",i);
        long f=rdl(p); if(f<=0) continue; sum+=f; if(f>mx)mx=f; cnt++; }
    *mean = cnt ? (int)(sum/cnt/1000) : -1;
    *max  = mx  ? (int)(mx/1000)     : -1;
}
// ---- EPP (CPU energy-vs-performance bias; intel_pstate HWP, raw 0-255: 0=max perf, 255=max powersave) ----
// sysfs echoes named tiers for the round values; map those back to the raw number.
#define EPP_PATH0 "/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference"
static const int EPP_PRESET[]={0,32,64,96,128,160,192,224,255};   // knobs: 0..255 step 32 (+255 cap)
#define NEPP ((int)(sizeof EPP_PRESET / sizeof EPP_PRESET[0]))
#define NBATT 4   // battery charge-limit cycle: off/60/70/80
static int epp_named(const char*s){ // standard intel_pstate tier name -> raw value, else -1
    if(!strcmp(s,"performance"))return 0;        if(!strcmp(s,"balance_performance"))return 128;
    if(!strcmp(s,"balance_power"))return 192;     if(!strcmp(s,"power"))return 255;     return -1; }
static int get_epp(char*name,int n){ // returns raw 0-255 (best effort); name <- sysfs string
    char s[24]; rds(EPP_PATH0,s,sizeof s); if(name) snprintf(name,n,"%s",s[0]?s:"?");
    if(!s[0]) return -1; int k=epp_named(s); if(k>=0) return k;
    int v=atoi(s); return (v>=0&&v<=255)?v:-1; }
static int set_epp(int v){ // write EPP (raw 0-255) to every cpu. root always has write access.
    if(v<0)v=0; if(v>255)v=255;
    int n=(int)sysconf(_SC_NPROCESSORS_ONLN), ok=-1;
    for(int i=0;i<n;i++){ char p[128]; snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference",i);
        FILE*f=fopen(p,"w"); if(!f) continue; fprintf(f,"%d\n",v); if(fclose(f)==0) ok=0; }
    return ok; }
static int epp_nearest(int v){ int bi=0,bd=1<<30; for(int i=0;i<NEPP;i++){ int d=v-EPP_PRESET[i]; if(d<0)d=-d; if(d<bd){bd=d;bi=i;} } return bi; }
// ============================================================================
// dGPU per-point V/F-curve UNDERVOLT  (NvAPI via libnvidia-api.so)
// Mechanism + struct offsets ported from the python nvapi_vf.py (verified on
// this Ada 4080). Function IDs reverse-engineered by nvcurve. NvAPI exposes the
// real per-point curve + live voltage that NVML cannot.
// ============================================================================
#define NVF_INIT   0x0150E828u
#define NVF_ENUM   0xE5AC921Fu
#define NVF_NAME   0xCEEE8E9Fu
#define NVF_VFP    0x21537AD4u   // GetVFPCurve: base curve
#define NVF_MASK   0x507B4B59u   // GetClockBoostMask
#define NVF_CT     0x23F1B133u   // GetClockBoostTable: per-point offsets
#define NVF_VOLT   0x465F9BCFu   // GetCurrentVoltage
#define NVF_SETCT  0x0733E009u   // SetClockBoostTable: write per-point offsets
#define VFP_SIZE 0x1C28
#define VFP_BASE 0x48
#define VFP_STRIDE 0x1C
#define CT_SIZE  0x2420
#define CT_BASE  0x44
#define CT_STRIDE 0x24
#define CT_DELTA 0x14
#define MASK_SIZE 0x182C
#define VOLT_SIZE 0x4C
#define VOLT_OFF  0x28
#define NVPTS 128
typedef int (*nvfn0)(void);
typedef int (*nvfn2)(void*,void*);
static struct {
    void *lib; void*(*qi)(unsigned); void *gpu; int ok; char name[96];
    int n; int freq[NVPTS]; int volt[NVPTS]; int isgpu[NVPTS];   // base curve (kHz,uV)
} NV={0};
static unsigned char NVMASK[32];

static void* nvq(unsigned fid){ return NV.qi?(void*)NV.qi(fid):NULL; }
static int nvcall(unsigned fid,void*buf,int size,int ver){
    void*p=nvq(fid); if(!p) return -1;
    *(uint32_t*)buf=((unsigned)ver<<16)|size;
    return ((nvfn2)p)(NV.gpu,buf);
}
static int nv_get_mask(void){
    unsigned char b[MASK_SIZE]; memset(b,0,sizeof b);
    for(int i=4;i<36;i++) b[i]=0xFF;
    if(nvcall(NVF_MASK,b,MASK_SIZE,1)!=0) return -1;
    memcpy(NVMASK,b+4,32); return 0;
}
static int nv_read_curve(void){
    if(nv_get_mask()!=0) return -1;
    unsigned char v[VFP_SIZE]; memset(v,0,sizeof v); for(int i=0;i<32;i++) v[4+i]=NVMASK[i];
    if(nvcall(NVF_VFP,v,VFP_SIZE,1)!=0) return -1;
    unsigned char c[CT_SIZE]; memset(c,0,sizeof c); for(int i=0;i<32;i++) c[4+i]=NVMASK[i];
    if(nvcall(NVF_CT,c,CT_SIZE,1)!=0) return -1;
    NV.n=0; int in_mem=0;
    for(int i=0;i<NVPTS;i++){
        uint32_t f=*(uint32_t*)(v+VFP_BASE+i*VFP_STRIDE);
        uint32_t vo=*(uint32_t*)(v+VFP_BASE+i*VFP_STRIDE+4);
        uint32_t flags=*(uint32_t*)(c+CT_BASE+i*CT_STRIDE);
        if(f==0&&vo==0) continue;
        if(flags==1) in_mem=1;
        NV.freq[NV.n]=f; NV.volt[NV.n]=vo;
        NV.isgpu[NV.n]=(!in_mem && f>150000 && f<3300000 && vo>400000 && vo<1300000);
        NV.n++;
    }
    return 0;
}
static int nv_voltage_mv(void){
    if(!NV.ok) return -1;
    unsigned char b[VOLT_SIZE]; memset(b,0,sizeof b);
    if(nvcall(NVF_VOLT,b,VOLT_SIZE,1)!=0) return -1;
    return (int)(*(uint32_t*)(b+VOLT_OFF)/1000);
}
static int nv_init(void){
    NV.lib=dlopen("libnvidia-api.so.1",RTLD_NOW); if(!NV.lib) NV.lib=dlopen("libnvidia-api.so",RTLD_NOW);
    if(!NV.lib) return -1;
    NV.qi=(void*(*)(unsigned))dlsym(NV.lib,"nvapi_QueryInterface"); if(!NV.qi) return -1;
    void*ip=nvq(NVF_INIT); if(!ip||((nvfn0)ip)()!=0) return -1;
    void*ep=nvq(NVF_ENUM); if(!ep) return -1;
    void*gpus[64]={0}; int ng=0;
    ((nvfn2)ep)(gpus,&ng); if(ng<=0) return -1;
    NV.gpu=gpus[0];
    void*np=nvq(NVF_NAME); if(np){ char nb[256]={0}; ((nvfn2)np)(NV.gpu,nb); strncpy(NV.name,nb,sizeof NV.name-1); }
    if(nv_read_curve()!=0) return -1;
    NV.ok=1; return 0;
}
// interpolate freq(kHz) at voltage vuv over gpu-core curve (sorted ascending)
static int nv_interp(int vuv){
    int first=-1,last=-1;
    for(int i=0;i<NV.n;i++) if(NV.isgpu[i]){ if(first<0)first=i; last=i; }
    if(first<0) return 0;
    if(vuv<=NV.volt[first]) return NV.freq[first];
    if(vuv>=NV.volt[last])  return NV.freq[last];
    int prev=first;
    for(int i=first;i<=last;i++){ if(!NV.isgpu[i])continue;
        if(NV.volt[i]>=vuv){ int v0=NV.volt[prev],f0=NV.freq[prev],v1=NV.volt[i],f1=NV.freq[i];
            if(v1==v0) return f1; return f0+(int)((double)(vuv-v0)/(v1-v0)*(f1-f0)); }
        prev=i; }
    return NV.freq[last];
}
// write per-point undervolt: left-shift curve by mv above min_freq_mhz; <0 err
static int nv_write_uv(int mv,int minf_mhz){
    if(!NV.ok) return -1;
    unsigned char c[CT_SIZE]; memset(c,0,sizeof c); for(int i=0;i<32;i++) c[4+i]=NVMASK[i];
    if(nvcall(NVF_CT,c,CT_SIZE,1)!=0) return -1;        // read current table as base
    *(uint32_t*)c=(1u<<16)|CT_SIZE;
    for(int i=4;i<36;i++) c[i]=0;                        // clear mask -> sparse
    int dv=mv*1000, floor=minf_mhz*1000;
    for(int i=0;i<NV.n;i++){
        if(!NV.isgpu[i]) continue;
        int delta=0;
        if(NV.freq[i]>=floor){ int t=nv_interp(NV.volt[i]+dv); delta=t-NV.freq[i];
            if(delta>1000000)delta=1000000; if(delta<-1000000)delta=-1000000; }
        c[4+(i/8)] |= (1<<(i%8));
        *(int32_t*)(c+CT_BASE+i*CT_STRIDE+CT_DELTA)=delta;
    }
    void*sp=nvq(NVF_SETCT); if(!sp) return -1;
    return ((nvfn2)sp)(NV.gpu,c);
}
static int nv_reset(void){ return nv_write_uv(0,0); }
static int nv_uv_count(int mv,int minf_mhz,int*peak_mhz){      // dry preview
    int dv=mv*1000,floor=minf_mhz*1000,cnt=0,pk=0;
    for(int i=0;i<NV.n;i++){ if(!NV.isgpu[i]||NV.freq[i]<floor) continue;
        int d=nv_interp(NV.volt[i]+dv)-NV.freq[i]; if(d){cnt++; if(d>pk)pk=d;} }
    if(peak_mhz)*peak_mhz=pk/1000; return cnt;
}
// ---- undervolt profile persistence (~/.config/razerctl-uv.conf) ----
static void uv_path(char*b,int n){ const char*h=getenv("HOME"); snprintf(b,n,"%s/.config/razerctl-uv.conf",(h&&*h)?h:"/root"); }
static void uv_save(int mv,int minf,int maxf){
    const char*h=getenv("HOME"); char dir[256]; snprintf(dir,sizeof dir,"%s/.config",(h&&*h)?h:"/root"); mkdir(dir,0755);
    char p[300]; uv_path(p,sizeof p); FILE*f=fopen(p,"w");
    if(f){ fprintf(f,"uv_mv=%d\nmin_freq=%d\nmax_freq=%d\n",mv,minf,maxf); fclose(f); }
}
static int uv_load(int*mv,int*minf,int*maxf){
    char p[300]; uv_path(p,sizeof p); FILE*f=fopen(p,"r"); if(!f) return -1;
    char k[40]; int v; *mv=0;*minf=0;*maxf=0;
    while(fscanf(f,"%39[^=]=%d ",k,&v)==2){
        if(!strcmp(k,"uv_mv"))*mv=v; else if(!strcmp(k,"min_freq"))*minf=v; else if(!strcmp(k,"max_freq"))*maxf=v; }
    fclose(f); return 0;
}
// ---- NVML-lite (dlopen) for dGPU clock/power/temp on the undervolt page ----
static struct { void*lib; void*h; int ok;
    int(*clk)(void*,int,unsigned*); int(*pw)(void*,unsigned*); int(*tp)(void*,int,unsigned*);
} ML={0};
static int ml_init(void){
    ML.lib=dlopen("libnvidia-ml.so.1",RTLD_NOW); if(!ML.lib) ML.lib=dlopen("libnvidia-ml.so",RTLD_NOW);
    if(!ML.lib) return -1;
    int(*init)(void)=(int(*)(void))dlsym(ML.lib,"nvmlInit_v2"); if(!init||init()!=0) return -1;
    int(*gh)(unsigned,void**)=(int(*)(unsigned,void**))dlsym(ML.lib,"nvmlDeviceGetHandleByIndex_v2");
    if(!gh||gh(0,&ML.h)!=0) return -1;
    ML.clk=(int(*)(void*,int,unsigned*))dlsym(ML.lib,"nvmlDeviceGetClockInfo");
    ML.pw =(int(*)(void*,unsigned*))dlsym(ML.lib,"nvmlDeviceGetPowerUsage");
    ML.tp =(int(*)(void*,int,unsigned*))dlsym(ML.lib,"nvmlDeviceGetTemperature");
    ML.ok=1; return 0;
}
static int ml_core(void){ unsigned v=0; return (ML.ok&&ML.clk&&ML.clk(ML.h,0,&v)==0)?(int)v:-1; }   // NVML_CLOCK_GRAPHICS=0
static int ml_power(void){ unsigned v=0; return (ML.ok&&ML.pw&&ML.pw(ML.h,&v)==0)?(int)(v/1000):-1; }
static int ml_temp(void){ unsigned v=0; return (ML.ok&&ML.tp&&ML.tp(ML.h,0,&v)==0)?(int)v:-1; }     // NVML_TEMPERATURE_GPU=0
// nvmlDeviceSetGpuLockedClocks/ResetGpuLockedClocks are privileged NVML calls that
// require REAL ROOT (euid 0) -- unlike the NvAPI curve write, cap_sys_admin is NOT
// enough, the call just returns NO_PERMISSION. Return the NVML rc so callers can
// surface that (0=ok). maxf<=0 means "no cap" -> caller should nv_unlock() instead.
static int nv_lock_max(int maxf){ if(maxf>0&&ML.ok){ int(*lk)(void*,unsigned,unsigned)=(int(*)(void*,unsigned,unsigned))dlsym(ML.lib,"nvmlDeviceSetGpuLockedClocks"); if(lk) return lk(ML.h,210,maxf); } return -1; }
// Release any locked-clock clamp (also root-only). Safe no-op if NVML unavailable.
static int nv_unlock(void){ if(ML.ok){ int(*ul)(void*)=(int(*)(void*))dlsym(ML.lib,"nvmlDeviceResetGpuLockedClocks"); if(ul) return ul(ML.h); } return -1; }

// "on" = nvidia-powerd ACTIVE this boot. We toggle via start/stop (runtime, polkit-scoped to the unit),
// NOT enable/disable, so it's intentionally non-persistent: a reboot returns to powerd OFF (D3cold default).
static int powerd_enabled(void){ return system("/usr/bin/systemctl is-active --quiet nvidia-powerd 2>/dev/null")==0; }

// ---- arrow-key input: returns a logical key or a raw char ----
enum { K_UP=0x100,K_DOWN,K_LEFT,K_RIGHT,K_ENTER,K_ESC };
// Read ONE more byte of an in-flight escape sequence, waiting up to `us` for it.
// Uses raw read(0) -- NOT getchar() -- so it shares the same kernel buffer the
// main loop's select() polls. (Mixing getchar()'s stdio buffer with select() on
// the raw fd silently dropped arrow keys: getchar() slurped the whole ESC[A into
// stdio, select() saw an empty kernel buffer, and the arrow decoded as a bare ESC.)
static int seq_byte(int us){
    fd_set r; FD_ZERO(&r); FD_SET(0,&r); struct timeval tv={0,us};
    if(select(1,&r,0,0,&tv)<=0) return -1;
    unsigned char b; return read(0,&b,1)==1 ? b : -1;
}
static int readkey(void){
    unsigned char c; if(read(0,&c,1)!=1) return -1;
    if(c==27){   // ESC alone, or CSI arrow sequence -- 50ms grace so a terminal that
        int a=seq_byte(50000);            // splits ESC [ A across reads (SSH) isn't chopped
        if(a<0) return K_ESC;
        if(a=='['){ int b=seq_byte(50000);
            if(b=='A')return K_UP; if(b=='B')return K_DOWN;
            if(b=='C')return K_RIGHT; if(b=='D')return K_LEFT; return -1; }
        return K_ESC;
    }
    if(c=='\r'||c=='\n') return K_ENTER;
    return c;
}
#define ROWC(y,is,...) do{ printf("  %s ",(is)?"\033[1;36m▶":" "); printf(__VA_ARGS__); printf("\033[0m\033[K\n"); }while(0)
#define MROWS 9
#define DROWS 6

// Pull razerctld's authoritative persisted state (mode/fanmode/fanrpm/kbd/battery) once
// at TUI startup, so opening the dashboard reflects whatever's actually running instead
// of guessing/forcing defaults -- the whole point of the daemon is that it keeps whatever
// was last set, TUI open or not.
static int status_pull(int*mode,int*fanmode,int*fanrpm,int*kbd,int*battery){
    char r[160]; if(rpc("STATUS",r,sizeof r)!=0) return -1;
    return sscanf(r,"OK mode=%d fanmode=%d fanrpm=%d kbd=%d battery=%d",mode,fanmode,fanrpm,kbd,battery)==5?0:-1;
}
static int batti_from_pct(const int*BP,int n,int battery){
    for(int i=0;i<n;i++) if(BP[i]==(battery<0?0:battery)) return i; return 0;
}
static int tui(int fd,const char*node){
    int manual=0,rpm=4000; char msg[160]="";
    static const int BP[NBATT]={0,60,70,80};
    int pm=0,fanmode=0,fanrpm=4000,kbi=0,battery=-1;
    if(status_pull(&pm,&fanmode,&fanrpm,&kbi,&battery)==0){ rpm=fanrpm; }
    else snprintf(msg,sizeof msg,"razerctld unreachable -- showing stale/default state");
    manual=(fanmode==1);
    int sp=get_fan_setpoint(fd);
    long long pe=-1,ce=-1; double pts=0,cts=0; int ncpu=(int)sysconf(_SC_NPROCESSORS_ONLN); int mon=1;
    int powerd_on=powerd_enabled(); int eppi=epp_nearest(get_epp(NULL,0));
    int batti=batti_from_pct(BP,NBATT,battery);
    int page=0, sel=0, dsel=0;                               // page 0=main 1=dGPU undervolt
    int uv_mv=0, minf=1695, maxf=2400, nv_applied=0, nv_started=0; char nverr[80]="";  // sensible starts (15MHz-aligned): floor 1695, ceiling 2400
    // Telemetry is cached and refreshed at most once per POLL_S; redraws/keypresses reuse the
    // cache so navigation stays snappy and the delta metrics (W, busy%) keep a steady window.
    #define POLL_S 5.0
    int f1=-1,f2=-1,temp=-1,ac=-1,meanf=-1,maxf_f=-1; double bw=0,pkgw=-1,corew=-1,busy=-1,deep=-1;
    char gst[32]="?",gps[16]="?",bstatus[24]="?"; double last_poll=-1;
    raw_on();
    for(;;){
        // ---- telemetry (throttled to POLL_S) ----
        if(mon && page==0 && (last_poll<0 || mono()-last_poll>=POLL_S)){
            last_poll=mono();
            f1=get_tach(fd,1); f2=get_tach(fd,2); bw=batt_watts(&ac,bstatus,sizeof bstatus);
            { char pp[300]; dgpu_path(pp,sizeof pp,"power/runtime_status"); rds(pp,gst,sizeof gst);
              dgpu_path(pp,sizeof pp,"power_state"); rds(pp,gps,sizeof gps); }
            pkgw=rapl_w("/sys/class/powercap/intel-rapl:0/energy_uj",&pe,&pts);
            corew=rapl_w("/sys/class/powercap/intel-rapl:0:0/energy_uj",&ce,&cts);
            temp=pkg_temp(); busy=cpu_busy(); deep=deep_res(ncpu); cpu_freq(ncpu,&meanf,&maxf_f);
            powerd_on=powerd_enabled();
            sp=get_fan_setpoint(fd);
            if(fanmode!=1) rpm=sp;   // Auto/Curve: show the daemon's live setpoint, not a stale local guess
        }

        // ---- render ----
        if(page==0){
            char eppname[24]; int epp=get_epp(eppname,sizeof eppname);
            printf("\033[H\033[1;36m  razerctl\033[0m  Blade 16 (1532:02b7)  [%s]\033[K\n",node);
            printf("  --------------------------------------------\n");
            if(mon){
                { const char*bcol=!strcmp(bstatus,"Discharging")?"\033[1;31m":"\033[1;33m";
                  const char*blabel=bstatus[0]?bstatus:(ac?"AC":"BAT");   // fallback only if /status is unreadable
                  printf("   Fan RPM  \033[1;32m%4d / %4d\033[0m   Batt %s%s %.1fW\033[0m\033[K\n",f1,f2,bcol,blabel,bw); }
                { int gone=(gst[0]==0); int asleep=gone||(strcmp(gst,"suspended")==0);
                  printf("   dGPU     %s%s\033[0m   CPU \033[1;33m%dC %.0fW\033[0m   busy \033[1;33m%.0f%%\033[0m\033[K\n",
                      asleep?"\033[1;32m":"\033[1;31m", gone?"absent ~0W":(asleep?"asleep ~0W":"AWAKE"),temp,pkgw,busy); }
            } else printf("   \033[1;90m-- monitor paused --\033[0m\033[K\n");
            printf("  --------------------------------------------\n");
            printf("   \033[1mSETTINGS\033[0m  \033[1;90m↑↓ select  ←→ change\033[0m\n");
            ROWC(0,sel==0,"Perf mode : \033[1;33m%-9s\033[0m %s",modename(pm),pm==2?"GPU eco ~80W":pm==1?"boost ~175W":"boost ~118W");
            ROWC(1,sel==1,"Fan       : \033[1;33m%-7s\033[0m %s",fanmode==0?"Auto":fanmode==1?"Manual":"Curve",
                 (fanmode==2)?"temp-driven":(fanmode==1?"manual rpm":"firmware"));
            ROWC(2,sel==2,"Fan RPM   : \033[1;33m%-5d\033[0m %s",rpm,fanmode==1?"":"\033[1;90m(Manual only)\033[0m");
            ROWC(3,sel==3,"Kbd light : \033[1;33m%s\033[0m",KBD[kbi].name);
            ROWC(4,sel==4,"CPU EPP   : \033[1;33m%-3d\033[0m (%s)",epp,eppname);
            ROWC(5,sel==5,"DynBoost  : %s\033[0m",powerd_on?"\033[1;32mON  (≤175W)":"\033[1;31mOFF (80W cap)");
            ROWC(6,sel==6,"Battery   : \033[1;33m%s\033[0m",BP[batti]?({static char bb[8];snprintf(bb,8,"%d%%",BP[batti]);bb;}):"Off (100%)");
            printf("  - - - - - - - - - - - - - - - - - - - - - - \n");
            ROWC(7,sel==7,"Monitoring: %s\033[0m",mon?"\033[1;32mON":"\033[1;90mpaused");
            ROWC(8,sel==8,"\033[1;36mdGPU undervolt ▸\033[0m  \033[1;90mEnter to open\033[0m");
            printf("  --------------------------------------------\n");
            printf("   \033[1;90m↑↓ move   ←→ change   Enter open/toggle   q quit\033[0m\n");
        } else {
            if(!nv_started){ nv_started=1; if(nv_init()!=0) snprintf(nverr,sizeof nverr,"NvAPI init failed (libnvidia-api.so?)"); ml_init(); }
            int liveV=NV.ok?nv_voltage_mv():-1, core=ml_core(), pw=ml_power(), gt=ml_temp();
            int pk=0,cnt=NV.ok?nv_uv_count(uv_mv,minf,&pk):0;
            int root=(geteuid()==0);   // Max-freq cap (SetGpuLockedClocks) is root-only; undervolt is sudo-less
            printf("\033[H\033[1;36m  razerctl · dGPU undervolt\033[0m  [%s]   \033[1;90m◂ Esc: back\033[0m\033[K\n",NV.ok?NV.name:"no NvAPI");
            printf("  --------------------------------------------\n");
            if(NV.ok){
                printf("   LIVE  core \033[1;33m%4dMHz\033[0m  volt \033[1;32m%dmV\033[0m  pwr \033[1;33m%dW\033[0m  temp \033[1;33m%dC\033[0m\033[K\n",
                    core<0?0:core, liveV<0?0:liveV, pw<0?0:pw, gt<0?0:gt);
            } else printf("   \033[1;31m%s\033[0m\033[K\n", nverr[0]?nverr:"NvAPI unavailable");
            if(root) printf("   \033[1;32m● editable (root)\033[0m\033[K\n");
            printf("  --------------------------------------------\n");
            printf("   \033[1mUNDERVOLT\033[0m  \033[1;90m↑↓ select  ←→ change  Enter: Apply/Reset\033[0m\n");
            ROWC(0,dsel==0,"Undervolt : \033[1;33m%-3d mV\033[0m  \033[1;90mcurve left-shift\033[0m",uv_mv);
            ROWC(1,dsel==1,"Min freq  : \033[1;33m%-5d MHz\033[0m \033[1;90mstock below\033[0m",minf);
            ROWC(2,dsel==2,"Max freq  : \033[1;33m%-5s\033[0m \033[1;90mclock ceiling\033[0m",maxf?({static char mb[12];snprintf(mb,12,"%dMHz",maxf);mb;}):"off");
            printf("  - - - - - - - - - - - - - - - - - - - - - - \n");
            ROWC(3,dsel==3,"\033[1;32m[ Apply ]\033[0m  \033[1;90m%d pts, peak +%dMHz\033[0m",cnt,pk);
            ROWC(4,dsel==4,"[ Reset ]  \033[1;90mback to stock curve\033[0m");
            ROWC(5,dsel==5,"\033[1;36m◂ Back\033[0m      \033[1;90mEnter or Esc\033[0m");
            printf("   state: %s\033[0m\033[K\n", nv_applied?"\033[1;32mAPPLIED":"\033[1;90mnot applied");
            printf("  --------------------------------------------\n");
            printf("   \033[1;90m↑↓ move   ←→ change   Enter apply/reset/back   q quit\033[0m\n");
        }
        if(msg[0]) printf("\n   \033[32m%s\033[0m\033[K\n",msg);
        printf("\033[J");   // wipe any lines below the frame (page switch / msg cleared)
        fflush(stdout);

        // ---- input ----
        // Sleep only until the next thing actually falls due (telemetry @POLL_S, dGPU page @1s
        // live voltage); a keypress wakes us early regardless. The fan curve no longer ticks here
        // -- razerctld drives it in the background, TUI open or not.
        double wait;
        if(page){ wait=1.0; }
        else {
            wait=POLL_S;
            if(mon && last_poll>=0){ double w=last_poll+POLL_S-mono(); if(w<wait) wait=w; }
            if(wait<0.1) wait=0.1; if(wait>POLL_S) wait=POLL_S;
        }
        struct timeval tv={(int)wait,(int)((wait-(int)wait)*1e6)};
        fd_set r; FD_ZERO(&r); FD_SET(0,&r);
        if(select(1,&r,0,0, (mon||page)?&tv:NULL)<=0) continue;
        msg[0]=0; int k=readkey();

        if(page==0){
            // Quitting the TUI no longer forces fan mode back to Auto -- razerctld keeps
            // running Curve/Manual/whatever was set in the background after this exits.
            if(k=='q'){ break; }
            else if(k==K_UP){ sel=(sel+MROWS-1)%MROWS; }
            else if(k==K_DOWN){ sel=(sel+1)%MROWS; }
            else if(k==K_ENTER){
                if(sel==7){ mon=!mon; pe=ce=-1; last_poll=-1; snprintf(msg,sizeof msg,mon?"monitoring ON":"monitoring paused"); }
                else if(sel==8){ page=1; dsel=0; }
            }
            else if(k==K_LEFT||k==K_RIGHT){
                int dir=(k==K_RIGHT)?1:-1;
                if(sel==7){ mon=!mon; pe=ce=-1; last_poll=-1; snprintf(msg,sizeof msg,mon?"monitoring ON":"monitoring paused"); }
                else if(sel==0){ int w=(pm+dir+3)%3; set_pmode(fd,w,fanmode==1); pm=get_pmode(fd); powerd_on=powerd_enabled();
                    snprintf(msg,sizeof msg,"mode -> %s",modename(pm)); }
                else if(sel==1){ fanmode=(fanmode+dir+3)%3;
                    if(fanmode==0){ manual=0; set_fan_auto(fd); snprintf(msg,sizeof msg,"fan Auto"); }
                    else if(fanmode==1){ manual=1; set_fan(fd,rpm); snprintf(msg,sizeof msg,"fan Manual %d",rpm); }
                    else { manual=0; set_fan_curve(fd); snprintf(msg,sizeof msg,"fan Curve (temp-driven, runs in razerctld)"); }
                    sp=get_fan_setpoint(fd); }
                else if(sel==2){ if(fanmode==1){ rpm+=dir*500; if(rpm>4800)rpm=4800; if(rpm<2000)rpm=2000;
                    set_fan(fd,rpm); sp=get_fan_setpoint(fd); snprintf(msg,sizeof msg,"rpm %d",rpm); }
                    else snprintf(msg,sizeof msg,"set Fan to Manual first"); }
                else if(sel==3){ kbi=(kbi+dir+NKBD)%NKBD; if(KBD[kbi].r<0)kbd_off(fd); else kbd_color(fd,KBD[kbi].r,KBD[kbi].g,KBD[kbi].b);
                    snprintf(msg,sizeof msg,"kbd -> %s",KBD[kbi].name); }
                else if(sel==4){ eppi=(eppi+dir+NEPP)%NEPP; int v=EPP_PRESET[eppi];
                    if(set_epp(v)==0) snprintf(msg,sizeof msg,"EPP -> %d (TLP resets on AC/DC)",v);
                    else snprintf(msg,sizeof msg,"EPP write failed"); }
                else if(sel==5){ int r2=system(powerd_on?"/usr/bin/systemctl stop nvidia-powerd 2>/dev/null":"/usr/bin/systemctl start nvidia-powerd 2>/dev/null");
                    powerd_on=powerd_enabled(); snprintf(msg,sizeof msg,r2?"boost toggle failed (polkit?)":"DynBoost -> %s",powerd_on?"ON":"OFF"); }
                else if(sel==6){ batti=(batti+dir+NBATT)%NBATT; int p=BP[batti]; int val=p?(p|0x80):0x41;
                    int r2=set_charge_limit(fd,val);
                    if(r2) snprintf(msg,sizeof msg,"charge-limit write FAILED");
                    else if(p) snprintf(msg,sizeof msg,"charge limit -> %d%%",p);
                    else snprintf(msg,sizeof msg,"charge limit OFF (100%%)"); }
            }
        } else {  // dGPU undervolt page
            int can_edit=(geteuid()==0); // only the undervolt Apply/Reset needs root now -- run `sudo razerctl` for this page
            if(k==K_ESC){ page=0; last_poll=-1; }
            else if(k=='q'){ break; }
            else if(k==K_UP){ dsel=(dsel+DROWS-1)%DROWS; }
            else if(k==K_DOWN){ dsel=(dsel+1)%DROWS; }
            else if((k==K_ENTER&&dsel==5)){ page=0; last_poll=-1; }   // Back works without root
            else if(!can_edit){ snprintf(msg,sizeof msg,"cannot edit dGPU settings"); }
            else if(k==K_LEFT||k==K_RIGHT){
                int dir=(k==K_RIGHT)?1:-1;
                if(dsel==0){ uv_mv+=dir*5; if(uv_mv<0)uv_mv=0; if(uv_mv>150)uv_mv=150; }
                else if(dsel==1){ minf+=dir*15; if(minf<0)minf=0; if(minf>3000)minf=3000; }
                else if(dsel==2){ maxf+=dir*15; if(maxf<0)maxf=0; if(maxf>3105)maxf=3105; }
            }
            else if(k==K_ENTER){
                if(!NV.ok) snprintf(msg,sizeof msg,"NvAPI unavailable — cannot apply");
                else if(dsel==3){ int r2=nv_write_uv(uv_mv,minf);
                    if(r2!=0) snprintf(msg,sizeof msg,"curve write FAILED (nvapi %d)",r2);
                    else { nv_applied=1; int pk,c=nv_uv_count(uv_mv,minf,&pk);
                        if(maxf>0) nv_lock_max(maxf); else nv_unlock();
                        uv_save(uv_mv,minf,maxf);
                        snprintf(msg,sizeof msg,"APPLIED -%dmV >%dMHz (%d pts,+%dMHz)%s - saved",uv_mv,minf,c,pk,maxf>0?", capped":""); } }
                else if(dsel==4){ nv_reset(); nv_unlock();
                    nv_applied=0; uv_mv=0; maxf=0; { char p[300]; uv_path(p,sizeof p); unlink(p); }
                    snprintf(msg,sizeof msg,"reset to stock (curve + clocks unlocked)"); }
            }
        }
    }
    if(NV.ok){ nv_reset(); nv_unlock(); }   // leave the GPU at stock on exit (curve + clock clamp; both reset on reboot anyway)
    raw_off(); return 0;
}
int main(int argc,char**argv){
    // dGPU undervolt CLI (no EC/hidraw needed; NvAPI enumerates the GPU by device,
    // so it's immune to PCI-address changes across boots). This is the one client
    // feature that still touches real hardware directly and still needs root to WRITE.
    if(argc>=2 && !strcmp(argv[1],"--uv-apply")){           // headless re-apply (systemd unit)
        if(geteuid()!=0){ fprintf(stderr,"razerctl --uv-apply needs root\n"); return 1; }
        int mv,minf,maxf; if(uv_load(&mv,&minf,&maxf)!=0){ printf("no undervolt profile\n"); return 0; }
        if(nv_init()!=0){ printf("dGPU absent (iGPU-only boot?) - undervolt skipped\n"); return 0; }  // graceful no-op
        int r=nv_write_uv(mv,minf); int lr=0;
        if(maxf>0){ ml_init(); lr=nv_lock_max(maxf); }   // root-only; login service runs as user so this no-ops
        printf("uv-apply: -%dmV >%dMHz%s -> %s%s\n",mv,minf, maxf?" capped":"", r==0?"ok":"FAILED",
            (maxf>0&&lr!=0)?" (max-freq cap skipped: needs root)":"");
        return r==0?0:1;
    }
    if(argc>=2 && !strcmp(argv[1],"uv")){                   // razerctl uv <mv> <minf> [maxf] | reset
        if(nv_init()!=0){ fprintf(stderr,"NvAPI unavailable (libnvidia-api.so?)\n"); return 1; }
        if(argc>=3 && !strcmp(argv[2],"reset")){ nv_reset(); ml_init(); int ur=nv_unlock();
            char p[300]; uv_path(p,sizeof p); unlink(p);
            printf("reset to stock, profile cleared%s\n", ur==0?" (clocks unlocked)":" (clock-unlock needs root: sudo razerctl uv reset)");
            return 0; }
        if(argc<4){ fprintf(stderr,"usage: razerctl uv <mV> <min_freq_MHz> [max_freq_MHz]\n       razerctl uv reset\n"); return 1; }
        int mv=atoi(argv[2]),minf=atoi(argv[3]),maxf=argc>4?atoi(argv[4]):0;
        int r=nv_write_uv(mv,minf);
        if(r!=0){ fprintf(stderr,"curve write FAILED (nvapi %d) - need cap_sys_admin or sudo\n",r); return 1; }
        int lr=0; if(maxf>0){ ml_init(); lr=nv_lock_max(maxf); } else { ml_init(); nv_unlock(); }
        uv_save(mv,minf,maxf); int pk,c=nv_uv_count(mv,minf,&pk);
        if(maxf>0 && lr!=0) fprintf(stderr,"WARNING: max-freq cap NOT applied (nvml %d) - SetGpuLockedClocks needs root: sudo razerctl uv %d %d %d\n",lr,mv,minf,maxf);
        printf("applied -%dmV >%dMHz (%d pts, +%dMHz)%s; profile saved (persists at login)\n",mv,minf,c,pk,(maxf&&lr==0)?", capped":"");
        return 0;
    }
    // Everything else talks to razerctld over the socket -- no hidraw fd here anymore, so no
    // root and no single-instance lock needed (the daemon serializes EC access by itself).
    { char pr[64]; if(rpc("PING",pr,sizeof pr)!=0){
        fprintf(stderr,"razerctl: can't reach razerctld (%s) -- is it running?\n",RAZERCTLD_SOCK);
        fprintf(stderr,"  try: sudo systemctl start razerctld\n");
        return 2; } }
    int fd=0; char node[64]="razerctld";
    if(argc==1) return tui(fd,node);
    if(!strcmp(argv[1],"nvtest")){
        if(nv_init()!=0){ printf("nv_init FAILED\n"); return 1; }
        int g=0; for(int i=0;i<NV.n;i++) if(NV.isgpu[i]) g++;
        printf("GPU: %s  pts=%d gpu-core=%d  liveV=%dmV\n",NV.name,NV.n,g,nv_voltage_mv());
        int pk; int c=nv_uv_count(50,1000,&pk);
        printf("-50mV>1000MHz preview: %d pts, peak +%dMHz\n",c,pk);
        return 0;
    }
    if(!strcmp(argv[1],"get")){
        char en[24]; int ev=get_epp(en,sizeof en);
        printf("node=%s perf=%s fan-setpoint=%d rpm (0=auto) epp=%d (%s) cpu=%dC\n",node,modename(get_pmode(fd)),get_fan_setpoint(fd),ev,en,pkg_temp());
    } else if(!strcmp(argv[1],"epp")){
        // CPU energy-vs-performance bias (intel_pstate HWP, raw 0-255). NOTE: TLP overrides this
        // on every AC/DC switch via CPU_ENERGY_PERF_POLICY_ON_AC/BAT -> manual set is a live nudge.
        char en[24]; int ev=get_epp(en,sizeof en);
        if(argc==2){
            printf("epp=%d (%s)   presets: 0 32 64 96 128 160 192 224 255  (0=max perf, 255=max powersave)\n",ev,en);
            printf("note: TLP resets this on the next AC<->DC change (CPU_ENERGY_PERF_POLICY_ON_AC/BAT)\n");
        } else { int v=atoi(argv[2]); if(v<0)v=0; if(v>255)v=255;
            int r=set_epp(v); char en2[24]; int nv=get_epp(en2,sizeof en2);
            printf("%s epp -> %d (now %d / %s); TLP will reset it on the next AC/DC switch\n", r==0?"ok":"FAILED", v, nv, en2);
            if(r!=0) fprintf(stderr,"  (energy_performance_preference not writable; is /etc/tmpfiles.d/epp-write.conf applied?)\n");
        }
    } else if(!strcmp(argv[1],"fan")&&argc==3){
        if(!strcmp(argv[2],"auto")) printf("%s\n",set_fan_auto(fd)==0?"fan auto":"failed");
        else { int r=atoi(argv[2]); if(r<2000)r=2000; if(r>4800)r=4800;   // Synapse range 2000-4800 (EC caps ~4700 actual; >4800 gains nothing, tested 2026-06-14)
            int ok=set_fan(fd,r)==0; printf("%s %d (setpoint now %d)\n",ok?"fan manual":"failed",r,get_fan_setpoint(fd)); }
    } else if(!strcmp(argv[1],"kbd")&&argc==3){
        const char*c=argv[2]; int i=-1;
        for(int k=0;k<NKBD;k++) if(!strcmp(c,KBD[k].name)){ i=k; break; }
        if(i<0){ fprintf(stderr,"kbd: white|red|purple|green|off\n"); return 1; }
        if(KBD[i].r<0) printf("%s\n", kbd_off(fd)==0?"kbd off":"failed");
        else printf("%s %s @30%%\n", kbd_color(fd,KBD[i].r,KBD[i].g,KBD[i].b)==0?"kbd":"failed", c);
    } else if(!strcmp(argv[1],"rpm")||!strcmp(argv[1],"watch")){
        printf("live fan rpm @2s (Ctrl-C to stop). NOTE: tach lags ~40-50s after a change.\n");
        for(;;){
            printf("\rfan1=%4d fan2=%4d rpm | setpoint=%-4d perf=%-8s ",
                   get_tach(fd,1),get_tach(fd,2),get_fan_setpoint(fd),modename(get_pmode(fd)));
            fflush(stdout); sleep(2);
        }
    } else if(!strcmp(argv[1],"mode")&&argc==3){
        int m = !strcmp(argv[2],"balanced")?0:!strcmp(argv[2],"gaming")?1:!strcmp(argv[2],"creator")?2:-1;
        if(m<0){ fprintf(stderr,"mode: balanced|gaming|creator\n"); return 1; }
        int r=set_pmode(fd,m,0);   // CLI has no fan-mode state across processes -- never force manual here
        printf("%s -> %s  (GPU %s)\n", r==0?"ok":"FAILED", modename(get_pmode(fd)),
               m==2?"eco ~80W, Dynamic Boost OFF":m==1?"boost up to ~175W":"boost ~118W");
    } else if(!strcmp(argv[1],"battery")){
        // EC battery charge limit. SETTER opcode is well-attested from the Synapse capture
        // (0x07/0x12 arg = pct|0x80; 60->bc 65->c1 80->d0 off->41), BUT the readback (0x07/0x8f)
        // does NOT return the percentage -- it gives a status byte (0x02 in Windows, 0x00 here),
        // so a write cannot be auto-verified; verify behaviorally (charging stops near the limit).
        int raw=get_charge_limit(fd);
        if(argc==2||(argc==3&&!strcmp(argv[2],"status"))){
            printf("charge-limit EC byte (0x07/0x8f): %s%d (0x%02x) -- NOTE: this is a status byte, not the %%; readback unconfirmed\n",
                   raw<0?"read FAILED ":"", raw, raw<0?0:raw);
        } else if((argc==3&&!strcmp(argv[2],"off")) || argc==3){
            int off = !strcmp(argv[2],"off");
            int p = off ? -1 : atoi(argv[2]);
            if(!off && (p<50||p>100)){ fprintf(stderr,"battery: <50-100>|off|status  (Synapse range is 50-80)\n"); return 1; }
            int val = off ? 0x41 : (p|0x80);   // off = Synapse's disable byte (0x41 = 65 with bit7 clear -> optimizer off, charge to 100).
                                                // Can't derive it from get_charge_limit (0x8f returns a status byte, not the %), so use a fixed in-range value.
            int r=set_charge_limit(fd,val);
            printf("%s charge limit %s (wrote 0x%02x). Note: the EC readback can't confirm the %%; verify by watching charging stop near the limit.\n",
                   r==0?"ok":"FAILED", off?"disabled":argv[2], val);
        } else { fprintf(stderr,"battery [status | <50-100> | off]\n"); return 1; }
    } else if(!strcmp(argv[1],"powerd")&&argc==3){
        // toggle nvidia-powerd (Dynamic Boost daemon). off => lets dGPU reach D3cold (~0W).
        const char*a=argv[2];
        if(!strcmp(a,"status")){
            printf("nvidia-powerd: %s\n", system("/usr/bin/systemctl is-active --quiet nvidia-powerd")==0?"active":"inactive");
            char ps[48]="?\n",psp[300]; dgpu_path(psp,sizeof psp,"power_state"); FILE*f=fopen(psp,"r");
            if(f){ if(!fgets(ps,sizeof ps,f)) strcpy(ps,"?\n"); fclose(f);}
            else strcpy(ps,"removed (no-dGPU boot mode)\n");
            printf("dGPU power_state: %s", ps);
        } else if(!strcmp(a,"off")||!strcmp(a,"on")){
            // sudo-less via polkit (49-nvidia-powerd.rules); absolute path = don't trust inherited $PATH
            int r=system(!strcmp(a,"off") ? "/usr/bin/systemctl stop nvidia-powerd" : "/usr/bin/systemctl start nvidia-powerd");
            printf("nvidia-powerd %s -> %s\n", a, r==0?"ok":"failed (polkit rule missing?)");
        } else { fprintf(stderr,"powerd: on|off|status\n"); return 1; }
        } else if(!strcmp(argv[1],"fancurve")||!strcmp(argv[1],"autofan")){
        // Curve mode now runs inside razerctld: tell it once, then just watch. Unlike the old
        // foreground loop, Ctrl-C here does NOT revert to Auto -- the daemon keeps curving after
        // this process exits (that's the whole point of moving it server-side).
        if(set_fan_curve(fd)!=0){ fprintf(stderr,"failed to enable curve mode (razerctld unreachable?)\n"); return 1; }
        printf("fan curve enabled in razerctld (temp-driven, persists across reboots until changed).\n");
        printf("watching live setpoint @2s (Ctrl-C to stop watching -- curve keeps running in the background)...\n");
        for(;;){
            printf("\rfan1=%4d fan2=%4d rpm | setpoint=%-4d ",get_tach(fd,1),get_tach(fd,2),get_fan_setpoint(fd));
            fflush(stdout); sleep(2);
        }
    } else if(!strcmp(argv[1],"power")&&argc==3){
        // max: enable nvidia-powerd (Dynamic Boost) + raise TDP to card max.
        // save: disable powerd + reset TDP to driver default.
        // status: report powerd state + current/max/default power limits.
        const char*a=argv[2];
        if(!strcmp(a,"status")){
            printf("nvidia-powerd: %s\n", system("/usr/bin/systemctl is-active --quiet nvidia-powerd")==0?"active (Dynamic Boost ON)":"inactive");
            // GPU Ceiling Power Limit (current), max, and default via -q -d POWER
            FILE*f=popen("/usr/bin/nvidia-smi -q -d POWER 2>/dev/null","r");
            if(f){ char line[128];
                while(fgets(line,sizeof line,f)){
                    if(strstr(line," N/A")) continue; // skip unset base-power fields
                    if(strstr(line,"Current Power Limit")||strstr(line,"Max Power Limit")||strstr(line,"Default Power Limit")||strstr(line,"Average Power Draw"))
                        printf(" %s",line);
                } pclose(f); }
        } else if(!strcmp(a,"max")||!strcmp(a,"save")){
            // sudo-less via polkit (49-nvidia-powerd.rules). Both just toggle nvidia-powerd; the
            // dGPU ceiling is owned by powerd (max=up to 175W) or the driver default (save) which
            // re-applies on the dGPU's next D3cold sleep/wake. (No nvidia-smi -pl: needs root + flaky on laptop GPUs.)
            int rc;
            if(!strcmp(a,"max")){
                rc=system("/usr/bin/systemctl start nvidia-powerd");
                printf("power max: nvidia-powerd (Dynamic Boost) started -> %s\n",rc==0?"ok":"FAILED (polkit rule missing?)");
                printf("  powerd boosts the dGPU ceiling up to 175 W automatically under load.\n");
            } else {
                rc=system("/usr/bin/systemctl stop nvidia-powerd");
                printf("power save: nvidia-powerd stopped -> %s\n",rc==0?"ok":"FAILED (polkit rule missing?)");
                printf("  dGPU ceiling returns to the driver default on its next D3cold sleep/wake.\n");
            }
        } else { fprintf(stderr,"power: max|save|status\n"); return 1; }
    } else {
        printf("usage: razerctl [get | epp [0-255] | rpm | mode <balanced|gaming|creator> | battery [status|<50-100>|off] | fan <auto|RPM> | fancurve | kbd <white|red|purple|green|off> | powerd <on|off|status> | power <max|save|status> | uv <mV> <min> [max] | uv reset | nvtest]   (no args = TUI)\n");
    }
    close(fd); return 0;
}
